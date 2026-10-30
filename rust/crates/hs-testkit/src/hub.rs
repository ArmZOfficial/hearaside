//! `TestHub`: the part of `HubProcessor` (plugins/hub) the self-tests and the browser tests drive, over the Rust stack: the
//! `HubEngine` on a bus, the friends room, the `ShareServer`, the permanent-link `ShareDirectory`, the REST API
//! (`HubControl`), per-friend takes and the Hub parameters as plain values. There is no editor and no host: an audio thread of
//! its own runs the demo scene in real time (like a DAW that never stops), and the caller's thread is the message thread:
//! call [`TestHub::pump`] (or [`TestHub::service`] from your own loop), which is the timer of the plug-in.
//!
//! Not ported (the Rust Hub plug-in, R8, does it): auto sync (REST `/sync` is recorded and ignored), the OBS hotkeys and the
//! Track requests / feeders (`serviceRemote`, `serviceRequests`, `serviceFeeders`), the virtual mic, the latency meter thread
//! (the fx latencies read 0), the account's website as the share base, session presets, and the plug-in state XML (the state
//! here is JSON with the same fields).
use crate::friend_room::{FriendRoom, FriendView};
use crate::recorder::TakeRecorder;
use crate::track::{TestTrack, TrackParams};
use hs_app::{Settings, ShareDirectory, Str, origin_of, tr};
use hs_bus::bus::{
    bits_float, post_command, post_source_command, read_slot_identity, read_source_identity,
};
use hs_bus::protocol::{
    BusLayout, SlotFlags, SlotState, SourceFlags, kMaxFriends, kMaxSlots, kMaxSources,
};
use hs_bus::shm::{SharedMemory, now_ns};
use hs_engine::hub_engine::{HubEngine, HubParams, Role};
use hs_engine::measure::HubMeasurer;
use hs_engine::track_publisher::NO_TIME;
use hs_net::control::ControlServer;
use hs_net::rest::{HubControl, HubParam, HubState, RestService, SourceView, TrackView};
use hs_net::share::{CloudflaredSource, ShareServer, Tunnel};
use serde_json::{Value, json};
use std::path::PathBuf;
use std::sync::atomic::{AtomicBool, AtomicI32, AtomicU32, Ordering::Relaxed};
use std::sync::{Arc, Mutex, MutexGuard, PoisonError};
use std::thread::JoinHandle;
use std::time::{Duration, Instant};

const SAMPLE_RATE: f64 = 48_000.0;
/// the block the checks run the Hub at (`hub.prepareToPlay(48000, 256)` in `ui-snapshot`)
pub const BLOCK: usize = 256;

fn lock<T>(m: &Mutex<T>) -> MutexGuard<'_, T> {
    m.lock().unwrap_or_else(PoisonError::into_inner)
}

/// What a test wants to be different from the defaults.
#[derive(Clone, Debug)]
pub struct HubOptions {
    /// the private bus (`ui-snapshot` used "Snapshot"; never a DAW's "Main")
    pub bus: String,
    /// the settings file; `None` = the tools' own file next to the user's (`settings-ui-snapshot`), never the user's
    pub settings_path: Option<PathBuf>,
    /// where friends' takes go; `None` = `Documents\HEARASIDE\Recordings`
    pub recordings_dir: Option<PathBuf>,
    /// seven Tracks playing sines, like the mock-up
    pub demo_tracks: bool,
    /// ports of the REST API; `None` = 47800..=47809
    pub rest_ports: Option<Vec<u16>>,
}

impl Default for HubOptions {
    fn default() -> Self {
        HubOptions {
            bus: "Snapshot".into(),
            settings_path: None,
            recordings_dir: None,
            demo_tracks: true,
            rest_ports: None,
        }
    }
}

/// The Hub's parameters (`hubparam::` in `HubProcessor.h`), readable from the audio thread.
pub struct Params {
    master_db: AtomicU32,
    limiter_on: AtomicBool,
    ceiling_db: AtomicU32,
    preview: AtomicBool,
    panic: AtomicBool,
    sync_safety: AtomicI32,
    headphones_db: AtomicU32,
    silence: AtomicBool,
}

impl Params {
    fn new() -> Self {
        Params {
            master_db: AtomicU32::new(0f32.to_bits()),
            limiter_on: AtomicBool::new(true),
            ceiling_db: AtomicU32::new((-1f32).to_bits()),
            preview: AtomicBool::new(false),
            panic: AtomicBool::new(false),
            sync_safety: AtomicI32::new(0),
            headphones_db: AtomicU32::new(0f32.to_bits()),
            silence: AtomicBool::new(false),
        }
    }

    pub fn master_db(&self) -> f32 {
        f32::from_bits(self.master_db.load(Relaxed))
    }

    pub fn headphones_db(&self) -> f32 {
        f32::from_bits(self.headphones_db.load(Relaxed))
    }

    pub fn limiter_on(&self) -> bool {
        self.limiter_on.load(Relaxed)
    }

    pub fn preview(&self) -> bool {
        self.preview.load(Relaxed)
    }

    /// "Mute Stream"
    pub fn panic(&self) -> bool {
        self.panic.load(Relaxed)
    }

