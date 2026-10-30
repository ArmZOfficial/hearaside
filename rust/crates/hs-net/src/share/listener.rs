//! A listener's browser: the Stream Mix (Hub output 0) as it is written, in ~10 ms packets. Port of
//! `ShareServer::streamTo`. A line that cannot keep up gets no queue: the packet that waits is the only one, and the
//! newest audio is sent when it is out (`SendQueue`).
use super::rtcworker::{RtcHandle, RtcSink, RtcWorker};
use super::sock;
use super::util::{PongBudget, has, parse_control, var_int, var_string};
use super::ws::{self, MAGIC1};
use super::{Ctx, LISTEN_MAX_LAG_MS, MAX_CONTROL_QUEUE, lock};
use crate::opus::{App, OpusEnc};
use crate::wire::{
    CODEC_OPUS, CODEC_PCM16, FLAG_NONE, HRA3_HEADER_BYTES, Hra3Header, MAGIC3, write_hra3,
};
use hs_bus::paced_send::{SendQueue, catch_up};
use hs_bus::ring::{ReadResult, ring_read_at};
use std::net::TcpStream;
use std::sync::atomic::{AtomicBool, Ordering};
use std::sync::{Arc, Mutex};

const HELLO: &[u8] = b"{\"v\":3,\"codecs\":[\"opus\",\"pcm16\"],\"webrtc\":true}";
/// An Opus listener whose loop ran late catches up with up to this many 10 ms packets per round.
const OPUS_BURST: usize = 8;

/// Interleaved 16-bit little-endian stereo after whatever header `packet` already holds.
pub(crate) fn push_pcm(packet: &mut Vec<u8>, l: &[f32], r: &[f32], n: usize) {
    for i in 0..n {
        for v in [l[i], r[i]] {
            let s = (v.clamp(-1.0, 1.0) * 32767.0).round() as i16;
            packet.extend_from_slice(&s.to_le_bytes());
        }
    }
}

/// HRA1: magic, rate, channels (2), 0, frames; then the PCM.
pub(crate) fn hra1_packet(packet: &mut Vec<u8>, rate: u32, l: &[f32], r: &[f32], n: usize) {
    packet.clear();
    packet.extend_from_slice(&MAGIC1.to_le_bytes());
    packet.extend_from_slice(&rate.to_le_bytes());
    packet.extend_from_slice(&2u16.to_le_bytes());
    packet.extend_from_slice(&0u16.to_le_bytes());
    packet.extend_from_slice(&(n as u32).to_le_bytes());
    push_pcm(packet, l, r, n);
}

struct ListenSink {
    q: Mutex<SendQueue>,
    over: AtomicBool,
}

impl RtcSink for ListenSink {
    fn signal(&self, json: String) {
        // the peer runs on its own thread (C++ bug: its callbacks appended to the queue without a lock)
        if !self.over.load(Ordering::Acquire) {
            lock(&self.q).append(&ws::frame(1, json.as_bytes()));
        }
    }
    fn message(&self, _data: &[u8]) {}
    fn finished(&self) -> bool {
        self.over.load(Ordering::Acquire)
    }
}

enum Step {
    /// nothing to send now
    Skip,
    Sent,
    Gone,
}

struct Streamer<'a> {
    ctx: &'a Ctx,
    s: &'a TcpStream,
    sink: &'a ListenSink,
    rtc: Option<Arc<RtcHandle>>,
    l: Vec<f32>,
    r: Vec<f32>,
    packet: Vec<u8>,
    framed: Vec<u8>,
    enc: OpusEnc,
    opus_ready: bool,
    codec: u8,
    bitrate: i32,
    seq: u32,
}

