//! HEARASIDE share (like Audiomovers LISTENTO): a small web server inside the Hub. Port of
//! `plugins/common/ShareServer.{h,cpp}`.
//!
//! - `/l/<token>`: page that plays the Stream Mix (what viewers hear) in any browser
//! - `/s/<token>`: page that sends a browser microphone into the DAW (App Audio "*link*", or one friend's link)
//!
//! Audio goes over WebSockets as 16-bit PCM (HRA1 / HRA2) or Opus (HRA3), and over a WebRTC DataChannel when one is up
//! (HRA3 only). For an internet link the Hub starts `cloudflared` (a free Cloudflare quick tunnel) when it is
//! installed; otherwise the links work on the local network.
//!
//! Plain std threads, one per connection (as the C++), none of them real time: the bus is read and written through the
//! lock-free ring functions of `hs-bus`, and the audio threads of the Hub never wait for anything in here.
//! Differences from the C++ (bugs fixed) are listed in `docs/handoff/` (R3.3 ledger) and marked `C++ bug` below.
mod friend;
mod listener;
mod rtcworker;
mod sender;
// sock / util / ws are shared with `control` (R3.4): the same socket waits, constant-time compare, JUCE-style JSON
pub(crate) mod sock;
mod tunnel;
pub(crate) mod util;
pub(crate) mod ws;

use crate::aec::{AecMode, FriendEcho};
use crate::rtc::IceServerConfig;
use hs_bus::protocol::{FriendState, kMaxFriends};
use hs_bus::shm::SharedMemory;
use std::collections::HashMap;
use std::net::{Ipv4Addr, SocketAddr, TcpListener, TcpStream, ToSocketAddrs};
use std::path::PathBuf;
use std::sync::atomic::{
    AtomicBool, AtomicI32, AtomicU8, AtomicU16, AtomicU32, AtomicU64, AtomicUsize, Ordering,
};
use std::sync::{Arc, Mutex, MutexGuard, PoisonError};
use std::thread::JoinHandle;
use std::time::{Duration, Instant};
use util::{AtomicF32, ct_eq, path_is};

const FIRST_PORT: u16 = 47810;
const PORT_COUNT: u16 = 20;
/// plenty for a few listeners
const MAX_CONNS: usize = 40;
/// listeners never take the friends' (and the send-in link's) places
const MAX_LISTENERS: i32 = MAX_CONNS as i32 - kMaxFriends as i32 - 2;
/// one LAN device: a few pages + their WebSockets
const MAX_PER_PEER: usize = 6;
/// connections that have not sent a whole request head yet: the rest of the places stay free
const MAX_PENDING: usize = 24;
const MAX_HEAD: usize = 8192;
/// Most frames one PCM microphone packet may hold (120 ms at 48 kHz, the same bound as an Opus packet); the pages
/// send ~10 ms. A bigger claim is not audio.
pub(crate) const MAX_PCM_FRAMES: usize = 5760;
const HEAD_TIMEOUT: Duration = Duration::from_secs(5);
/// bytes of pongs that may wait behind a stuck line
pub(crate) const MAX_CONTROL_QUEUE: usize = 1024;
/// friend: the page buffers ~60 ms itself, so no more than this waits at the Hub
pub(crate) const FRIEND_MAX_LAG_MS: u32 = 150;
/// listener: the page may buffer up to 0.8 s itself
pub(crate) const LISTEN_MAX_LAG_MS: u32 = 300;
const MAX_FRIENDS: usize = kMaxFriends;

const LISTEN_HTML: &[u8] = include_bytes!("../../../../../plugins/hub/web/listen.html");
const SEND_HTML: &[u8] = include_bytes!("../../../../../plugins/hub/web/send.html");

#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub enum Tunnel {
    Off,
    Missing,
    Starting,
    Ready,
    Failed,
}

impl Tunnel {
    fn from_u8(v: u8) -> Self {
        match v {
            1 => Tunnel::Missing,
            2 => Tunnel::Starting,
            3 => Tunnel::Ready,
            4 => Tunnel::Failed,
            _ => Tunnel::Off,
        }
    }
}

/// Cloudflare first: while the tunnel is starting the links are "" (a LAN link copied then never works outside the
/// house). Without internet cloudflared never says it's ready: LAN after this.
pub const TUNNEL_WAIT: Duration = Duration::from_millis(20_000);

/// Where `start` looks for `cloudflared`.
#[derive(Clone, Debug, Default)]
pub enum CloudflaredSource {
    /// `HEARASIDE_NO_TUNNEL` set = no tunnel (tests: local links only); else the usual places ([`find_cloudflared`]).
    #[default]
    Auto,
    /// Never start a tunnel.
    Disabled,
    /// Only these folders (tests, portable installs).
    Dirs(Vec<PathBuf>),
}

/// One send-in link per friend: `/s/<token>`. The table is what the server checks; it is owned by the Hub's
/// FriendRoom (message thread). A friend who connects again takes over their own slot ("newest wins" inside one slot,
/// never across slots).
#[derive(Clone, Debug)]
pub struct FriendLink {
    /// 0..7 = bus friend index; anything else is ignored (the C++ indexed past its arrays)
    pub slot: i32,
    /// `FriendHeader::id`
    pub id: u32,
    /// the secret in the link (never logged)
    pub token: String,
    /// shown on the friend's page
    pub name: String,
    /// "<host> invited you to sing" ("" = not shown)
    pub host: String,
}

/// What the line to one friend does: round trip from the page's pong (median of the last few), the bit rates of the
/// last second, and the worst gaps since the friend connected. `rtt_ms < 0` = no pong yet (an older page never
/// answers).
#[derive(Clone, Copy, Debug, PartialEq)]
pub struct FriendStats {
    pub rtt_ms: f32,
    pub up_kbps: f32,
    pub down_kbps: f32,
    /// longest silence between two microphone packets
    pub mic_gap_max_ms: f32,
    /// most audio waiting for a slow line
    pub down_lag_max_ms: f32,
    /// times a slow line made the Hub jump ahead
    pub down_skips: u32,
}

impl Default for FriendStats {
    fn default() -> Self {
        FriendStats {
            rtt_ms: -1.0,
            up_kbps: 0.0,
            down_kbps: 0.0,
            mic_gap_max_ms: 0.0,
            down_lag_max_ms: 0.0,
            down_skips: 0,
        }
    }
}

/// `FriendStats` as atomics: the reader and the writer thread of a session fill it.
pub(crate) struct FriendNet {
    pub rtt: AtomicF32,
    pub up: AtomicF32,
    pub down: AtomicF32,
    pub mic_gap: AtomicF32,
    pub lag: AtomicF32,
    pub skips: AtomicU32,
    pub up_bytes: AtomicU64,
    pub down_bytes: AtomicU64,
}

