//! A friend's browser (S2): the microphone comes in (HRA1, HRA2 or HRA3 Opus), the Stream Mix as the DAW makes it goes
//! out (HRA2 / HRA3, never delayed by Line up). Port of `ShareServer::friendSession` and `friendSend`.
//!
//! HRA2 microphone packets say which Stream Mix position the friend was hearing when the packet's first frame was
//! captured; with that the Hub knows how late the voice is. Per session: this thread only reads (a slow line down must
//! never make the microphone wait), `send_loop` writes, and the WebRTC peer has its own thread (`rtcworker`).
use super::listener::push_pcm;
use super::rtcworker::{RtcHandle, RtcSink};
use super::sender::pcm_to_float;
use super::sock;
use super::util::{PongBudget, has, now_ms, parse_control, var_bool, var_string};
use super::ws::{self, MAGIC1, MAGIC2};
use super::{CountGuard, Ctx, FRIEND_MAX_LAG_MS, MAX_CONTROL_QUEUE, MAX_PCM_FRAMES, lock};
use crate::aec::AecMode;
use crate::opus::{App, OpusDec, OpusEnc};
use crate::wire::{
    CODEC_OPUS, CODEC_PCM16, FLAG_FEC, HRA3_HEADER_BYTES, Hra3Header, MAGIC3, parse_hra3, seq_gap,
    write_hra3,
};
use hs_bus::bus::{bits_float, float_bits};
use hs_bus::paced_send::{SendQueue, catch_up};
use hs_bus::protocol::{FriendState, kMaxFriends};
use hs_bus::ring::{ReadResult, ring_read_at, ring_write};
use hs_bus::shm::now_ns;
use std::net::TcpStream;
use std::sync::atomic::{AtomicBool, AtomicU8, Ordering};
use std::sync::{Arc, Mutex, OnceLock};
use std::time::Duration;

/// The longest Opus frame: 120 ms at 48 kHz. A friend's packet that claims more is not audio.
const MAX_OPUS_FRAMES: usize = 5760;
const RING_SCRATCH: usize = 4096;

/// What leaves for one friend: the writer thread fills and empties it, the reader thread adds pongs, the peer adds
/// signaling.
struct FriendOut {
    q: Mutex<SendQueue>,
    /// either thread: the session is finished
    over: AtomicBool,
    /// negotiated codec: `CODEC_OPUS` or `CODEC_PCM16`
    codec: AtomicU8,
    /// (3.5) "Hear myself from Hub"
    hear_myself: AtomicBool,
}

/// Decoder, jitter bookkeeping and delay measurement of the microphone path; one lock because the WebSocket reader and
/// the DataChannel thread both feed it.
struct AudioState {
    dec: OpusDec,
    dec_inited: bool,
    last_seq: u32,
    last_seq_valid: bool,
    /// recent delay readings (ms), for the median
    history: Vec<f64>,
    smooth: f64,
    last_mic: u32,
    l: Vec<f32>,
    r: Vec<f32>,
}

struct FriendCtx {
    ctx: Ctx,
    slot: usize,
    session: u32,
    out: FriendOut,
    audio: Mutex<AudioState>,
    rtc: OnceLock<Arc<RtcHandle>>,
}

impl RtcSink for FriendCtx {
    fn signal(&self, json: String) {
        if !self.out.over.load(Ordering::Acquire) {
            lock(&self.out.q).append(&ws::frame(1, json.as_bytes()));
        }
    }
    fn message(&self, data: &[u8]) {
        let net = &self.ctx.inner.friend_net[self.slot];
        net.up_bytes.fetch_add(data.len() as u64, Ordering::Relaxed);
        self.process_audio(data);
    }
    fn finished(&self) -> bool {
        self.out.over.load(Ordering::Acquire)
    }
}

fn median(v: &[f64]) -> f64 {
    let mut s = v.to_vec();
    s.sort_by(f64::total_cmp);
    s[s.len() / 2]
}

impl FriendCtx {
    fn current(&self) -> bool {
        self.ctx.inner.friend_gen[self.slot].load(Ordering::SeqCst) == self.session
    }