impl Streamer<'_> {
    fn send_now(s: &TcpStream) -> impl FnMut(&[u8]) -> isize + '_ {
        move |p| sock::try_send(s, p)
    }

    /// Reads the next packet's audio from the ring at `cursor` (`w` = write position now, `w > cursor`) and sends it.
    fn produce(&mut self, cursor: &mut u64, w: u64, rate: u32) -> Step {
        let layout = self.ctx.bus.layout();
        let wp = &layout.streamHeader.writePos.0[0];
        if self.codec == CODEC_OPUS
            && !self.opus_ready
            && matches!(rate, 48000 | 24000 | 16000 | 12000 | 8000)
            && self.enc.init(rate as i32, 2, App::Audio)
        {
            self.enc.set_bitrate(self.bitrate);
            self.enc.set_complexity(5);
            self.opus_ready = true;
        }
        let use_opus = self.codec == CODEC_OPUS && self.opus_ready;
        let step = rate / 100;
        let avail = w - *cursor;
        let n = if use_opus {
            step
        } else {
            avail.min(self.l.len() as u64) as u32
        };
        if (!use_opus && n < rate / 200) || (use_opus && avail < u64::from(step)) {
            return Step::Skip; // wait for ~5 ms of audio
        }
        let (l, r) = (&mut self.l[..], &mut self.r[..]);
        let mut dst: [&mut [f32]; 2] = [&mut l[..n as usize], &mut r[..n as usize]];
        if ring_read_at(&layout.streamAudio.0[0], wp, *cursor, &mut dst, n, None)
            == ReadResult::Overrun
        {
            *cursor = wp
                .load(Ordering::SeqCst)
                .saturating_sub(u64::from(rate / 10));
            return Step::Skip;
        }
        *cursor += u64::from(n);
        let n = n as usize;
        let mut opus_packet = false;
        if use_opus {
            let mut payload = [0u8; 1275];
            let bytes = self
                .enc
                .encode(&[&self.l[..n], &self.r[..n]], n, &mut payload);
            if bytes > 0 {
                let h = Hra3Header {
                    magic: MAGIC3,
                    codec: CODEC_OPUS,
                    chans: 2,
                    flags: FLAG_NONE,
                    seq: self.seq,
                    pos: *cursor,
                    frames: n as u16,
                };
                self.seq = self.seq.wrapping_add(1);
                self.packet.resize(HRA3_HEADER_BYTES + bytes as usize, 0);
                write_hra3(&mut self.packet, &h, &payload[..bytes as usize]);
                opus_packet = true;
            }
        }
        if !opus_packet {
            hra1_packet(&mut self.packet, rate, &self.l, &self.r, n);
        }
        // only Opus (HRA3) packets carry a sequence number, so only they may take the unordered DataChannel
        if opus_packet && self.rtc.as_ref().is_some_and(|h| h.is_open()) {
            if let Some(h) = &self.rtc {
                h.send(&self.packet);
            }
        } else {
            ws::frame_into(&mut self.framed, 2, &self.packet);
            let mut q = lock(&self.sink.q);
            q.offer(&self.framed);
            if !q.flush(Self::send_now(self.s)) {
                return Step::Gone;
            }
        }
        Step::Sent
    }

    /// `rtc_answer` / `rtc_candidate` / `codec` / `bitrate` from the page.
    fn control(&mut self, text: &[u8]) {
        let Some(j) = parse_control(text) else { return };
        if has(&j, "type") {
            match var_string(&j["type"]).as_str() {
                "rtc_answer" if has(&j, "sdp") => {
                    if let Some(h) = &self.rtc {
                        h.answer(var_string(&j["sdp"]));
                    }
                }
                "rtc_candidate" if has(&j, "candidate") => {
                    if let Some(h) = &self.rtc {
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
                "opus" => {
                    self.codec = CODEC_OPUS;
                    if has(&j, "bitrate") {
                        self.bitrate = var_int(&j["bitrate"]).clamp(32000, 320000);
                        if self.opus_ready {
                            self.enc.set_bitrate(self.bitrate);
                        }
                    }
                }
                "pcm16" => self.codec = CODEC_PCM16,
                _ => {}
            }
        }
    }
}

pub(crate) fn stream_to(ctx: &Ctx, s: &TcpStream) {
    let inner = &ctx.inner;
    let layout = ctx.bus.layout();
    let sh = &layout.streamHeader;
    sock::limit_send_buffer(s);
    let sink = Arc::new(ListenSink {
        q: Mutex::new(SendQueue::new()),
        over: AtomicBool::new(false),
    });
    lock(&sink.q).append(&ws::frame(1, HELLO));
    let mut worker = inner.start_rtc(sink.clone());
    let mut st = Streamer {
        ctx,
        s,
        sink: &sink,
        rtc: worker.as_ref().map(RtcWorker::handle),
        l: vec![0.0; 4096],
        r: vec![0.0; 4096],
        packet: Vec::new(),
        framed: Vec::new(),
        enc: OpusEnc::new(),
        opus_ready: false,
        codec: CODEC_PCM16,
        bitrate: 128000,
        seq: 0,
    };
    let alive = || inner.is_running();
    let mut incoming: Vec<u8> = Vec::new();
    let mut pongs = PongBudget::default();
    let mut cursor = sh.writePos.0[0].load(Ordering::SeqCst);
    let mut idle = 0u32;
    'session: while inner.is_running() {
        let wait = if lock(&sink.q).idle() { 10 } else { 2 };
        match sock::wait_readable(s, wait) {
            1 => {
                // the browser closed, pinged or sent a control message
                let mut on_ping = |p: &[u8]| {
                    pongs.push(&mut lock(&sink.q), &ws::frame(0xA, p), MAX_CONTROL_QUEUE);
                };
                let ping: &mut dyn FnMut(&[u8]) = &mut on_ping;
                let op = ws::read_message(s, &mut incoming, 1000, &alive, Some(ping));
                if op == 8 || op == 0 {
                    break;
                }
                if op == 1 {
                    st.control(&incoming);
                }
            }
            r if r < 0 => break,
            _ => {}
        }
        if !lock(&sink.q).flush(Streamer::send_now(s)) {
            break;
        }
        let rate = sh.sampleRate.load(Ordering::SeqCst).max(8000);
        let w = sh.writePos.0[0].load(Ordering::Acquire);
        if sh.active.load(Ordering::SeqCst) == 0 || w == cursor {
            cursor = w;
            idle = idle.wrapping_add(1);
            if idle.is_multiple_of(100) {
                // keep the connection (and the tunnel) alive while the DAW is quiet
                let mut hdr = [0u8; 16];
                hdr[..4].copy_from_slice(&MAGIC1.to_le_bytes());
                hdr[4..8].copy_from_slice(&rate.to_le_bytes());
                let mut q = lock(&sink.q);
                q.offer(&ws::frame(2, &hdr));
            }
            continue;
        }
        idle = 0;
        if !lock(&sink.q).idle() {
            // the line is slower than the music: do not build what cannot go out now
            cursor = catch_up(cursor, w, rate, LISTEN_MAX_LAG_MS, 100);
            continue;
        }
        if w < cursor || w - cursor > u64::from(rate) * 2 {
            cursor = w.saturating_sub(u64::from(rate / 10)); // fell far behind (or the Hub restarted): jump to now
        }
        let mut w = w;
        for burst in 0..OPUS_BURST {
            match st.produce(&mut cursor, w, rate) {
                Step::Gone => break 'session,
                Step::Skip => break,
                Step::Sent => {}
            }
            // Opus goes out one 10 ms packet per round; when the loop ran late (it waits up to 10 ms for the browser)
            // the next ones follow at once instead of the listener falling behind (C++ bug: lag grew until the jump)
            if st.codec != CODEC_OPUS || burst + 1 == OPUS_BURST || !lock(&sink.q).idle() {
                break;
            }
            w = sh.writePos.0[0].load(Ordering::Acquire);
            if w < cursor {
                break;
            }
        }
    }
    sink.over.store(true, Ordering::Release);
    if let Some(w) = worker.as_mut() {
        w.shutdown();
    }
}