impl FriendNet {
    fn new() -> Self {
        FriendNet {
            rtt: AtomicF32::new(-1.0),
            up: AtomicF32::new(0.0),
            down: AtomicF32::new(0.0),
            mic_gap: AtomicF32::new(0.0),
            lag: AtomicF32::new(0.0),
            skips: AtomicU32::new(0),
            up_bytes: AtomicU64::new(0),
            down_bytes: AtomicU64::new(0),
        }
    }
    pub(crate) fn reset(&self) {
        self.rtt.store(-1.0);
        self.up.store(0.0);
        self.down.store(0.0);
        self.mic_gap.store(0.0);
        self.lag.store(0.0);
        self.skips.store(0, Ordering::Relaxed);
        self.up_bytes.store(0, Ordering::Relaxed);
        self.down_bytes.store(0, Ordering::Relaxed);
    }
}

struct Urls {
    listen_token: String,
    send_token: String,
    public_base: String,
    allowed_origin: String,
}

/// One accepted connection as the accept loop and the other connections see it.
pub(crate) struct ConnInfo {
    /// the client's address (127.0.0.1 for everything that comes through the tunnel)
    peer: String,
    /// who is really asking: Cloudflare's Cf-Connecting-Ip behind the tunnel, else `peer`
    client: Mutex<String>,
    done: AtomicBool,
    head_done: AtomicBool,
}

struct ConnEntry {
    info: Arc<ConnInfo>,
    /// a second handle only to `shutdown` the socket from `stop()`
    wake: TcpStream,
    thread: Option<JoinHandle<()>>,
}

pub(crate) struct Inner {
    pub running: AtomicBool,
    port: AtomicU16,
    pub listeners: AtomicI32,
    pub senders: AtomicI32,
    pub sender_gen: AtomicU32,
    /// one microphone into the bus at a time
    pub link_in_write: Mutex<()>,
    conns: Mutex<Vec<ConnEntry>>,
    urls: Mutex<Urls>,
    friends: Mutex<Vec<FriendLink>>,
    pub friend_gen: [AtomicU32; MAX_FRIENDS],
    pub friend_kick: [AtomicBool; MAX_FRIENDS],
    pub friend_remeasure: [AtomicBool; MAX_FRIENDS],
    /// writes of one friend's microphone into the bus
    pub friend_write: [Mutex<()>; MAX_FRIENDS],
    pub friend_echo: [Mutex<FriendEcho>; MAX_FRIENDS],
    pub friend_net: [FriendNet; MAX_FRIENDS],
    /// bytes the send queue of the live session holds (tests: it must stay bounded)
    pub friend_queue_bytes: [AtomicUsize; MAX_FRIENDS],
    pub friends_live: AtomicI32,
    listen_enabled: AtomicBool,
    rtc_enabled: AtomicBool,
    tunnel: AtomicU8,
    tunnel_start: Mutex<Option<Instant>>,
    ice: Mutex<Vec<IceServerConfig>>,
    /// STUN host names resolved once per `start()` on the server's own thread: `url -> addresses`
    resolved: Mutex<HashMap<String, Vec<SocketAddr>>>,
    /// running WebRTC worker threads (all of them are joined by their session; `stop()` leaves none)
    pub rtc_threads: Arc<AtomicUsize>,
    cloudflared: Mutex<CloudflaredSource>,
}

fn lock<T>(m: &Mutex<T>) -> MutexGuard<'_, T> {
    m.lock().unwrap_or_else(PoisonError::into_inner)
}

impl Inner {
    fn new() -> Self {
        Inner {
            running: AtomicBool::new(false),
            port: AtomicU16::new(0),
            listeners: AtomicI32::new(0),
            senders: AtomicI32::new(0),
            sender_gen: AtomicU32::new(0),
            link_in_write: Mutex::new(()),
            conns: Mutex::new(Vec::new()),
            urls: Mutex::new(Urls {
                listen_token: String::new(),
                send_token: String::new(),
                public_base: String::new(),
                allowed_origin: String::new(),
            }),
            friends: Mutex::new(Vec::new()),
            friend_gen: std::array::from_fn(|_| AtomicU32::new(0)),
            friend_kick: std::array::from_fn(|_| AtomicBool::new(false)),
            friend_remeasure: std::array::from_fn(|_| AtomicBool::new(false)),
            friend_write: std::array::from_fn(|_| Mutex::new(())),
            friend_echo: std::array::from_fn(|_| Mutex::new(FriendEcho::new())),
            friend_net: std::array::from_fn(|_| FriendNet::new()),
            friend_queue_bytes: std::array::from_fn(|_| AtomicUsize::new(0)),
            friends_live: AtomicI32::new(0),
            listen_enabled: AtomicBool::new(true),
            rtc_enabled: AtomicBool::new(true),
            tunnel: AtomicU8::new(0),
            tunnel_start: Mutex::new(None),
            ice: Mutex::new(Vec::new()),
            resolved: Mutex::new(HashMap::new()),
            rtc_threads: Arc::new(AtomicUsize::new(0)),
            cloudflared: Mutex::new(CloudflaredSource::Auto),
        }
    }

    pub(crate) fn is_running(&self) -> bool {
        self.running.load(Ordering::SeqCst)
    }
    fn port(&self) -> u16 {
        self.port.load(Ordering::SeqCst)
    }
    fn tunnel(&self) -> Tunnel {
        Tunnel::from_u8(self.tunnel.load(Ordering::SeqCst))
    }
    fn set_tunnel(&self, t: Tunnel) {
        self.tunnel.store(t as u8, Ordering::SeqCst);
    }
    fn listen_token(&self) -> String {
        lock(&self.urls).listen_token.clone()
    }
    fn send_token(&self) -> String {
        lock(&self.urls).send_token.clone()
    }
    fn public_base(&self) -> String {
        lock(&self.urls).public_base.clone()
    }
    fn set_public_base(&self, v: &str) {
        lock(&self.urls).public_base = v.to_owned();
    }
    /// A session's WebRTC peer, unless switched off (tests that open dozens of sessions).
    pub(crate) fn start_rtc(
        &self,
        sink: Arc<dyn rtcworker::RtcSink>,
    ) -> Option<rtcworker::RtcWorker> {
        if self.rtc_enabled.load(Ordering::SeqCst) {
            rtcworker::RtcWorker::start(sink, self.ice_servers(), self.rtc_threads.clone())
        } else {
            None
        }
    }

    /// The configured ICE servers (default: the two public STUN servers of the C++).
    fn ice_config(&self) -> Vec<IceServerConfig> {
        let v = lock(&self.ice).clone();
        if v.is_empty() {
            rtcworker::default_ice()
        } else {
            v
        }
    }