    fn engine(&self) -> HubParams {
        let silence = self.silence.load(Relaxed);
        HubParams {
            master_db: self.master_db(),
            limiter_on: self.limiter_on(),
            ceiling_db: f32::from_bits(self.ceiling_db.load(Relaxed)),
            preview: self.preview() && !silence,
            panic: self.panic(),
            sync_safety: self.sync_safety.load(Relaxed),
            bypassed: false,
            silence,
        }
    }
}

/// An audio parameter value as the host stores it: inside its range, on its 0.1 dB steps (`NormalisableRange` interval).
fn snap(v: f32, lo: f32, hi: f32) -> f32 {
    if v.is_nan() {
        return lo.max(0.0).min(hi);
    }
    (((v.clamp(lo, hi) - lo) / 0.1).round() * 0.1 + lo).clamp(lo, hi)
}

/// The audio thread of the scene: the demo Tracks, then the Hub on the sum, one 256-frame block every 5.33 ms of wall clock
/// (blocks are caught up after a hiccup, like a DAW's callback).
struct Audio {
    stop: Arc<AtomicBool>,
    thread: Option<JoinHandle<()>>,
}

impl Audio {
    fn start(
        engine: Arc<Mutex<HubEngine>>,
        tracks: Arc<Mutex<Vec<Arc<TestTrack>>>>,
        params: Arc<Params>,
    ) -> Audio {
        let stop = Arc::new(AtomicBool::new(false));
        let flag = Arc::clone(&stop);
        let thread = std::thread::Builder::new()
            .name("testkit-audio".into())
            .spawn(move || {
                let mut master = [vec![0.0f32; BLOCK], vec![0.0f32; BLOCK]];
                let mut one = [vec![0.0f32; BLOCK], vec![0.0f32; BLOCK]];
                let t0 = Instant::now();
                let mut done = 0u64;
                while !flag.load(Relaxed) {
                    let due = (t0.elapsed().as_secs_f64() * SAMPLE_RATE / BLOCK as f64) as u64;
                    if due > done + 400 {
                        done = due - 4; // a long stall: do not race to catch up
                    }
                    while done < due && !flag.load(Relaxed) {
                        done += 1;
                        master[0].fill(0.0);
                        master[1].fill(0.0);
                        for t in lock(&tracks).iter() {
                            t.process_demo(&mut one);
                            for c in 0..2 {
                                for (m, s) in master[c].iter_mut().zip(&one[c]) {
                                    *m += 0.3 * s; // the Tracks reach the master (what viewers hear)
                                }
                            }
                        }
                        let [l, r] = &mut master;
                        lock(&engine).process(
                            &mut [&mut l[..], &mut r[..]],
                            &params.engine(),
                            NO_TIME,
                            false,
                            false,
                        );
                    }
                    std::thread::sleep(Duration::from_millis(1));
                }
            })
            .expect("the audio thread starts");
        Audio {
            stop,
            thread: Some(thread),
        }
    }

    fn stop(&mut self) {
        self.stop.store(true, Relaxed);
        if let Some(t) = self.thread.take() {
            let _ = t.join();
        }
    }
}

pub struct TestHub {
    bus_name: String,
    engine: Arc<Mutex<HubEngine>>,
    measurer: HubMeasurer,
    shm: Option<Arc<SharedMemory>>,
    params: Arc<Params>,
    audio: Audio,
    tracks: Arc<Mutex<Vec<Arc<TestTrack>>>>,
    settings: Settings,
    share: ShareServer,
    directory: ShareDirectory,
    room: FriendRoom,
    rest: Option<RestService>,
    recorders: Vec<TakeRecorder>,
    rec_wanted: Arc<[AtomicBool; kMaxFriends]>,
    share_wanted: bool,
    line_up: bool,
    /// the bus the share server was started on (its address)
    share_bus: Option<usize>,
    listen_token: String,
    send_token: String,
    share_secret: String,
    started: Instant,
    last_maintain: Instant,
    last_connect: Instant,
    silent_since: Option<Instant>,
    viewers_silent: bool,
    sync_requests: Vec<&'static str>,
}