    /// Longest silence between two microphone packets.
    fn note_mic(&self, a: &mut AudioState) {
        let net = &self.ctx.inner.friend_net[self.slot];
        let t = now_ms();
        if a.last_mic != 0 && t.wrapping_sub(a.last_mic) as f32 > net.mic_gap.load() {
            net.mic_gap.store(t.wrapping_sub(a.last_mic) as f32);
        }
        a.last_mic = t;
    }

    /// The delay used to line the echo canceller's reference up: measured, else the round trip, else 80 ms.
    fn delay_ms(&self) -> f32 {
        let h = &self.ctx.bus.layout().friends.0[self.slot];
        let mut d = bits_float(h.delayBits.load(Ordering::Relaxed));
        if d < 0.0 {
            d = self.ctx.inner.friend_net[self.slot].rtt.load();
        }
        if d < 0.0 { 80.0 } else { d }
    }

    /// Echo cancellation in whole steps of the canceller's frame (5 ms: HAS Karaoke's packets; a browser's 10 ms
    /// ones are two steps); stereo input is then L on both sides, as the C++.
    fn cancel_echo(&self, a: &mut AudioState, frames: usize, stereo: bool, delay_samples: i32) {
        let mut echo = lock(&self.ctx.inner.friend_echo[self.slot]);
        let step = echo.frame_size();
        let (mut rem, mut off) = (frames, 0);
        while step > 0 && rem >= step {
            echo.process_in_place(&mut a.l[off..off + step], delay_samples);
            if stereo {
                let (l, r) = (&a.l[off..off + step], &mut a.r[off..off + step]);
                r.copy_from_slice(l);
            }
            off += step;
            rem -= step;
        }
    }

    /// One delay reading (ms): median of the last few, smoothed, published to the Hub.
    fn measure(&self, a: &mut AudioState, ms: f64) {
        if !(0.0..3000.0).contains(&ms) {
            return;
        }
        a.history.push(ms);
        if a.history.len() > 41 {
            a.history.remove(0);
        }
        if a.history.len() >= 8 {
            let med = median(&a.history);
            a.smooth = if a.smooth < 0.0 {
                med
            } else {
                a.smooth + (med - a.smooth) * 0.15
            };
            let h = &self.ctx.bus.layout().friends.0[self.slot];
            h.delayBits
                .store(float_bits(a.smooth as f32), Ordering::Relaxed);
        }
    }

    /// Writes the friend's voice into the bus unless a newer connection of this friend took the slot.
    /// `false` = taken over, drop everything.
    fn write_ring(
        &self,
        l: &[f32],
        r: &[f32],
        frames: usize,
        rate: u32,
        peak: Option<f32>,
    ) -> bool {
        let layout = self.ctx.bus.layout();
        let h = &layout.friends.0[self.slot];
        let _w = lock(&self.ctx.inner.friend_write[self.slot]);
        if !self.current() {
            return false;
        }
        if let Some(pk) = peak {
            h.sampleRate.store(rate, Ordering::Relaxed);
            ring_write(
                &layout.friendAudio.0[self.slot],
                &h.writePos.0,
                &[l, r],
                frames as u32,
            );
            h.heartbeatNs.store(now_ns(), Ordering::Relaxed);
            h.peakBits.store(float_bits(pk), Ordering::Relaxed);
        } else {
            // concealed frames: audio only
            ring_write(
                &layout.friendAudio.0[self.slot],
                &h.writePos.0,
                &[l, r],
                frames as u32,
            );
        }
        true
    }

    /// A binary message from the friend (WebSocket or DataChannel): HRA3 Opus, or HRA1 / HRA2 PCM16.
    fn process_audio(&self, data: &[u8]) {
        let mut guard = lock(&self.audio);
        if !self.current() {
            return;
        }
        let a: &mut AudioState = &mut guard;
        if let Some((h3, payload)) = parse_hra3(data) {
            self.out.codec.store(h3.codec, Ordering::Relaxed);
            if h3.codec == CODEC_OPUS {
                self.opus_packet(a, &h3, payload);
            }
        } else {
            self.pcm_packet(a, data);
        }
    }