    /// The ICE servers for a new peer, every STUN host as IP addresses: the peer must never do DNS (a resolver that
    /// hangs would hang a thread the DAW cannot stop). A name that is not resolved (yet, or at all) is left out.
    pub(crate) fn ice_servers(&self) -> Vec<IceServerConfig> {
        let resolved = lock(&self.resolved);
        let mut out = Vec::new();
        for c in self.ice_config() {
            match stun_authority(&c.url) {
                Some(auth) if auth.parse::<SocketAddr>().is_err() => {
                    for a in resolved.get(&c.url).into_iter().flatten() {
                        out.push(IceServerConfig {
                            url: format!("stun:{a}"),
                            ..c.clone()
                        });
                    }
                }
                _ => out.push(c), // an address already, or turn: / stuns: (the peer refuses those without DNS)
            }
        }
        if out.is_empty() {
            // none usable: a closed loopback port, so the peer gathers host candidates only (an empty list would mean
            // "the default servers", i.e. DNS)
            out.push(IceServerConfig {
                url: "stun:127.0.0.1:9".into(),
                username: String::new(),
                password: String::new(),
            });
        }
        out
    }

    /// Resolves the STUN host names (blocking: runs on the server's own thread).
    fn resolve_ice(&self) {
        for c in self.ice_config() {
            let Some(auth) = stun_authority(&c.url) else {
                continue;
            };
            if auth.parse::<SocketAddr>().is_ok() {
                continue;
            }
            if let Ok(addrs) = auth.to_socket_addrs() {
                let mut v: Vec<SocketAddr> = addrs.collect();
                v.sort_unstable();
                v.dedup();
                lock(&self.resolved).insert(c.url.clone(), v);
            }
        }
    }

    fn needs_dns(&self) -> bool {
        self.ice_config()
            .iter()
            .any(|c| stun_authority(&c.url).is_some_and(|a| a.parse::<SocketAddr>().is_err()))
    }

    fn lan_base(&self) -> String {
        let ip = util::pick_lan(&util::local_ipv4());
        format!("http://{ip}:{}", self.port())
    }

    fn radmin_base(&self) -> String {
        match util::pick_radmin(&util::local_ipv4()) {
            Some(ip) => format!("http://{ip}:{}", self.port()),
            None => String::new(),
        }
    }

    /// Public, LAN, or "" while waiting for the tunnel.
    fn link_base(&self, now: Instant) -> String {
        let p = self.public_base();
        if !p.is_empty() {
            return p;
        }
        if self.tunnel() == Tunnel::Starting
            && lock(&self.tunnel_start).is_some_and(|t| now.duration_since(t) < TUNNEL_WAIT)
        {
            return String::new();
        }
        self.lan_base()
    }

    fn origin_allowed(&self, origin: &str) -> bool {
        let (public, allowed) = {
            let u = lock(&self.urls);
            (u.public_base.clone(), u.allowed_origin.clone())
        };
        util::origin_allowed(origin, self.port(), &util::local_ipv4(), &public, &allowed)
    }

    /// `(slot, id, name, host)` of the friend whose link this token is. Looks at every entry: how long it takes does
    /// not say which one matched (C++ bug: `!=` on the strings).
    pub(crate) fn friend_slot_for(&self, token: &str) -> Option<(usize, u32, String, String)> {
        if token.len() < 20 {
            return None;
        }
        let list = lock(&self.friends);
        let mut found = None;
        for f in list.iter() {
            if ct_eq(f.token.as_bytes(), token.as_bytes()) && found.is_none() {
                found = Some((f.slot as usize, f.id, f.name.clone(), f.host.clone()));
            }
        }
        found
    }
}

/// What every session thread holds: the server state and the bus mapping (alive as long as any thread uses it).
#[derive(Clone)]
pub(crate) struct Ctx {
    pub inner: Arc<Inner>,
    pub bus: Arc<SharedMemory>,
}

/// `host:port` of a `stun:` URL (default port 3478), as the peer reads it; `None` for other schemes.
fn stun_authority(url: &str) -> Option<String> {
    let host = url.strip_prefix("stun:")?;
    let host = host.strip_suffix("?transport=udp").unwrap_or(host);
    Some(if host.starts_with('[') {
        if host.ends_with(']') {
            format!("{host}:3478")
        } else {
            host.to_owned()
        }
    } else if host.contains(':') {
        host.to_owned()
    } else {
        format!("{host}:3478")
    })
}

struct Run {
    bus: Arc<SharedMemory>,
    accept: Option<JoinHandle<()>>,
    /// resolves the STUN host names once; joined by `stop()` like every other thread
    dns: Option<JoinHandle<()>>,
    tunnel_thread: Option<JoinHandle<()>>,
    tunnelled: Option<Arc<tunnel::Tunnelled>>,
}

pub struct ShareServer {
    inner: Arc<Inner>,
    run: Mutex<Option<Run>>,
}

impl Default for ShareServer {
    fn default() -> Self {
        Self::new()
    }
}

impl Drop for ShareServer {
    fn drop(&mut self) {
        self.stop();
    }
}

/// Decrements a counter when the session ends, also when it panics.
pub(crate) struct CountGuard<'a>(&'a AtomicI32, i32);

impl<'a> CountGuard<'a> {
    pub(crate) fn new(c: &'a AtomicI32) -> Self {
        let n = c.fetch_add(1, Ordering::SeqCst) + 1;
        CountGuard(c, n)
    }
    /// The count including this one.
    pub(crate) fn count(&self) -> i32 {
        self.1
    }
}

impl Drop for CountGuard<'_> {
    fn drop(&mut self) {
        self.0.fetch_sub(1, Ordering::SeqCst);
    }
}

impl ShareServer {
    pub fn new() -> Self {
        ShareServer {
            inner: Arc::new(Inner::new()),
            run: Mutex::new(None),
        }
    }

    // ---- lifecycle --------------------------------------------------------------------------------------------