impl TestHub {
    pub fn new(opts: &HubOptions) -> TestHub {
        let mut engine = HubEngine::new();
        engine.connect(&opts.bus);
        engine.prepare(SAMPLE_RATE, BLOCK);
        engine.set_output_name(0, "Stream Mix");
        for i in 0..8 {
            engine.set_output_name(1 + i, &format!("Stem {}", i + 1));
        }
        let recorders: Vec<TakeRecorder> = (0..kMaxFriends)
            .map(|_| {
                let r = TakeRecorder::new();
                if let Some(d) = &opts.recordings_dir {
                    r.set_folder(d);
                }
                r.prepare(SAMPLE_RATE);
                r
            })
            .collect();
        let rec_wanted: Arc<[AtomicBool; kMaxFriends]> =
            Arc::new(std::array::from_fn(|_| AtomicBool::new(false)));
        {
            let (recs, wanted) = (recorders.clone(), Arc::clone(&rec_wanted));
            engine.set_friend_tap(Some(Box::new(move |idx, ch, time, playing| {
                if let (Some(r), Some(w)) = (recs.get(idx), wanted.get(idx)) {
                    r.write(ch, w.load(Relaxed), time, playing && time != NO_TIME);
                }
            })));
        }
        let measurer = engine.measurer();
        let shm = measurer.shared_memory();
        let engine = Arc::new(Mutex::new(engine));

        let tracks = Arc::new(Mutex::new(Vec::new()));
        if opts.demo_tracks {
            lock(&tracks).extend(demo_tracks(&opts.bus));
        }
        let params = Arc::new(Params::new());
        let audio = Audio::start(
            Arc::clone(&engine),
            Arc::clone(&tracks),
            Arc::clone(&params),
        );

        let settings_path = opts
            .settings_path
            .clone()
            .unwrap_or_else(snapshot_settings_path);
        let settings = Settings::open(settings_path);
        let share = ShareServer::new();
        // `ui-snapshot` sets HEARASIDE_NO_TUNNEL=1 itself: a test never opens a tunnel
        share.set_cloudflared_source(CloudflaredSource::Disabled);
        let rest = match &opts.rest_ports {
            Some(p) => RestService::with_server(ControlServer::with_ports(p)),
            None => RestService::new(),
        };
        let now = Instant::now();
        TestHub {
            bus_name: opts.bus.clone(),
            engine,
            measurer,
            shm,
            params,
            audio,
            tracks,
            settings,
            share,
            directory: ShareDirectory::new(),
            room: FriendRoom::new(),
            rest: Some(rest),
            recorders,
            rec_wanted,
            share_wanted: false,
            line_up: true,
            share_bus: None,
            listen_token: ShareServer::new_token(),
            send_token: ShareServer::new_token(),
            share_secret: String::new(),
            started: now,
            last_maintain: now,
            last_connect: now,
            silent_since: None,
            viewers_silent: false,
            sync_requests: Vec::new(),
        }
    }

    // ---- the message thread's timer -------------------------------------------------------------------------------------

    /// `HubProcessor::timerCallback`.
    pub fn service(&mut self) {
        let now = Instant::now();
        if self.shm.is_none() && now.duration_since(self.last_connect) > Duration::from_secs(2) {
            self.last_connect = now;
            if lock(&self.engine).connect(&self.bus_name) {
                self.shm = self.measurer.shared_memory();
            }
        }
        if now.duration_since(self.last_maintain) >= Duration::from_millis(500) {
            self.last_maintain = now;
            self.measurer.maintain();
        }
        for t in lock(&self.tracks).iter() {
            t.service();
        }
        for j in 0..kMaxFriends {
            // a friend's take belongs earlier on the timeline: their delay + our jitter buffer
            let mut shift = 0.0f64;
            if let Some(b) = self.bus() {
                let d = bits_float(b.friends.0[j].delayBits.load(Relaxed));
                let buf = bits_float(self.measurer.friend_info(j).buffer_ms_bits.load(Relaxed));
                if d >= 0.0 {
                    shift = f64::from(d) + f64::from(buf);
                }
            }
            self.recorders[j].set_shift_frames((shift * SAMPLE_RATE * 0.001 + 0.5) as i64);
            self.recorders[j].service();
        }
        self.service_share();
        self.service_control();
        self.service_silence();
        self.settings.tick();
    }

    /// Runs the timer at 20 Hz for `ms` milliseconds (`pump` in `ui-snapshot`).
    pub fn pump(&mut self, ms: u64) {
        let end = Instant::now() + Duration::from_millis(ms);
        while Instant::now() < end {
            self.service();
            let left = end.saturating_duration_since(Instant::now());
            std::thread::sleep(left.min(Duration::from_millis(20)));
        }
    }

    /// Pumps until `f` is true or `ms` pass; the answer of the last try.
    pub fn pump_until(&mut self, ms: u64, mut f: impl FnMut(&mut TestHub) -> bool) -> bool {
        let end = Instant::now() + Duration::from_millis(ms);
        loop {
            if f(self) {
                return true;
            }
            if Instant::now() >= end {
                return false;
            }
            self.service();
            std::thread::sleep(Duration::from_millis(20));
        }
    }

    fn service_control(&mut self) {
        let now = self.now_ms();
        if let Some(mut rest) = self.rest.take() {
            rest.service(self, now);
            self.rest = Some(rest);
        }
    }

    fn now_ms(&self) -> u32 {
        self.started.elapsed().as_millis() as u32
    }

    fn service_silence(&mut self) {
        let Some(bus) = self.bus().filter(|_| self.measurer.role() == Role::Owner) else {
            self.silent_since = None;
            self.viewers_silent = false;
            return;
        };
        let sh = &bus.streamHeader;
        let out = bits_float(sh.peakBits[0][0].load(Relaxed))
            .max(bits_float(sh.peakBits[0][1].load(Relaxed)));
        // a Track meant for the viewers that has sound right now (> -50 dBFS)
        let now = now_ns();
        let feeding = bus.slots.iter().any(|s| {
            let f = s.flags.load(Relaxed);
            let hb = s.heartbeatNs.load(Relaxed);
            s.state.load(Relaxed) == SlotState::kSlotActive
                && f & SlotFlags::kFlagApp == 0
                && f & SlotFlags::kFlagStr != 0
                && hb != 0
                && now.saturating_sub(hb) < 1_000_000_000
                && bits_float(s.strGainBits.load(Relaxed)) > -59.0
                && bits_float(s.peakInBits[0].load(Relaxed))
                    .max(bits_float(s.peakInBits[1].load(Relaxed)))
                    > 0.003
        });
        let panic = self.params.panic(); // muted on purpose: not a problem
        if out < 0.001 && feeding && !panic {
            // < -60 dBFS out
            self.silent_since.get_or_insert_with(Instant::now);
        } else {
            self.silent_since = None;
        }
        self.viewers_silent = self
            .silent_since
            .is_some_and(|t| t.elapsed() > Duration::from_secs(5));
    }