    fn opus_packet(&self, a: &mut AudioState, h3: &Hra3Header, payload: &[u8]) {
        let frames = usize::from(h3.frames);
        // C++ bug: `frames` up to 65535 drove ten concealment writes of that size into the friend's ring
        if frames == 0 || frames > MAX_OPUS_FRAMES {
            return;
        }
        let chans = usize::from(h3.chans);
        if !a.dec_inited || a.dec.channels() != chans as i32 {
            a.dec.init(48000, chans as i32);
            a.dec_inited = true;
            a.last_seq = h3.seq;
            a.last_seq_valid = true;
        } else if a.last_seq_valid && seq_gap(h3.seq, a.last_seq) < 0 {
            return; // late or repeated (the DataChannel is unordered): its place has already been played
        } else if a.last_seq_valid && seq_gap(h3.seq, a.last_seq) > 0 {
            let missed = seq_gap(h3.seq, a.last_seq).min(10) as u32;
            a.l.resize(frames, 0.0);
            a.r.resize(frames, 0.0);
            for m in 0..missed {
                let AudioState { dec, l, r, .. } = &mut *a;
                let mut ch: [&mut [f32]; 2] = [&mut l[..frames], &mut r[..frames]];
                // the packet just before this one comes back from this packet's in-band FEC, older ones are concealed
                let fec = m + 1 == missed && dec.decode(Some(payload), &mut ch, frames, true) > 0;
                if !fec {
                    dec.decode_plc(&mut ch, frames);
                }
                let (l, r) = (&a.l, &a.r);
                let src_r = if chans == 2 {
                    &r[..frames]
                } else {
                    &l[..frames]
                };
                if !self.write_ring(&l[..frames], src_r, frames, 48000, None) {
                    break;
                }
            }
        }
        a.last_seq = h3.seq;
        a.last_seq_valid = true;

        a.l.resize(frames, 0.0);
        a.r.resize(frames, 0.0);
        let decoded = {
            let AudioState { dec, l, r, .. } = &mut *a;
            let mut ch: [&mut [f32]; 2] = [&mut l[..frames], &mut r[..frames]];
            dec.decode(Some(payload), &mut ch, frames, false)
        };
        if decoded <= 0 {
            return;
        }
        let dec_frames = (decoded as usize).min(frames);
        let delay_samples = (self.delay_ms() * 48.0) as i32;
        self.cancel_echo(a, dec_frames, chans == 2, delay_samples);
        self.note_mic(a);
        let stereo = chans == 2;
        let mut pk = 0.0f32;
        for i in 0..dec_frames {
            pk = pk.max(a.l[i].abs());
            if stereo {
                pk = pk.max(a.r[i].abs());
            }
        }
        let layout = self.ctx.bus.layout();
        let w = layout.friendMixWrite.0.load(Ordering::Acquire);
        let hub_rate = layout
            .streamHeader
            .sampleRate
            .load(Ordering::Relaxed)
            .max(8000);
        let src_r = if stereo {
            &a.r[..dec_frames]
        } else {
            &a.l[..dec_frames]
        };
        if !self.write_ring(&a.l[..dec_frames], src_r, dec_frames, 48000, Some(pk)) {
            return;
        }
        if h3.pos != 0 && w >= h3.pos {
            let ms = (w - h3.pos) as f64 * 1000.0 / f64::from(hub_rate)
                - dec_frames as f64 * 500.0 / 48000.0;
            self.measure(a, ms);
        }
    }