    /// Binds the first free port from 47810 and starts serving. Message thread. `false` when no port is free.
    pub fn start(&self, bus: Arc<SharedMemory>, listen_token: &str, send_token: &str) -> bool {
        let mut run = lock(&self.run);
        self.stop_locked(&mut run);
        sock::ignore_sigpipe();
        let inner = &self.inner;
        {
            let mut u = lock(&inner.urls);
            u.listen_token = listen_token.to_owned();
            u.send_token = send_token.to_owned();
        }
        let Some((listener, port)) = (FIRST_PORT..FIRST_PORT + PORT_COUNT).find_map(|p| {
            TcpListener::bind((Ipv4Addr::UNSPECIFIED, p))
                .ok()
                .map(|l| (l, p))
        }) else {
            return false;
        };
        if listener.set_nonblocking(true).is_err() {
            return false;
        }
        // a process the DAW starts must not keep the listener open after we close it
        sock::no_inherit_listener(&listener);
        inner.port.store(port, Ordering::SeqCst);
        inner.running.store(true, Ordering::SeqCst);
        let ctx = Ctx {
            inner: inner.clone(),
            bus: bus.clone(),
        };
        let accept = std::thread::Builder::new()
            .name("share-accept".into())
            .spawn(move || accept_loop(&ctx, &listener));
        let Ok(accept) = accept else {
            inner.running.store(false, Ordering::SeqCst);
            inner.port.store(0, Ordering::SeqCst);
            return false;
        };
        let dns = inner.needs_dns().then(|| {
            let inner2 = inner.clone();
            std::thread::Builder::new()
                .name("share-dns".into())
                .spawn(move || inner2.resolve_ice())
                .ok()
        });
        let mut r = Run {
            bus,
            accept: Some(accept),
            dns: dns.flatten(),
            tunnel_thread: None,
            tunnelled: None,
        };
        self.start_tunnel(&mut r, port);
        *run = Some(r);
        true
    }

    fn start_tunnel(&self, run: &mut Run, port: u16) {
        let inner = &self.inner;
        let source = lock(&inner.cloudflared).clone();
        let dirs = match source {
            CloudflaredSource::Disabled => {
                inner.set_tunnel(Tunnel::Off);
                return;
            }
            CloudflaredSource::Auto => {
                if std::env::var_os("HEARASIDE_NO_TUNNEL").is_some_and(|v| !v.is_empty()) {
                    inner.set_tunnel(Tunnel::Off); // tests: local links only
                    return;
                }
                tunnel::default_search_dirs()
            }
            CloudflaredSource::Dirs(d) => d,
        };
        let Some(exe) = tunnel::find_in(&dirs) else {
            inner.set_tunnel(Tunnel::Missing);
            return;
        };
        match tunnel::Tunnelled::start(&exe, port) {
            Ok((t, reader)) => {
                let t = Arc::new(t);
                *lock(&inner.tunnel_start) = Some(Instant::now());
                inner.set_tunnel(Tunnel::Starting);
                let inner2 = inner.clone();
                match std::thread::Builder::new()
                    .name("share-tunnel".into())
                    .spawn(move || tunnel_loop(&inner2, reader))
                {
                    Ok(h) => {
                        run.tunnel_thread = Some(h);
                        run.tunnelled = Some(t);
                    }
                    Err(_) => {
                        t.kill();
                        inner.set_tunnel(Tunnel::Failed);
                    }
                }
            }
            Err(_) => inner.set_tunnel(Tunnel::Failed),
        }
    }

    /// Ends every thread and closes every socket; the cloudflared process dies. Safe to call twice.
    pub fn stop(&self) {
        let mut run = lock(&self.run);
        self.stop_locked(&mut run);
    }

    fn stop_locked(&self, run: &mut Option<Run>) {
        let Some(mut r) = run.take() else { return };
        let inner = &self.inner;
        inner.running.store(false, Ordering::SeqCst);
        if let Some(t) = &r.tunnelled {
            t.kill(); // first: nothing below may wait on the tunnel
        }
        if let Some(h) = r.accept.take() {
            let _ = h.join();
        }
        // The accept thread is gone: nobody adds to the list any more. Sockets are shut down (never closed under a
        // running thread: C++ bug, the descriptor could be reused by the next connection), then each thread joined.
        let entries: Vec<ConnEntry> = std::mem::take(&mut *lock(&inner.conns));
        for e in &entries {
            sock::shutdown(&e.wake);
        }
        for mut e in entries {
            if let Some(h) = e.thread.take() {
                let _ = h.join();
            }
        }
        if let Some(h) = r.tunnel_thread.take() {
            let _ = h.join();
        }
        if let Some(h) = r.dns.take() {
            let _ = h.join(); // the one thread that can wait on the network: it is never left running
        }
        r.tunnelled = None; // kills (again) and reaps the process
        inner.set_public_base("");
        inner.set_tunnel(Tunnel::Off);
        *lock(&inner.tunnel_start) = None;
        let l = r.bus.layout();
        l.linkIn.active.store(0, Ordering::SeqCst);
        for f in &l.friends.0 {
            if f.state.load(Ordering::SeqCst) == FriendState::kFriendLive {
                f.state.store(FriendState::kFriendOffline, Ordering::SeqCst);
            }
        }
        inner.listeners.store(0, Ordering::SeqCst);
        inner.senders.store(0, Ordering::SeqCst);
        inner.friends_live.store(0, Ordering::SeqCst);
        inner.port.store(0, Ordering::SeqCst);
    }

    pub fn running(&self) -> bool {
        self.inner.is_running()
    }

    /// 0 while stopped.
    pub fn port(&self) -> u16 {
        self.inner.port()
    }

    // ---- links ------------------------------------------------------------------------------------------------

    /// `http://192.168.1.5:47810`
    pub fn lan_base(&self) -> String {
        self.inner.lan_base()
    }

    /// `http://26.x.x.x:47810` while Radmin VPN is up, else "".
    pub fn radmin_base(&self) -> String {
        self.inner.radmin_base()
    }

    /// `https://xxxx.trycloudflare.com`, "" = none.
    pub fn public_base(&self) -> String {
        self.inner.public_base()
    }

    /// Best link: public if ready, else LAN ("" while the tunnel starts).
    pub fn listen_url(&self) -> String {
        let b = self.inner.link_base(Instant::now());
        if b.is_empty() {
            b
        } else {
            format!("{b}/l/{}", self.inner.listen_token())
        }
    }

    /// `http://127.0.0.1:47810/l/...` for this computer.
    pub fn local_listen_url(&self) -> String {
        format!(
            "http://127.0.0.1:{}/l/{}",
            self.inner.port(),
            self.inner.listen_token()
        )
    }

    pub fn send_url(&self) -> String {
        let b = self.inner.link_base(Instant::now());
        if b.is_empty() {
            b
        } else {
            format!("{b}/s/{}", self.inner.send_token())
        }
    }

    /// Best link: public if ready, else LAN.
    pub fn friend_url(&self, token: &str) -> String {
        let b = self.inner.link_base(Instant::now());
        if b.is_empty() {
            b
        } else {
            format!("{b}/s/{token}")
        }
    }

    /// Over Radmin VPN (HAS Karaoke: browsers need https for the mic).
    pub fn radmin_friend_url(&self, token: &str) -> String {
        let b = self.inner.radmin_base();
        if b.is_empty() || self.inner.port() == 0 {
            String::new()
        } else {
            format!("{b}/s/{token}")
        }
    }