    /// The loudest sample that reached friend `slot`'s ring since `*cursor` (a frame position, start with 0), and moves the
    /// cursor to the end. Unlike the header's `peakBits`, which holds the last packet only, a short beep between two looks is
    /// not missed, and it proves that audio really arrived. Looks at the last second at most.
    pub fn friend_ring_peak(&self, slot: usize, cursor: &mut u64) -> f32 {
        let Some(b) = self.bus().filter(|_| slot < kMaxFriends) else {
            return 0.0;
        };
        let write = &b.friends.0[slot].writePos.0;
        let w = write.load(std::sync::atomic::Ordering::Acquire);
        // a friend who came back starts the ring again; a long gap is not worth reading
        let mut pos = if w < *cursor { 0 } else { *cursor }.max(w.saturating_sub(48_000));
        let (mut l, mut r) = (vec![0.0f32; 1024], vec![0.0f32; 1024]);
        let mut peak = 0.0f32;
        while pos < w {
            let n = (w - pos).min(1024) as u32;
            let mut got = 0;
            hs_bus::ring::ring_read_at(
                &b.friendAudio.0[slot],
                write,
                pos,
                &mut [&mut l[..], &mut r[..]],
                n,
                Some(&mut got),
            );
            if got == 0 {
                break;
            }
            for k in 0..got as usize {
                peak = peak.max(l[k].abs()).max(r[k].abs());
            }
            pos += u64::from(got);
        }
        *cursor = w;
        peak
    }

    // ---- the bus and the engine -----------------------------------------------------------------------------------------

    pub fn bus(&self) -> Option<&BusLayout> {
        self.shm.as_deref().map(SharedMemory::layout)
    }

    pub fn bus_name(&self) -> &str {
        &self.bus_name
    }

    /// The mapping of the bus (shared with the engine and the share server).
    pub fn shared_memory(&self) -> Option<Arc<SharedMemory>> {
        self.shm.clone()
    }

    /// The REST API is listening.
    pub fn rest_running(&self) -> bool {
        self.rest.as_ref().is_some_and(|r| r.server().running())
    }

    /// Its port (0 while stopped).
    pub fn rest_port(&self) -> u16 {
        self.rest.as_ref().map_or(0, |r| r.server().port())
    }

    pub fn measurer(&self) -> &HubMeasurer {
        &self.measurer
    }

    pub fn role(&self) -> Role {
        self.measurer.role()
    }

    pub fn params(&self) -> &Params {
        &self.params
    }

    pub fn settings(&mut self) -> &mut Settings {
        &mut self.settings
    }

    /// The demo Tracks (and any added with `add_track`).
    pub fn tracks_on_bus(&self) -> Vec<Arc<TestTrack>> {
        lock(&self.tracks).clone()
    }

    pub fn add_track(&self, name: &str, p: TrackParams) -> Option<Arc<TestTrack>> {
        let mut v = lock(&self.tracks);
        let t = Arc::new(TestTrack::connect(&self.bus_name, name, v.len(), p)?);
        v.push(Arc::clone(&t));
        Some(t)
    }

    // ---- share ------------------------------------------------------------------------------------------------------------

    pub fn share(&self) -> &ShareServer {
        &self.share
    }

    pub fn directory(&self) -> &ShareDirectory {
        &self.directory
    }

    pub fn sharing(&self) -> bool {
        self.share_wanted
    }

    pub fn permanent_links_set(&self) -> bool {
        self.settings.permanent_links() && !self.share_base().is_empty()
    }

    /// The share web site set in Settings (the account's website is not used here).
    pub fn share_base(&self) -> String {
        self.settings.share_base()
    }

    pub fn permanent_url(&self, listen: bool) -> String {
        if !self.permanent_links_set() || self.listen_token.len() < 24 {
            return String::new();
        }
        let (kind, t) = if listen {
            ("l", &self.listen_token)
        } else {
            ("s", &self.send_token)
        };
        format!("{}/{kind}/{t}", self.share_base())
    }