    fn pcm_packet(&self, a: &mut AudioState, data: &[u8]) {
        let two = data.len() >= 4 && data[..4] == MAGIC2.to_le_bytes();
        let hdr = if two { 24 } else { 16 };
        if data.len() < hdr {
            return;
        }
        let word =
            |at: usize| u32::from_le_bytes([data[at], data[at + 1], data[at + 2], data[at + 3]]);
        let (magic, rate, frames) = (word(0), word(4), word(12) as usize);
        let chans = usize::from(u16::from_le_bytes([data[8], data[9]]));
        let echo = if two {
            u64::from_le_bytes(data[16..24].try_into().unwrap_or([0; 8]))
        } else {
            0
        };
        if !(magic == MAGIC1 || magic == MAGIC2)
            || !(1..=2).contains(&chans)
            || !(8000..=192_000).contains(&rate)
        {
            return;
        }
        let frames = frames.min((data.len() - hdr) / (2 * chans));
        if frames > MAX_PCM_FRAMES {
            return; // not audio: a page sends ~10 ms
        }
        self.note_mic(a);
        {
            let AudioState { l, r, .. } = &mut *a;
            pcm_to_float(&data[hdr..], chans, frames, l, r);
        }
        let delay_samples = (self.delay_ms() * (rate as f32 / 1000.0)) as i32;
        self.cancel_echo(a, frames, chans == 2, delay_samples);
        let mut pk = 0.0f32;
        for i in 0..frames {
            pk = pk.max(a.l[i].abs().max(a.r[i].abs()));
        }
        let layout = self.ctx.bus.layout();
        let w = layout.friendMixWrite.0.load(Ordering::Acquire);
        let hub_rate = layout
            .streamHeader
            .sampleRate
            .load(Ordering::Relaxed)
            .max(8000);
        if !self.write_ring(&a.l, &a.r, frames, rate, Some(pk)) {
            return;
        }
        if echo != 0 && w >= echo && frames > 0 {
            let ms = (w - echo) as f64 * 1000.0 / f64::from(hub_rate)
                - frames as f64 * 500.0 / f64::from(rate);
            self.measure(a, ms);
        }
    }
}

/// The value after `{"pong":`, like `strtoull` (leading blanks, digits, stops at the first other character).
fn pong_value(msg: &[u8]) -> Option<u32> {
    let rest = msg.strip_prefix(b"{\"pong\":")?;
    let text = std::str::from_utf8(rest).ok()?.trim_start();
    let digits: String = text.chars().take_while(char::is_ascii_digit).collect();
    // no number at all is not a pong (C++: strtoull gave 0, a huge "round trip" that was then dropped anyway);
    // a number that is too long wraps like the C++ `uint32_t(strtoull(...))`
    if digits.is_empty() {
        return None;
    }
    Some(digits.parse::<u64>().unwrap_or(u64::MAX) as u32)
}

pub(crate) fn friend_session(ctx: &Ctx, s: &TcpStream, slot: usize, id: u32, token: &str) {
    let _mmcss = sock::Mmcss::new("Audio");
    let inner = &ctx.inner;
    let layout = ctx.bus.layout();
    let h = &layout.friends.0[slot];
    let net = &inner.friend_net[slot];
    // The newest connection of this friend wins. The bump and the Live store are one step under `friend_write`, like the
    // check-and-store at the end of the older session: it can no longer store Offline after this Live (C++ bug).
    let session = {
        let _w = lock(&inner.friend_write[slot]);
        let session = inner.friend_gen[slot].fetch_add(1, Ordering::SeqCst) + 1;
        inner.friend_kick[slot].store(false, Ordering::SeqCst);
        inner.friend_remeasure[slot].store(false, Ordering::SeqCst);
        h.sampleRate.store(48000, Ordering::Relaxed);
        h.delayBits.store(float_bits(-1.0), Ordering::Relaxed); // not measured yet
        h.heartbeatNs.store(now_ns(), Ordering::Relaxed);
        h.state.store(FriendState::kFriendLive, Ordering::Release);
        session
    };
    let _live = CountGuard::new(&inner.friends_live);
    net.reset();
    sock::limit_send_buffer(s);
    lock(&inner.friend_echo[slot]).init(48000, 240, 200); // 5 ms steps: the size of HAS Karaoke's packets

    let fctx = Arc::new(FriendCtx {
        ctx: ctx.clone(),
        slot,
        session,
        out: FriendOut {
            q: Mutex::new(SendQueue::new()),
            over: AtomicBool::new(false),
            codec: AtomicU8::new(CODEC_PCM16),
            hear_myself: AtomicBool::new(false),
        },
        audio: Mutex::new(AudioState {
            dec: OpusDec::new(),
            dec_inited: false,
            last_seq: 0,
            last_seq_valid: false,
            history: Vec::new(),
            smooth: -1.0,
            last_mic: 0,
            l: Vec::new(),
            r: Vec::new(),
        }),
        rtc: OnceLock::new(),
    });
    let mut closing = false;
    // hello v3: tells the friend who they are and which codecs are supported
    {
        let (name, host) = inner
            .friend_slot_for(token)
            .map(|f| (f.2, f.3))
            .unwrap_or_default();
        let hello = format!(
            "{{\"v\": 3, \"name\": {}, \"host\": {}, \"codecs\": [\"opus\", \"pcm16\"], \"webrtc\": true}}",
            ws::json_str(&name),
            ws::json_str(&host)
        );
        if !ws::send_frame(s, 1, hello.as_bytes()) {
            closing = true;
        }
    }
    let mut worker = if closing {
        None
    } else {
        inner.start_rtc(fctx.clone())
    };
    if let Some(w) = &worker {
        let _ = fctx.rtc.set(w.handle());
    }

    let alive = || {
        inner.is_running()
            && !fctx.out.over.load(Ordering::Acquire)
            && fctx.current()
            && !inner.friend_kick[slot].load(Ordering::SeqCst)
    };
    std::thread::scope(|scope| {
        let writer = scope.spawn(|| send_loop(&fctx, s));
        read_loop(&fctx, s, token, id, closing, &alive);
        fctx.out.over.store(true, Ordering::Release);
        if let Some(w) = worker.as_mut() {
            w.shutdown();
        }
        let _ = writer.join();
    });

    let _w = lock(&inner.friend_write[slot]);
    if fctx.current() || inner.friend_kick[slot].load(Ordering::SeqCst) {
        // not taken over by a newer connection
        if h.id.load(Ordering::SeqCst) == id
            && h.state.load(Ordering::SeqCst) == FriendState::kFriendLive
        {
            h.state
                .store(FriendState::kFriendOffline, Ordering::Release);
        }
        h.peakBits.store(0, Ordering::Relaxed);
    }
}