    pub fn tunnel(&self) -> Tunnel {
        self.inner.tunnel()
    }

    pub fn listeners(&self) -> i32 {
        self.inner.listeners.load(Ordering::SeqCst)
    }

    pub fn sender_active(&self) -> bool {
        self.inner.senders.load(Ordering::SeqCst) > 0
    }

    pub fn friends_connected(&self) -> i32 {
        self.inner.friends_live.load(Ordering::SeqCst)
    }

    // ---- settings ---------------------------------------------------------------------------------------------

    /// Web pages allowed to open the WebSockets besides this Hub's own addresses (the share web site of the permanent
    /// links). Requests without an Origin (App Audio, tools) are always allowed.
    pub fn set_allowed_origin(&self, origin: &str) {
        lock(&self.inner.urls).allowed_origin = origin.to_lowercase();
    }

    pub fn origin_allowed(&self, origin: &str) -> bool {
        self.inner.origin_allowed(origin)
    }

    /// The table of friend links. Entries with a slot outside 0..8 are ignored.
    pub fn set_friend_links(&self, links: Vec<FriendLink>) {
        let ok: Vec<FriendLink> = links
            .into_iter()
            .filter(|l| usize::try_from(l.slot).is_ok_and(|s| s < MAX_FRIENDS))
            .collect();
        *lock(&self.inner.friends) = ok;
    }

    /// End this friend's connection (removed from the room).
    pub fn kick_friend(&self, slot: i32) {
        if let Some(i) = slot_index(slot) {
            self.inner.friend_kick[i].store(true, Ordering::SeqCst);
        }
    }

    /// Forget the measured delay and measure again.
    pub fn remeasure_friend(&self, slot: i32) {
        if let Some(i) = slot_index(slot) {
            self.inner.friend_remeasure[i].store(true, Ordering::SeqCst);
        }
    }

    /// The listen links (`/l/<token>`) can be switched off without taking the friends away.
    pub fn set_listen_enabled(&self, on: bool) {
        self.inner.listen_enabled.store(on, Ordering::SeqCst);
    }

    pub fn listen_enabled(&self) -> bool {
        self.inner.listen_enabled.load(Ordering::SeqCst)
    }

    /// A new listen link while the server runs (friends stay connected); the old link stops working.
    pub fn set_listen_token(&self, token: &str) {
        lock(&self.inner.urls).listen_token = token.to_owned();
    }

    /// Where `start` looks for `cloudflared` (default: [`CloudflaredSource::Auto`]). Takes effect at the next `start`.
    pub fn set_cloudflared_source(&self, source: CloudflaredSource) {
        *lock(&self.inner.cloudflared) = source;
    }

    /// Running WebRTC worker threads: 0 after `stop()`, since every one is joined.
    #[doc(hidden)]
    pub fn rtc_threads(&self) -> usize {
        self.inner.rtc_threads.load(Ordering::SeqCst)
    }

    /// Switches the WebRTC peers of new sessions off (the WebSocket carries everything). For tests with many sessions.
    #[doc(hidden)]
    pub fn set_webrtc_enabled(&self, on: bool) {
        self.inner.rtc_enabled.store(on, Ordering::SeqCst);
    }

    /// ICE servers for the WebRTC peers (default: the two public STUN servers of the C++). Takes effect for the
    /// next session; host names are resolved by `start()`, so set them before it (a name added later is left out).
    pub fn set_ice_servers(&self, servers: Vec<IceServerConfig>) {
        *lock(&self.inner.ice) = servers;
    }

    // ---- friends: stats and echo canceller --------------------------------------------------------------------

    pub fn friend_stats(&self, slot: i32) -> FriendStats {
        let Some(i) = slot_index(slot) else {
            return FriendStats::default();
        };
        let n = &self.inner.friend_net[i];
        FriendStats {
            rtt_ms: n.rtt.load(),
            up_kbps: n.up.load(),
            down_kbps: n.down.load(),
            mic_gap_max_ms: n.mic_gap.load(),
            down_lag_max_ms: n.lag.load(),
            down_skips: n.skips.load(Ordering::Relaxed),
        }
    }

    /// Bytes held by the send queue of this friend's live session; stays below one audio packet plus control messages
    /// however slow the line is.
    #[doc(hidden)]
    pub fn friend_queue_bytes(&self, slot: i32) -> usize {
        slot_index(slot).map_or(0, |i| {
            self.inner.friend_queue_bytes[i].load(Ordering::Relaxed)
        })
    }

    /// Acoustic Echo Cancellation.
    pub fn set_friend_aec_mode(&self, slot: i32, mode: AecMode) {
        if let Some(i) = slot_index(slot) {
            lock(&self.inner.friend_echo[i]).set_mode(mode);
        }
    }

    pub fn friend_aec_mode(&self, slot: i32) -> AecMode {
        slot_index(slot).map_or(AecMode::Auto, |i| lock(&self.inner.friend_echo[i]).mode())
    }

    pub fn set_friend_leak_detected(&self, slot: i32, leak: bool) {
        if let Some(i) = slot_index(slot) {
            lock(&self.inner.friend_echo[i]).set_leak_detected(leak);
        }
    }

    pub fn friend_leak_detected(&self, slot: i32) -> bool {
        slot_index(slot).is_some_and(|i| lock(&self.inner.friend_echo[i]).leak_detected())
    }

    pub fn friend_erle_db(&self, slot: i32) -> f32 {
        slot_index(slot).map_or(0.0, |i| lock(&self.inner.friend_echo[i]).erle_db())
    }

    // ---- statics ----------------------------------------------------------------------------------------------

    /// 26 characters from the OS's secure random source (130 bits); "" if it fails.
    pub fn new_token() -> String {
        // links live on the internet for months now (permanent links): 26 x 5 bits, never a PRNG
        const ABC: &[u8; 32] = b"abcdefghijkmnpqrstuvwxyz23456789"; // no 0/o/1/l
        let mut bytes = [0u8; 26];
        if !secure_random(&mut bytes) {
            return String::new();
        }
        bytes
            .iter()
            .map(|b| ABC[usize::from(b & 31)] as char)
            .collect()
    }

    /// 64 hex characters (256 bits), proves who owns the permanent links; "" if the random source fails.
    pub fn new_secret() -> String {
        let mut bytes = [0u8; 32];
        if !secure_random(&mut bytes) {
            return String::new();
        }
        bytes.iter().map(|b| format!("{b:02x}")).collect()
    }

    pub fn find_cloudflared() -> Option<PathBuf> {
        tunnel::find_in(&tunnel::default_search_dirs())
    }