    fn service_share(&mut self) {
        let permanent = self.permanent_links_set();
        if permanent {
            self.ensure_strong_tokens(); // the link can be copied (and sent) before the first share
        }
        // the share server follows the bus this Hub owns
        let bus = if self.measurer.role() == Role::Owner {
            self.shm.clone()
        } else {
            None
        };
        let bus_id = bus.as_ref().map(|b| Arc::as_ptr(b) as usize);
        // the server runs for the listen links and/or for the friends' send-in links
        let wanted = self.share_wanted || self.room.count() > 0;
        if wanted && bus_id != self.share_bus {
            self.share_bus = bus_id;
            match &bus {
                Some(b) => {
                    self.share
                        .start(Arc::clone(b), &self.listen_token, &self.send_token);
                }
                None => self.share.stop(),
            }
        } else if !wanted && self.share.running() {
            self.share.stop();
            self.share_bus = None;
        }
        self.share.set_listen_enabled(self.share_wanted);
        self.room.service(
            bus.as_deref().map(SharedMemory::layout),
            &self.measurer,
            &self.share,
            "",
        );
        self.measurer
            .set_line_up(self.line_up, self.settings.line_up_limit_ms() as f32);
        if !wanted {
            self.directory.stop(); // nothing to find any more: the links go offline
            return;
        }
        self.renew_revoked_links();
        // tell the share web site where the tunnel is (its own thread; heartbeat every 30 s)
        let base = if permanent {
            self.share_base()
        } else {
            String::new()
        };
        self.share.set_allowed_origin(&origin_of(&base));
        let friend_tokens: Vec<String> = self
            .room
            .entries()
            .iter()
            .filter(|e| e.token.len() >= 24)
            .map(|e| e.token.clone())
            .collect();
        let ready = self.share.running() && self.share.tunnel() == Tunnel::Ready;
        let (l, s) = if self.share_wanted {
            (self.listen_token.as_str(), self.send_token.as_str())
        } else {
            ("", "")
        };
        let tunnel = if ready {
            self.share.public_base()
        } else {
            String::new()
        };
        self.directory
            .update(&base, l, s, &self.share_secret, &tunnel, &friend_tokens);
    }

    /// Permanent links live on the internet for months: projects from before them get 130-bit tokens and a secret that
    /// proves the links are theirs.
    fn ensure_strong_tokens(&mut self) {
        if self.listen_token.len() < 24 || self.send_token.len() < 24 {
            let (l, s) = (ShareServer::new_token(), ShareServer::new_token());
            if !l.is_empty() && !s.is_empty() {
                self.listen_token = l;
                self.send_token = s;
                self.share_bus = None; // restart with them
            }
        }
        if self.share_secret.is_empty() {
            self.share_secret = ShareServer::new_secret();
        }
    }

    /// Links revoked on the account website get new tokens.
    fn renew_revoked_links(&mut self) {
        let revoked = self.directory.take_revoked();
        if revoked.is_empty() {
            return;
        }
        if revoked.contains(&self.listen_token) {
            let t = ShareServer::new_token();
            if !t.is_empty() {
                self.share.set_listen_token(&t);
                self.listen_token = t;
            }
        }
        if revoked.contains(&self.send_token) {
            let t = ShareServer::new_token();
            if !t.is_empty() {
                self.send_token = t;
                self.share_bus = None; // restart with it
            }
        }
        let ids: Vec<u32> = self
            .room
            .entries()
            .iter()
            .filter(|e| revoked.contains(&e.token))
            .map(|e| e.id)
            .collect();
        for id in ids {
            self.new_friend_link(id);
        }
    }

    pub fn set_sharing(&mut self, on: bool) {
        if on == self.share_wanted {
            return;
        }
        self.share_wanted = on;
        // Without permanent links every share gets a new random listen link: a link sent for an earlier stream stops working.
        if on && !self.permanent_links_set() {
            let t = ShareServer::new_token();
            if !t.is_empty() {
                self.share.set_listen_token(&t);
                self.listen_token = t;
            }
        }
        if on {
            self.service_share();
        } else {
            self.directory.stop(); // the permanent links say "not shared right now" at once; friends keep their server
        }
        self.share.set_listen_enabled(on);
    }

    // ---- the friends room -------------------------------------------------------------------------------------------------

    pub fn friends(&self) -> Vec<FriendView> {
        self.room.views(self.bus(), &self.measurer)
    }

    pub fn friend_count(&self) -> usize {
        self.room.count()
    }

    pub fn room_full(&self) -> bool {
        self.room.full()
    }

    /// `None` = the room is full.
    pub fn add_friend(&mut self, name: &str) -> Option<u32> {
        let id = self.room.add(name)?;
        self.service_share();
        Some(id)
    }

    pub fn remove_friend(&mut self, id: u32) {
        self.stop_friend_recording(id);
        self.room.remove(id);
        self.service_share();
    }

    pub fn rename_friend(&mut self, id: u32, name: &str) {
        self.room.rename(id, name);
        self.service_share();
    }

    pub fn room(&mut self) -> &mut FriendRoom {
        &mut self.room
    }

    /// The token in a friend's link (the link itself is `friend_link`).
    pub fn friend_token(&self, id: u32) -> String {
        self.room.token(id)
    }

    /// The link to send them ("" when sharing can't start).
    pub fn friend_link(&self, id: u32) -> String {
        let t = self.room.token(id);
        if t.is_empty() {
            return String::new();
        }
        // permanent: the same link every stream (the share web site finds the Hub's tunnel)
        if self.permanent_links_set() && t.len() >= 24 {
            return format!("{}/s/{t}", self.share_base());
        }
        if self.share.running() {
            self.share.friend_url(&t)
        } else {
            String::new()
        }
    }

    pub fn friend_radmin_link(&self, id: u32) -> String {
        let t = self.room.token(id);
        if t.is_empty() || !self.share.running() {
            String::new()
        } else {
            self.share.radmin_friend_url(&t)
        }
    }

    /// The old link stops working.
    pub fn new_friend_link(&mut self, id: u32) -> String {
        let slot = self.room.slot_of(id);
        if self.room.new_link(id).is_empty() {
            return String::new();
        }
        self.share.kick_friend(slot); // the old link is dead: the open page has to use the new one
        self.service_share();
        self.friend_link(id)
    }