/// The microphone: everything the friend's browser sends over the WebSocket.
fn read_loop(
    f: &Arc<FriendCtx>,
    s: &TcpStream,
    token: &str,
    id: u32,
    closing: bool,
    alive: &dyn Fn() -> bool,
) {
    let inner = &f.ctx.inner;
    let slot = f.slot;
    let net = &inner.friend_net[slot];
    let h = &f.ctx.bus.layout().friends.0[slot];
    let mut msg: Vec<u8> = Vec::new();
    let mut rtts: Vec<f32> = Vec::new(); // recent round trips (ms)
    let mut pongs = PongBudget::default();
    let mut last_check = now_ms();
    while !closing && alive() {
        let now = now_ms();
        if now.wrapping_sub(last_check) >= 250 {
            // now and then: is this link still this friend's?
            last_check = now;
            match inner.friend_slot_for(token) {
                Some((sl, cur, _, _)) if sl == slot && cur == id => {}
                _ => break,
            }
        }
        if inner.friend_remeasure[slot].swap(false, Ordering::SeqCst) {
            let mut a = lock(&f.audio);
            a.history.clear();
            a.smooth = -1.0;
            h.delayBits.store(float_bits(-1.0), Ordering::Relaxed);
        }
        // ---- microphone ----------------------------------------------------------------------------
        let ready = sock::wait_readable(s, 20);
        if ready < 0 {
            break;
        }
        if ready != 1 {
            continue;
        }
        // the browser's own ping: the answer goes out with the rest
        let mut on_ping = |p: &[u8]| {
            pongs.push(&mut lock(&f.out.q), &ws::frame(0xA, p), MAX_CONTROL_QUEUE);
        };
        let ping: &mut dyn FnMut(&[u8]) = &mut on_ping;
        let op = ws::read_message(s, &mut msg, 5000, alive, Some(ping));
        if op == 0 || op == 8 {
            break;
        }
        net.up_bytes.fetch_add(msg.len() as u64, Ordering::Relaxed);
        match op {
            1 if msg.len() > 8 && msg.len() < 40 && msg.starts_with(b"{\"pong\":") => {
                // the page answers the writer's ping
                if let Some(sent) = pong_value(&msg) {
                    let rtt = now_ms().wrapping_sub(sent);
                    if rtt < 10000 {
                        rtts.push(rtt as f32);
                        if rtts.len() > 5 {
                            rtts.remove(0);
                        }
                        let mut sorted = rtts.clone();
                        sorted.sort_by(f32::total_cmp);
                        net.rtt.store(sorted[sorted.len() / 2]);
                    }
                }
            }
            1 => control(f, &msg),
            2 => f.process_audio(&msg),
            _ => {}
        }
    }
}