    /// macOS / Linux: writing to a socket the browser already closed raises SIGPIPE, which would end the DAW. Ignored
    /// (once); `start()` calls this. A Rust runtime inside a plug-in does not do it by itself.
    pub fn ignore_broken_pipes() {
        sock::ignore_sigpipe();
    }
}

/// Fills `dst` from the OS's secure random source.
pub fn secure_random(dst: &mut [u8]) -> bool {
    util::secure_random(dst)
}

/// The first `cloudflared` in the usual places.
pub fn find_cloudflared() -> Option<PathBuf> {
    ShareServer::find_cloudflared()
}

fn slot_index(slot: i32) -> Option<usize> {
    usize::try_from(slot).ok().filter(|&s| s < MAX_FRIENDS)
}

// ---- connections ----------------------------------------------------------------------------------------------

/// Whether one more connection from `peer` fits. Pure so the limits can be tested: `conns` are the live ones as
/// `(peer, head_done)`.
fn admit(conns: &[(String, bool)], peer: &str) -> bool {
    if conns.len() >= MAX_CONNS {
        return false;
    }
    // one LAN device may not take every place; the tunnel's connections all come from 127.0.0.1
    let via_tunnel = peer.starts_with("127.");
    if !via_tunnel && conns.iter().filter(|(p, _)| p == peer).count() >= MAX_PER_PEER {
        return false;
    }
    // endless half-sent requests must not use up every place (the tunnel hides who sends them)
    conns.iter().filter(|(_, head_done)| !head_done).count() < MAX_PENDING
}

fn reap(inner: &Inner) {
    let finished: Vec<ConnEntry> = {
        let mut list = lock(&inner.conns);
        let mut keep = Vec::with_capacity(list.len());
        let mut out = Vec::new();
        for e in list.drain(..) {
            if e.info.done.load(Ordering::SeqCst) {
                out.push(e);
            } else {
                keep.push(e);
            }
        }
        *list = keep;
        out
    };
    // joined outside the lock: a finishing thread may still want it
    for mut e in finished {
        if let Some(h) = e.thread.take() {
            let _ = h.join();
        }
    }
}

struct DoneGuard(Arc<ConnInfo>);

impl Drop for DoneGuard {
    fn drop(&mut self) {
        self.0.done.store(true, Ordering::SeqCst);
    }
}

fn accept_loop(ctx: &Ctx, listener: &TcpListener) {
    let inner = &ctx.inner;
    while inner.is_running() {
        if !sock::listener_readable(listener, 50) {
            reap(inner);
            continue;
        }
        let (stream, addr) = match listener.accept() {
            Ok(v) => v,
            Err(_) => {
                sock::sleep_ms(20);
                continue;
            }
        };
        reap(inner);
        if !inner.is_running() {
            break;
        }
        let _ = stream.set_nonblocking(true);
        let _ = stream.set_nodelay(true);
        sock::no_inherit_stream(&stream);
        sock::no_sigpipe(&stream);
        let info = Arc::new(ConnInfo {
            peer: addr.ip().to_string(),
            client: Mutex::new(addr.ip().to_string()),
            done: AtomicBool::new(false),
            head_done: AtomicBool::new(false),
        });
        let Ok(wake) = stream.try_clone() else {
            continue;
        };
        let mut list = lock(&inner.conns);
        let live: Vec<(String, bool)> = list
            .iter()
            .filter(|e| !e.info.done.load(Ordering::SeqCst))
            .map(|e| (e.info.peer.clone(), e.info.head_done.load(Ordering::SeqCst)))
            .collect();
        // a refused connection is just closed (`stream` drops)
        if list.len() >= MAX_CONNS || !admit(&live, &info.peer) {
            continue;
        }
        let (ctx2, info2) = (ctx.clone(), info.clone());
        let spawned = std::thread::Builder::new()
            .name("share-conn".into())
            .spawn(move || {
                let _done = DoneGuard(info2.clone());
                serve(&ctx2, &info2, &stream);
                sock::shutdown(&stream);
            });
        if let Ok(h) = spawned {
            list.push(ConnEntry {
                info,
                wake,
                thread: Some(h),
            });
        }
    }
}

enum HeadError {
    /// timed out, closed, or the server is stopping: just closed
    Gone,
    /// more than 8 KiB without the empty line
    TooLarge,
}

/// The request head (up to the empty line), read without taking a byte of what follows. 5 s for all of it: one byte
/// now and then must not hold a connection for hours. Longer than 8 KiB is `TooLarge` (C++ bug: it went on with a cut
/// head).
fn read_head(s: &TcpStream, alive: &dyn Fn() -> bool) -> Result<Vec<u8>, HeadError> {
    let start = Instant::now();
    let mut head: Vec<u8> = Vec::new();
    let mut buf = [0u8; MAX_HEAD];
    loop {
        if !alive() || start.elapsed() > HEAD_TIMEOUT {
            return Err(HeadError::Gone);
        }
        if sock::wait_readable(s, 100) < 0 {
            return Err(HeadError::Gone);
        }
        let n = match s.peek(&mut buf[..MAX_HEAD - head.len()]) {
            Ok(0) => return Err(HeadError::Gone),
            Ok(n) => n,
            Err(e) if e.kind() == std::io::ErrorKind::WouldBlock => continue,
            Err(_) => return Err(HeadError::Gone),
        };
        // the terminator may straddle what we already have and what we peeked
        let tail = head.len().min(3);
        let mut scan = head[head.len() - tail..].to_vec();
        scan.extend_from_slice(&buf[..n]);
        let take = scan
            .windows(4)
            .position(|w| w == b"\r\n\r\n")
            .map_or(n, |p| p + 4 - tail);
        let mut chunk = vec![0u8; take];
        if !sock::read_exact(s, &mut chunk, 1000, alive) {
            return Err(HeadError::Gone);
        }
        head.extend_from_slice(&chunk);
        if head.ends_with(b"\r\n\r\n") {
            return Ok(head);
        }
        if head.len() >= MAX_HEAD {
            return Err(HeadError::TooLarge);
        }
    }
}

/// Throws away what the client is still sending (briefly), so closing does not reset the connection and the answer
/// we just wrote is not lost on the way.
fn drain(s: &TcpStream) {
    let mut buf = [0u8; 4096];
    for _ in 0..64 {
        if sock::wait_readable(s, 50) != 1 || sock::try_read(s, &mut buf) <= 0 {
            break;
        }
    }
}