    pub fn remeasure_friend(&self, id: u32) {
        self.share.remeasure_friend(self.room.slot_of(id));
    }

    pub fn set_line_up(&mut self, on: bool) {
        self.line_up = on;
    }

    pub fn line_up(&self) -> bool {
        self.line_up
    }

    // ---- per-friend WAV recording (1.10) ------------------------------------------------------------------------------------

    pub fn start_friend_recording(&self, id: u32) {
        let slot = self.room.slot_of(id);
        let Ok(s) = usize::try_from(slot).map(|s| s.min(kMaxFriends)) else {
            return;
        };
        if s >= kMaxFriends || self.rec_wanted[s].load(Relaxed) {
            return;
        }
        let name = match self.room.find(id) {
            Some(e) if !e.name.is_empty() => e.name.clone(),
            _ => format!("Friend {id}"),
        };
        if !self.recorders[s].is_open() && !self.recorders[s].open(&name) {
            return;
        }
        self.rec_wanted[s].store(true, Relaxed);
    }

    pub fn stop_friend_recording(&self, id: u32) {
        let Ok(s) = usize::try_from(self.room.slot_of(id)) else {
            return;
        };
        if s >= kMaxFriends {
            return;
        }
        if !self.rec_wanted[s].swap(false, Relaxed) && !self.recorders[s].is_open() {
            return;
        }
        self.recorders[s].close();
    }

    pub fn is_friend_recording(&self, id: u32) -> bool {
        usize::try_from(self.room.slot_of(id))
            .ok()
            .filter(|&s| s < kMaxFriends)
            .is_some_and(|s| self.rec_wanted[s].load(Relaxed))
    }

    pub fn friend_record_sec(&self, id: u32) -> f64 {
        let Ok(s) = usize::try_from(self.room.slot_of(id)) else {
            return 0.0;
        };
        if s >= kMaxFriends {
            return 0.0;
        }
        if self.recorders[s].is_open() {
            self.recorders[s].seconds()
        } else {
            self.recorders[s].last_seconds()
        }
    }

    pub fn friend_take(&self, id: u32) -> Option<PathBuf> {
        let s = usize::try_from(self.room.slot_of(id)).ok()?;
        self.recorders.get(s)?.last_take()
    }

    // ---- project state --------------------------------------------------------------------------------------------------------

    /// The Hub's part of a project (`getStateInformation`): friends with their links and levels, the share links and
    /// the share secret.
    pub fn save_state(&self) -> Vec<u8> {
        let mut v = json!({
            "bus": self.bus_name,
            "lineUp": self.line_up,
            "friends": self.room.to_state(),
            "listenToken": self.listen_token,
            "sendToken": self.send_token,
            "params": {
                "streamMaster": self.params.master_db(),
                "monitorMaster": self.params.headphones_db(),
                "limiterOn": self.params.limiter_on(),
                "preview": self.params.preview(),
                "panic": self.params.panic(),
            },
        });
        if !self.share_secret.is_empty() {
            v["shareSecret"] = Value::from(self.share_secret.clone());
        }
        serde_json::to_vec(&v).unwrap_or_default()
    }

    /// `setStateInformation`. A state that is not ours changes nothing.
    pub fn load_state(&mut self, data: &[u8]) -> bool {
        let Ok(v) = serde_json::from_slice::<Value>(data) else {
            return false;
        };
        if !v.is_object() {
            return false;
        }
        self.room.from_state(&v["friends"]);
        self.line_up = v["lineUp"].as_bool().unwrap_or(true);
        let (lt, st) = (
            v["listenToken"].as_str().unwrap_or(""),
            v["sendToken"].as_str().unwrap_or(""),
        );
        if lt.len() >= 8 && st.len() >= 8 && (lt != self.listen_token || st != self.send_token) {
            self.listen_token = lt.to_owned();
            self.send_token = st.to_owned();
            self.share_bus = None; // restart with the project's links
        }
        if let Some(s) = v["shareSecret"].as_str().filter(|s| s.len() == 64) {
            self.share_secret = s.to_owned();
        }
        true
    }

    /// Auto sync requests seen on the REST API (`"start"` / `"cancel"`); the sync itself is not ported yet.
    pub fn sync_requests(&self) -> &[&'static str] {
        &self.sync_requests
    }

    pub fn viewers_silent(&self) -> bool {
        self.viewers_silent
    }

    fn latency(&self) -> (i32, f64, bool) {
        let Some(b) = self.bus() else {
            return (0, 0.0, false);
        };
        let fpm = SAMPLE_RATE * 0.001;
        let h = &b.header;
        let block = h.hubBlockSize.load(Relaxed) as i32;
        let hub = f64::from(h.hubLatencyFrames.load(Relaxed)) / fpm;
        let daw = f64::from(block) / fpm;
        let obs = self.obs_connected();
        let obs_ms = if obs {
            f64::from(h.consumerBufferMs.load(Relaxed))
        } else {
            0.0
        };
        (block, daw + hub + obs_ms, obs)
    }

    fn obs_connected(&self) -> bool {
        let Some(b) = self.bus() else { return false };
        let hb = b.header.consumerHeartbeatNs.load(Relaxed);
        hb != 0 && now_ns().saturating_sub(hb) < 1_500_000_000
    }
}