/// Client control messages e.g. `{"codec":"opus"}`, `{"hearMyself":true}`, `{"aec":"on"}`, signaling.
fn control(f: &FriendCtx, text: &[u8]) {
    let Some(j) = parse_control(text) else { return };
    if has(&j, "type") {
        let rtc = f.rtc.get();
        match var_string(&j["type"]).as_str() {
            "rtc_answer" if has(&j, "sdp") => {
                if let Some(h) = rtc {
                    h.answer(var_string(&j["sdp"]));
                }
            }
            "rtc_candidate" if has(&j, "candidate") => {
                if let Some(h) = rtc {
                    let mid = if has(&j, "mid") {
                        var_string(&j["mid"])
                    } else {
                        String::new()
                    };
                    h.candidate(var_string(&j["candidate"]), mid);
                }
            }
            _ => {}
        }
    }
    if has(&j, "codec") {
        match var_string(&j["codec"]).as_str() {
            "opus" => f.out.codec.store(CODEC_OPUS, Ordering::Relaxed),
            "pcm16" => f.out.codec.store(CODEC_PCM16, Ordering::Relaxed),
            _ => {}
        }
    }
    if has(&j, "hearMyself") {
        f.out
            .hear_myself
            .store(var_bool(&j["hearMyself"]), Ordering::Relaxed);
    }
    let set_mode = |m| lock(&f.ctx.inner.friend_echo[f.slot]).set_mode(m);
    if has(&j, "speakerMode") && var_bool(&j["speakerMode"]) {
        set_mode(AecMode::On);
    }
    if has(&j, "aec") {
        match var_string(&j["aec"]).as_str() {
            "on" => set_mode(AecMode::On),
            "off" => set_mode(AecMode::Off),
            "auto" => set_mode(AecMode::Auto),
            _ => {}
        }
    }
}