pub(crate) fn respond(s: &TcpStream, code: u16, ctype: &str, body: &[u8]) -> bool {
    let reason = match code {
        200 => "OK",
        403 => "Forbidden",
        431 => "Request Header Fields Too Large",
        503 => "Service Unavailable",
        _ => "Not Found",
    };
    let mut out = format!(
        "HTTP/1.1 {code} {reason}\r\nContent-Type: {ctype}\r\nContent-Length: {}\r\nCache-Control: no-store\r\nConnection: close\r\nX-Content-Type-Options: nosniff\r\n\r\n",
        body.len()
    )
    .into_bytes();
    out.extend_from_slice(body);
    sock::write_all(s, &out)
}

fn respond_text(s: &TcpStream, code: u16, text: &str) -> bool {
    respond(s, code, "text/plain; charset=utf-8", text.as_bytes())
}

struct Request {
    path: String,
    key: String,
    origin: String,
    via: String,
}

/// `None` = not a GET.
fn parse_request(head: &[u8]) -> Option<Request> {
    let text = String::from_utf8_lossy(head);
    let lines: Vec<&str> = text.split(['\r', '\n']).collect();
    let mut first = lines.first()?.split(' ').filter(|t| !t.is_empty());
    if first.next()? != "GET" {
        return None;
    }
    let path = first.next()?.split('?').next().unwrap_or("").to_owned();
    let mut r = Request {
        path,
        key: String::new(),
        origin: String::new(),
        via: String::new(),
    };
    for l in &lines {
        let value = |name: &str| {
            let (n, v) = l.split_once(':')?;
            n.eq_ignore_ascii_case(name.trim_end_matches(':'))
                .then(|| v.trim().to_owned())
        };
        if let Some(v) = value("Cf-Connecting-Ip") {
            r.via = v;
        }
        if let Some(v) = value("Sec-WebSocket-Key") {
            r.key = v;
        }
        if let Some(v) = value("Origin") {
            r.origin = v;
        }
    }
    Some(r)
}

const NOT_VALID: &str = "HEARASIDE: this link is not valid (any more).";

fn serve(ctx: &Ctx, conn: &ConnInfo, s: &TcpStream) {
    let inner = &ctx.inner;
    let alive = || inner.is_running();
    let head = match read_head(s, &alive) {
        Ok(h) => h,
        Err(HeadError::Gone) => return,
        Err(HeadError::TooLarge) => {
            respond_text(s, 431, "HEARASIDE: the request header is too large.");
            drain(s);
            return;
        }
    };
    let Some(req) = parse_request(&head) else {
        respond(s, 404, "text/plain", b"");
        return;
    };
    conn.head_done.store(true, Ordering::SeqCst);
    let (path, key) = (req.path.as_str(), req.key.as_str());
    if conn.peer.starts_with("127.") {
        // through the tunnel: one visitor may not take every place either
        if !req.via.is_empty() && req.via.len() <= 45 {
            let list = lock(&inner.conns);
            *lock(&conn.client) = req.via.clone();
            let same = list
                .iter()
                .filter(|o| {
                    !o.info.done.load(Ordering::SeqCst)
                        && o.info.head_done.load(Ordering::SeqCst)
                        && *lock(&o.info.client) == req.via
                })
                .count();
            drop(list);
            if same > MAX_PER_PEER {
                respond_text(s, 503, "HEARASIDE: too many connections from your address.");
                return;
            }
        }
    }
    // a page from another site must not open the audio (or the mic input) behind the user's back
    if !key.is_empty() && !inner.origin_allowed(&req.origin) {
        respond_text(s, 403, "HEARASIDE: this page may not connect.");
        return;
    }
    let upgrade = || sock::write_all(s, ws::upgrade_response(key).as_bytes());

    let send_token = inner.send_token();
    // friends: one link each (and a tiny JSON for the page: the friend's name, who invited them)
    if path.starts_with("/s/") || path.starts_with("/ws/s/") || path.starts_with("/info/s/") {
        let tok = path.rsplit('/').next().unwrap_or("");
        if (send_token.is_empty() || !ct_eq(tok.as_bytes(), send_token.as_bytes()))
            && let Some((slot, id, name, host)) = inner.friend_slot_for(tok)
        {
            if path.starts_with("/s/") {
                respond(s, 200, "text/html; charset=utf-8", SEND_HTML);
                return;
            }
            if path.starts_with("/info/s/") {
                let json = format!(
                    "{{\"name\": {}, \"host\": {}}}",
                    ws::json_str(&name),
                    ws::json_str(&host)
                );
                respond(s, 200, "application/json; charset=utf-8", json.as_bytes());
                return;
            }
            if !key.is_empty() {
                if upgrade() {
                    friend::friend_session(ctx, s, slot, id, tok);
                }
                return;
            }
        }
    }
    if !inner.listen_enabled.load(Ordering::SeqCst)
        && (path.starts_with("/l/") || path.starts_with("/ws/l/"))
    {
        respond_text(s, 404, NOT_VALID);
        return;
    }
    let lt = inner.listen_token();
    if path_is(path, "/l/", &lt) {
        respond(s, 200, "text/html; charset=utf-8", LISTEN_HTML);
        return;
    }
    if path_is(path, "/s/", &send_token) {
        respond(s, 200, "text/html; charset=utf-8", SEND_HTML);
        return;
    }
    if !key.is_empty() && path_is(path, "/ws/l/", &lt) {
        // counted before the upgrade, so many connections at once cannot all pass the check (C++ bug: check, then count)
        let guard = CountGuard::new(&inner.listeners);
        if guard.count() > MAX_LISTENERS {
            drop(guard);
            respond_text(
                s,
                503,
                "HEARASIDE: too many listeners right now, try again later.",
            );
            return;
        }
        if upgrade() {
            listener::stream_to(ctx, s);
        }
        return;
    }
    if !key.is_empty() && path_is(path, "/ws/s/", &send_token) {
        if upgrade() {
            sender::receive_from(ctx, s);
        }
        return;
    }
    respond_text(s, 404, NOT_VALID);
}

/// cloudflared quick tunnel: prints `https://<random>.trycloudflare.com` once it is reachable.
fn tunnel_loop(inner: &Inner, mut reader: std::io::PipeReader) {
    use std::io::Read;
    let mut text: Vec<u8> = Vec::new();
    let mut buf = [0u8; 1024];
    while inner.is_running() {
        let n = match reader.read(&mut buf) {
            Ok(n) if n > 0 => n,
            _ => break, // exited (or killed by stop())
        };
        if inner.tunnel() == Tunnel::Ready {
            continue; // keep draining the pipe
        }
        text.extend_from_slice(&buf[..n]);
        if let Some(url) = util::find_trycloudflare_url(&text) {
            inner.set_public_base(&url);
            inner.set_tunnel(Tunnel::Ready);
        }
        if text.len() > 65536 {
            text.drain(..text.len() - 4096);
        }
    }
    if inner.is_running() {
        inner.set_public_base("");
        inner.set_tunnel(Tunnel::Failed);
    }
}