impl Drop for TestHub {
    fn drop(&mut self) {
        if let Some(rest) = &self.rest {
            rest.server().stop();
        }
        self.share.stop();
        self.directory.stop();
        for (j, r) in self.recorders.iter().enumerate() {
            self.rec_wanted[j].store(false, Relaxed);
            r.close();
        }
        self.settings.flush();
        self.audio.stop();
        lock(&self.tracks).clear();
        lock(&self.engine).disconnect();
    }
}

fn demo_tracks(bus: &str) -> Vec<Arc<TestTrack>> {
    struct T(&'static str, bool, bool, f32, f32, f32, i32);
    let demo = [
        T("Backing track", true, true, -3.0, 0.0, 0.0, -1),
        T("Vocal", false, true, 0.0, 0.0, 0.0, 1),
        T("Guitar", true, true, -2.0, -6.0, -30.0, -1),
        T("Click / Guide", true, false, -6.0, -10.0, 0.0, -1),
        T("Talk mic", false, false, 0.0, 0.0, 0.0, -1),
        T("Bass", true, true, -1.5, -4.0, 0.0, -1),
        T("Chorus", false, true, -4.0, 0.0, 25.0, 1),
    ];
    demo.iter()
        .enumerate()
        .filter_map(|(i, d)| {
            let p = TrackParams {
                mon: d.1,
                str_: d.2,
                str_gain_db: d.3,
                mon_trim_db: d.4,
                str_pan: d.5,
                str_delay_ms: if i == 0 { 40.0 } else { 0.0 },
                stem: d.6,
                ..TrackParams::default()
            };
            TestTrack::connect(bus, d.0, i, p).map(Arc::new)
        })
        .collect()
}

/// The settings of the tools (`HEARASIDE_SETTINGS_NAME`, default `settings-ui-snapshot`), never the user's.
pub fn snapshot_settings_path() -> PathBuf {
    let name = std::env::var("HEARASIDE_SETTINGS_NAME")
        .ok()
        .filter(|n| !n.trim().is_empty())
        .unwrap_or_else(|| "settings-ui-snapshot".into());
    hs_app::paths::settings_file_for(
        hs_app::paths::Os::current(),
        &hs_app::paths::PathEnv::from_process(),
        name.trim(),
    )
    .unwrap_or_else(|| std::env::temp_dir().join(format!("{}.xml", name.trim())))
}

/// The account file of the tools (`HEARASIDE_ACCOUNT_NAME`, default `account-ui-snapshot`), never the user's.
pub fn snapshot_account_path() -> PathBuf {
    let name = std::env::var("HEARASIDE_ACCOUNT_NAME")
        .ok()
        .filter(|n| !n.trim().is_empty())
        .unwrap_or_else(|| "account-ui-snapshot".into());
    hs_app::paths::account_file_for(
        hs_app::paths::Os::current(),
        &hs_app::paths::PathEnv::from_process(),
        name.trim(),
    )
    .unwrap_or_else(|| std::env::temp_dir().join(format!("{}.dat", name.trim())))
}

// ---- the REST API ---------------------------------------------------------------------------------------------------------

impl HubControl for TestHub {
    fn version(&self) -> String {
        env!("CARGO_PKG_VERSION").into()
    }

    fn hub_state(&self) -> HubState {
        let (daw_buffer, total, obs) = self.latency();
        HubState {
            stream_muted: self.params.panic(),
            hearing_viewers_mix: self.params.preview(),
            stream_db: self.params.master_db(),
            headphones_db: self.params.headphones_db(),
            peak_protection: self.params.limiter_on(),
            loudness_lufs: self.bus().map_or(-100.0, |b| {
                bits_float(b.streamHeader.loudnessSBits.load(Relaxed))
            }),
            obs_connected: obs,
            viewers_silent: self.viewers_silent,
            latency_to_obs_ms: obs.then_some(total),
            daw_buffer,
            sharing: self.share_wanted,
        }
    }

    fn listen_link(&self) -> String {
        if self.permanent_links_set() {
            self.permanent_url(true)
        } else {
            self.share.listen_url()
        }
    }

    /// Active slots, in insertion order (`HubProcessor::tracks`).
    fn tracks(&self) -> Vec<TrackView> {
        let Some(bus) = self.bus() else {
            return Vec::new();
        };
        let now = now_ns();
        let mut out: Vec<(u32, TrackView)> = Vec::new();
        for (i, s) in bus.slots.iter().enumerate().take(kMaxSlots) {
            if s.state.load(std::sync::atomic::Ordering::Acquire) != SlotState::kSlotActive {
                continue;
            }
            let f = s.flags.load(std::sync::atomic::Ordering::Acquire);
            if f & SlotFlags::kFlagApp != 0 {
                continue; // listed with its App Audio
            }
            let name = read_slot_identity(s).map(|(n, _, _)| n).unwrap_or_default();
            let hb = s.heartbeatNs.load(Relaxed);
            out.push((
                s.sequence.load(Relaxed),
                TrackView {
                    slot: i as i32,
                    name: if name.is_empty() {
                        format!("{} {}", tr(Str::TrackWord), i + 1)
                    } else {
                        name
                    },
                    active: hb != 0 && now.saturating_sub(hb) < 1_000_000_000,
                    you_hear: f & SlotFlags::kFlagMon != 0,
                    viewers_hear: f & SlotFlags::kFlagStr != 0,
                    headphone_db: bits_float(s.monTrimBits.load(Relaxed)),
                    viewers_db: bits_float(s.strGainBits.load(Relaxed)),
                    delay_ms: bits_float(s.strDelayBits.load(Relaxed)),
                    solo: f & SlotFlags::kFlagSolo != 0,
                    bypassed: f & SlotFlags::kFlagBypassed != 0,
                },
            ));
        }
        out.sort_by_key(|(seq, _)| *seq);
        out.into_iter().map(|(_, v)| v).collect()
    }

    /// App Audio instances, in insertion order (`HubProcessor::sources`).
    fn sources(&self) -> Vec<SourceView> {
        let Some(bus) = self.bus() else {
            return Vec::new();
        };
        let now = now_ns();
        let mut out: Vec<(u32, SourceView)> = Vec::new();
        for (i, s) in bus.sources.iter().enumerate().take(kMaxSources) {
            if s.state.load(std::sync::atomic::Ordering::Acquire) != SlotState::kSlotActive {
                continue;
            }
            let (name, app, _) = read_source_identity(s).unwrap_or_default();
            let flags = s.flags.load(Relaxed);
            let hb = s.heartbeatNs.load(Relaxed);
            let mut v = SourceView {
                index: i as i32,
                name: if name.is_empty() {
                    format!("App Audio {}", i + 1)
                } else {
                    name
                },
                program: app,
                on: flags & SourceFlags::kSrcOn != 0,
                active: hb != 0 && now.saturating_sub(hb) < 1_000_000_000,
                slot: -1,
                delay_ms: bits_float(s.delayBits.load(Relaxed)),
                recording: flags & SourceFlags::kSrcRecording != 0,
                ..SourceView::default()
            };
            let t = s.trackSlot.load(Relaxed);
            if let Some(sl) = usize::try_from(t).ok().and_then(|t| bus.slots.get(t)) {
                let f = sl.flags.load(std::sync::atomic::Ordering::Acquire);
                if sl.state.load(std::sync::atomic::Ordering::Acquire) == SlotState::kSlotActive
                    && f & SlotFlags::kFlagApp != 0
                {
                    v.slot = t;
                    v.you_hear = f & SlotFlags::kFlagMon != 0;
                    v.viewers_hear = f & SlotFlags::kFlagStr != 0;
                    v.viewers_db = bits_float(sl.strGainBits.load(Relaxed));
                    v.headphone_db = bits_float(sl.monTrimBits.load(Relaxed));
                }
            }
            out.push((s.sequence.load(Relaxed), v));
        }
        out.sort_by_key(|(seq, _)| *seq);
        out.into_iter().map(|(_, v)| v).collect()
    }

    fn hub_range(&self, p: HubParam) -> (f32, f32) {
        match p {
            HubParam::Master => (-60.0, 12.0),
            HubParam::Headphones => (-60.0, 6.0),
            HubParam::LimiterOn | HubParam::Preview | HubParam::Panic => (0.0, 1.0),
        }
    }

    fn track_range(&self, id: u32) -> (f32, f32) {
        use hs_bus::protocol::ParamId;
        match id {
            ParamId::StrGainDb => (-60.0, 12.0),
            ParamId::MonTrimDb => (-60.0, 6.0),
            ParamId::StrDelayMs => (0.0, 500.0),
            _ => (0.0, 1.0),
        }
    }

    fn set_param(&mut self, p: HubParam, plain: f32) {
        let (lo, hi) = self.hub_range(p);
        match p {
            HubParam::Master => self
                .params
                .master_db
                .store(snap(plain, lo, hi).to_bits(), Relaxed),
            HubParam::Headphones => self
                .params
                .headphones_db
                .store(snap(plain, lo, hi).to_bits(), Relaxed),
            HubParam::LimiterOn => self.params.limiter_on.store(plain > 0.5, Relaxed),
            HubParam::Preview => self.params.preview.store(plain > 0.5, Relaxed),
            HubParam::Panic => self.params.panic.store(plain > 0.5, Relaxed),
        }
    }

    fn send(&mut self, slot: i32, id: u32, value: f32) {
        if let (Some(bus), Ok(s)) = (self.bus(), usize::try_from(slot))
            && s < kMaxSlots
        {
            post_command(&bus.slots[s], id, value);
        }
    }

    fn send_source(&mut self, index: i32, id: u32, value: f32) {
        if let (Some(bus), Ok(s)) = (self.bus(), usize::try_from(index))
            && s < kMaxSources
        {
            post_source_command(&bus.sources[s], id, value);
        }
    }

    fn start_auto_sync(&mut self) {
        self.sync_requests.push("start");
    }

    fn cancel_auto_sync(&mut self) {
        self.sync_requests.push("cancel");
    }

    fn set_sharing(&mut self, on: bool) {
        TestHub::set_sharing(self, on);
    }

    fn is_bus_owner(&self) -> bool {
        self.measurer.role() == Role::Owner
    }

    fn rest_api_enabled(&self) -> bool {
        self.settings.rest_api()
    }

    fn rest_api_key(&self) -> String {
        self.settings.rest_api_key()
    }

    fn set_rest_api_key(&mut self, key: &str) {
        self.settings.set_rest_api_key(key);
    }
}