/// What the friend hears: the Stream Mix as the DAW makes it + every other friend (like Jamulus: each voice as soon as
/// it arrives; never their own). Never waits for the line: what the socket does not take is not queued, the Hub jumps
/// ahead instead (`SendQueue`, `catch_up`). Also: the delay for the friend's screen and the ping.
fn send_loop(f: &FriendCtx, s: &TcpStream) {
    let _mmcss = sock::Mmcss::new("Audio");
    let inner = &f.ctx.inner;
    let slot = f.slot;
    let layout = f.ctx.bus.layout();
    let h = &layout.friends.0[slot];
    let net = &inner.friend_net[slot];
    let mut ml = vec![0.0f32; RING_SCRATCH];
    let mut mr = vec![0.0f32; RING_SCRATCH];
    let mut mono = vec![0.0f32; RING_SCRATCH];
    // the other friends (friendVoiceAudio), same positions as ml / mr
    let mut voices: Vec<Vec<f32>> = (0..kMaxFriends).map(|_| vec![0.0; RING_SCRATCH]).collect();
    let (mut packet, mut framed) = (Vec::<u8>::new(), Vec::<u8>::new());
    let mut enc = OpusEnc::new();
    let mut opus_ready = false;
    let mut down_seq = 0u32;
    let mw0 = layout.friendMixWrite.0.load(Ordering::SeqCst);
    let mut mix_cursor = if mw0 > 2400 { mw0 - 960 } else { 0 };
    let send_now = |p: &[u8]| {
        let w = sock::try_send(s, p);
        if w > 0 {
            net.down_bytes.fetch_add(w as u64, Ordering::Relaxed);
        }
        w
    };
    let mut last_stat = now_ms();
    let mut last_info = last_stat;
    let (mut prev_up, mut prev_down) = (0u64, 0u64);
    let mut control_due = false; // delay + ping wait for an empty queue, and are only counted as sent when they were queued
    let mut blocked; // the socket still holds a packet
    while !f.out.over.load(Ordering::Acquire)
        && inner.is_running()
        && f.current()
        && !inner.friend_kick[slot].load(Ordering::SeqCst)
    {
        let now = now_ms();
        if now.wrapping_sub(last_stat) >= 1000 {
            // once a second: kbps
            let secs = now.wrapping_sub(last_stat) as f32 / 1000.0;
            last_stat = now;
            let up = net.up_bytes.load(Ordering::Relaxed);
            let down = net.down_bytes.load(Ordering::Relaxed);
            net.up
                .store(up.wrapping_sub(prev_up) as f32 * 8.0 / 1000.0 / secs);
            net.down
                .store(down.wrapping_sub(prev_down) as f32 * 8.0 / 1000.0 / secs);
            prev_up = up;
            prev_down = down;
        }
        if now.wrapping_sub(last_info) >= 1000 {
            control_due = true; // how late the friend is (their screen) and the ping (round trip)
        }
        {
            let mut q = lock(&f.out.q);
            if !q.flush(send_now) {
                break;
            }
            if control_due && q.idle() {
                // as soon as the queue is empty, before the next audio packet: a bad line needs these most
                let mut delay = bits_float(h.delayBits.load(Ordering::Relaxed));
                if !delay.is_finite() {
                    delay = -1.0;
                }
                q.append(&ws::frame(
                    1,
                    format!("{{\"delay\":{delay:.1}}}").as_bytes(),
                ));
                q.append(&ws::frame(1, format!("{{\"ping\":{now}}}").as_bytes()));
                control_due = false;
                last_info = now;
                if !q.flush(send_now) {
                    break;
                }
            }
            blocked = !q.idle();
            inner.friend_queue_bytes[slot].store(q.stored(), Ordering::Relaxed);
        }

        let hub_rate = layout
            .streamHeader
            .sampleRate
            .load(Ordering::Relaxed)
            .max(8000);
        let mw = layout.friendMixWrite.0.load(Ordering::Acquire);
        if mw < mix_cursor {
            mix_cursor = mw; // Hub restarted
        }
        let was = mix_cursor;
        mix_cursor = if blocked {
            catch_up(mix_cursor, mw, hub_rate, FRIEND_MAX_LAG_MS, 30)
        } else {
            catch_up(mix_cursor, mw, hub_rate, 2000, 100) // fell far behind: jump to now
        };
        if blocked && mix_cursor != was {
            net.skips.fetch_add(1, Ordering::Relaxed);
        }
        let lag_ms = (mw - mix_cursor) as f32 * 1000.0 / hub_rate as f32;
        if lag_ms > net.lag.load() {
            net.lag.store(lag_ms);
        }

        let cur_codec = f.out.codec.load(Ordering::Relaxed);
        if cur_codec == CODEC_OPUS
            && !opus_ready
            && matches!(hub_rate, 48000 | 24000 | 16000 | 12000 | 8000)
            && enc.init(hub_rate as i32, 2, App::Audio)
        {
            enc.set_bitrate(128000); // 128 kbps stereo
            enc.set_complexity(5);
            enc.set_inband_fec(true);
            enc.set_packet_loss_perc(5);
            opus_ready = true;
        }

        // Opus in 5 ms packets: half the packet wait and half the receiver's packet step of 10 ms ones
        let opus_frames = u64::from(hub_rate / 200);
        while !blocked && mw - mix_cursor >= opus_frames && inner.is_running() {
            let use_opus = cur_codec == CODEC_OPUS && opus_ready;
            let n = if use_opus {
                opus_frames as usize
            } else {
                (mw - mix_cursor).min(RING_SCRATCH as u64) as usize
            };
            if n == 0 || n > RING_SCRATCH {
                break;
            }
            {
                let mut dst: [&mut [f32]; 2] = [&mut ml[..n], &mut mr[..n]];
                if ring_read_at(
                    &layout.friendMixAudio.0,
                    &layout.friendMixWrite.0,
                    mix_cursor,
                    &mut dst,
                    n as u32,
                    None,
                ) == ReadResult::Overrun
                {
                    mix_cursor = mw.saturating_sub(u64::from(hub_rate / 10));
                    break;
                }
            }
            for (k, [v0, v1]) in voices.as_chunks_mut::<2>().0.iter_mut().enumerate() {
                let mut vd: [&mut [f32]; 2] = [&mut v0[..n], &mut v1[..n]];
                // a gap = silence
                ring_read_at(
                    &layout.friendVoiceAudio.0[k],
                    &layout.friendVoiceWrite.0[k],
                    mix_cursor,
                    &mut vd,
                    n as u32,
                    None,
                );
            }
            let hear_myself = f.out.hear_myself.load(Ordering::Relaxed);
            for (j, v) in voices.iter().enumerate() {
                if j == slot && !hear_myself {
                    continue; // nobody hears themselves come back unless opted in
                }
                for i in 0..n {
                    ml[i] += v[i];
                    mr[i] += v[i];
                }
            }
            for i in 0..n {
                mono[i] = 0.5 * (ml[i] + mr[i]);
            }
            lock(&inner.friend_echo[slot]).push_reference(&mono[..n]);

            let mut opus_packet = false;
            if use_opus {
                let mut payload = [0u8; 1275];
                let bytes = enc.encode(&[&ml[..n], &mr[..n]], n, &mut payload);
                if bytes > 0 {
                    let hd = Hra3Header {
                        magic: MAGIC3,
                        codec: CODEC_OPUS,
                        chans: 2,
                        flags: FLAG_FEC,
                        seq: down_seq,
                        pos: mix_cursor,
                        frames: n as u16,
                    };
                    down_seq = down_seq.wrapping_add(1);
                    packet.resize(HRA3_HEADER_BYTES + bytes as usize, 0);
                    write_hra3(&mut packet, &hd, &payload[..bytes as usize]);
                    opus_packet = true;
                }
            }
            if !opus_packet {
                // PCM16 (also the fallback on an encode error): HRA2, 24-byte header with the timeline position
                packet.clear();
                packet.extend_from_slice(&MAGIC2.to_le_bytes());
                packet.extend_from_slice(&hub_rate.to_le_bytes());
                packet.extend_from_slice(&2u16.to_le_bytes());
                packet.extend_from_slice(&0u16.to_le_bytes());
                packet.extend_from_slice(&(n as u32).to_le_bytes());
                packet.extend_from_slice(&mix_cursor.to_le_bytes());
                push_pcm(&mut packet, &ml, &mr, n);
            }
            // PCM has no sequence: ordered WebSocket
            if opus_packet && let Some(rtc) = f.rtc.get().filter(|r| r.is_open()) {
                rtc.send(&packet);
                net.down_bytes
                    .fetch_add(packet.len() as u64, Ordering::Relaxed);
                mix_cursor += n as u64;
            } else {
                ws::frame_into(&mut framed, 2, &packet);
                let mut q = lock(&f.out.q);
                if !q.offer(&framed) {
                    break; // the browser's ping answer is still going out: next round
                }
                mix_cursor += n as u64;
                if !q.flush(send_now) {
                    f.out.over.store(true, Ordering::Release);
                    break;
                }
                blocked = !q.idle();
                inner.friend_queue_bytes[slot].store(q.stored(), Ordering::Relaxed);
            }
        }
        if blocked {
            sock::wait_writable(s, 5);
        } else {
            std::thread::sleep(Duration::from_millis(2));
        }
    }
    f.out.over.store(true, Ordering::Release);
    if f.current() {
        // (a session that was taken over must not zero the live one's figure)
        inner.friend_queue_bytes[slot].store(0, Ordering::Relaxed);
    }
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn pong_text_is_read_like_strtoull_but_needs_a_number() {
        assert_eq!(pong_value(b"{\"pong\":123456}"), Some(123_456));
        assert_eq!(pong_value(b"{\"pong\": 7}"), Some(7));
        assert_eq!(pong_value(b"{\"pong\":}"), None);
        assert_eq!(pong_value(b"{\"pong\":x1}"), None);
        assert_eq!(pong_value(b"{\"ping\":1}"), None);
        // wraps like the C++ u32 cast of a u64
        assert_eq!(pong_value(b"{\"pong\":4294967297}"), Some(1));
    }
}