#[cfg(test)]
mod tests {
    use super::*;

    fn conns(peers: &[(&str, bool)]) -> Vec<(String, bool)> {
        peers.iter().map(|(p, h)| ((*p).to_owned(), *h)).collect()
    }

    #[test]
    fn admission_limits() {
        // one LAN device gets 6 places
        let six = conns(&[("192.168.1.9", true); 6]);
        assert!(!admit(&six, "192.168.1.9"));
        assert!(admit(&six, "192.168.1.10"));
        assert!(admit(&six[..5], "192.168.1.9"));
        // the tunnel's connections all come from loopback: no per-peer limit
        let local = conns(&[("127.0.0.1", true); 20]);
        assert!(admit(&local, "127.0.0.1"));
        // 24 connections without a whole head fill the pending places
        let pending = conns(&[("10.0.0.1", false); 24]);
        assert!(!admit(&pending, "10.0.0.2"));
        assert!(admit(&pending[..23], "10.0.0.2"));
        // 40 in all
        let all: Vec<(String, bool)> = (0..40).map(|i| (format!("10.0.{i}.1"), true)).collect();
        assert!(!admit(&all, "10.9.9.9"));
        assert!(admit(&all[..39], "10.9.9.9"));
    }

    #[test]
    fn request_parsing() {
        let r = parse_request(
            b"GET /ws/l/abc?x=1 HTTP/1.1\r\nHost: h\r\nsec-websocket-key:  KEY== \r\nORIGIN: http://a:1\r\ncf-connecting-ip: 1.2.3.4\r\n\r\n",
        )
        .unwrap();
        assert_eq!(r.path, "/ws/l/abc");
        assert_eq!(r.key, "KEY==");
        assert_eq!(r.origin, "http://a:1");
        assert_eq!(r.via, "1.2.3.4");
        assert!(parse_request(b"POST / HTTP/1.1\r\n\r\n").is_none());
        assert!(parse_request(b"get / HTTP/1.1\r\n\r\n").is_none());
        assert!(parse_request(b"\r\n\r\n").is_none());
        // repeated spaces are fine, an Origin inside a value is not a header
        let r = parse_request(b"GET   /x   HTTP/1.1\r\nX-Note: Origin: nope\r\n\r\n").unwrap();
        assert_eq!(r.path, "/x");
        assert_eq!(r.origin, "");
    }

    #[test]
    fn link_base_waits_for_the_tunnel_then_falls_back_to_lan() {
        let inner = Inner::new();
        inner.port.store(47810, Ordering::SeqCst);
        inner.set_tunnel(Tunnel::Starting);
        let t0 = Instant::now();
        *lock(&inner.tunnel_start) = Some(t0);
        assert_eq!(inner.link_base(t0 + Duration::from_secs(1)), "");
        assert!(
            inner
                .link_base(t0 + TUNNEL_WAIT + Duration::from_millis(1))
                .starts_with("http://")
        );
        inner.set_public_base("https://a-b.trycloudflare.com");
        assert_eq!(inner.link_base(t0), "https://a-b.trycloudflare.com");
        inner.set_public_base("");
        inner.set_tunnel(Tunnel::Missing);
        assert!(inner.link_base(t0).ends_with(":47810"));
    }

    #[test]
    fn friend_table_ignores_bad_slots_and_short_tokens() {
        let s = ShareServer::new();
        let link = |slot, token: &str| FriendLink {
            slot,
            id: 7,
            token: token.into(),
            name: "Ann".into(),
            host: String::new(),
        };
        let t = "abcdefghijklmnopqrst";
        s.set_friend_links(vec![link(8, t), link(-1, t), link(3, t), link(2, "short")]);
        assert_eq!(s.inner.friend_slot_for(t).map(|f| f.0), Some(3));
        assert!(s.inner.friend_slot_for("short").is_none());
        assert!(s.inner.friend_slot_for("abcdefghijklmnopqrsu").is_none());
    }

    #[test]
    fn ice_servers_reach_the_peer_as_addresses_only() {
        let inner = Inner::new();
        let cfg = |url: &str| IceServerConfig {
            url: url.into(),
            username: String::new(),
            password: String::new(),
        };
        // default config: two host names, nothing resolved yet -> no DNS in the peer, host candidates only
        assert!(inner.needs_dns());
        let none = inner.ice_servers();
        assert_eq!(none.len(), 1);
        assert_eq!(none[0].url, "stun:127.0.0.1:9");
        // addresses pass as they are
        *lock(&inner.ice) = vec![cfg("stun:192.0.2.7:3478"), cfg("stun:[2001:db8::1]:19302")];
        assert!(!inner.needs_dns());
        let urls: Vec<_> = inner.ice_servers().into_iter().map(|c| c.url).collect();
        assert_eq!(urls, ["stun:192.0.2.7:3478", "stun:[2001:db8::1]:19302"]);
        // a name is resolved by `resolve_ice` and replaced by its addresses
        *lock(&inner.ice) = vec![cfg("stun:localhost"), cfg("stun:192.0.2.7")];
        assert!(inner.needs_dns());
        inner.resolve_ice();
        let urls: Vec<_> = inner.ice_servers().into_iter().map(|c| c.url).collect();
        assert!(
            urls.len() >= 2 && urls.iter().all(|u| !u.contains("localhost")),
            "{urls:?}"
        );
        assert!(
            urls.iter()
                .any(|u| u == "stun:127.0.0.1:3478" || u == "stun:[::1]:3478"),
            "{urls:?}"
        );
        assert!(urls.contains(&"stun:192.0.2.7".to_owned()));
        // turn: is passed on untouched (the peer refuses it, still without DNS)
        *lock(&inner.ice) = vec![cfg("turn:example.test:3478")];
        assert_eq!(inner.ice_servers()[0].url, "turn:example.test:3478");
        assert_eq!(
            stun_authority("stun:h?transport=udp").as_deref(),
            Some("h:3478")
        );
        assert_eq!(stun_authority("stun:[::1]").as_deref(), Some("[::1]:3478"));
        assert_eq!(stun_authority("turn:h"), None);
    }

    #[test]
    fn tokens_have_the_documented_shape() {
        let t = ShareServer::new_token();
        assert_eq!(t.len(), 26);
        assert!(
            t.bytes()
                .all(|b| b"abcdefghijkmnpqrstuvwxyz23456789".contains(&b))
        );
        assert_ne!(t, ShareServer::new_token());
        let s = ShareServer::new_secret();
        assert_eq!(s.len(), 64);
        assert!(
            s.bytes()
                .all(|b| b.is_ascii_digit() || (b'a'..=b'f').contains(&b))
        );
        assert_ne!(s, ShareServer::new_secret());
    }
}
