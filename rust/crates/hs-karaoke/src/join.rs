//! Joining a room: HAS Karaoke as a friend of somebody's Hub (or of another HAS Karaoke hosting a room). The mic goes
//! up as Opus (HRA3, 5 ms packets with FEC) or PCM (HRA2, for an old Hub), the room's mix comes down through a jitter
//! buffer into the headphones. WebSocket first, then a WebRTC DataChannel (UDP) when it opens.
//! Port of `apps/karaoke/JoinClient.{h,cpp}`.
//!
//! [`join_client`] makes two halves: [`JoinClient`] for the UI and [`JoinAudio`] for the audio callback. One network
//! thread owns the WebSocket, the WebRTC peer, the Opus codecs and the echo canceller; it meets the audio thread only
//! in two lock-free rings (`hs-bus` rings, like the C++). The UI reads `state()` / `room_info()` on its timer instead
//! of the C++ `onStateChanged` posted with `callAsync`.
//!
//! C++ bugs this fixes:
//! 1. a late or repeated packet (the DataChannel is unordered) was played out of place and moved the sequence back,
//!    so the next packets filled up to 10 blocks of loss concealment over audio that had arrived. Dropped here with
//!    `hs_net::wire::seq_gap`, and the packet just before a gap comes back from the next one's in-band FEC (the fix
//!    the Hub got in 1.0.8, never made here);
//! 2. `disconnect` reset the jitter buffer from the UI thread while the audio thread used it; the audio thread
//!    resets it here;
//! 3. `setState` posted `callAsync([this] ...)`: a client destroyed first was used after free;
//! 4. the decoder, codec name and sequence were shared by the WebSocket reader and the DataChannel callback thread
//!    behind one mutex but read by `roomInfo` without it (data race); here one thread owns them;
//! 5. every received packet allocated a `std::vector` for the echo reference; here the decode and reference buffers
//!    are made once per connection (the WebSocket / DataChannel libraries still allocate per message, off the audio
//!    thread as before);
//! 6. `hearaside://join?url=` links (what a hosting HAS Karaoke hands out) were only understood by the window, not by
//!    `connect`; both link forms work here.
//!
//! DNS (the Hub's host, the STUN servers) runs on throwaway threads with a 4 s limit, so `disconnect()` never waits
//! on a resolver that hangs.
//!
//! Known gap, as before: no proxy for the WebSocket (WinHTTP used the system proxy; `/api/resolve` goes through
//! `hs-app`'s client, which does).
use crate::AtomicF32;
use hs_bus::protocol::ChannelRing;
use hs_bus::ring::{ring_read_at, ring_write};
use hs_engine::friend_reader::FriendReader;
use hs_net::aec::{AecMode, FriendEcho};
use hs_net::opus::{App, OpusDec, OpusEnc};
use hs_net::rtc::{IceServerConfig, RtcEvent, RtcPeer, RtcPeerConfig};
use hs_net::wire::{
    CODEC_OPUS, CODEC_PCM16, FLAG_FEC, HRA3_HEADER_BYTES, Hra3Header, MAGIC2, MAGIC3, parse_hra3,
    seq_gap, write_hra3,
};
use std::io::{ErrorKind, Read, Write};
use std::net::{SocketAddr, TcpStream, ToSocketAddrs};
use std::sync::atomic::{AtomicBool, AtomicI32, AtomicU8, AtomicU32, AtomicU64, Ordering::*};
use std::sync::{Arc, Mutex};
use std::thread::JoinHandle;
use std::time::Duration;
use tungstenite::{Message, WebSocket};

#[derive(Debug, Clone, Copy, PartialEq, Eq)]
pub enum ConnectionState {
    Disconnected,
    Resolving,
    Connecting,
    Handshaking,
    Live,
    Reconnecting,
    Failed,
}

const STATES: [ConnectionState; 7] = [
    ConnectionState::Disconnected,
    ConnectionState::Resolving,
    ConnectionState::Connecting,
    ConnectionState::Handshaking,
    ConnectionState::Live,
    ConnectionState::Reconnecting,
    ConnectionState::Failed,
];

#[derive(Debug, Clone, PartialEq)]
pub struct RoomInfo {
    pub name: String,
    pub host: String,
    pub token: String,
    /// what the Hub measured (`{"delay": ms}`), -1 = not yet
    pub delay_ms: f32,
    pub rtt_ms: f32,
    pub buffer_ms: f32,
    pub jitter_p95_ms: f32,
    pub underruns: u32,
    pub concealed_ms: f32,
    /// "opus" or "pcm16"
    pub codec: String,
    pub speaker_mode: bool,
    pub erle_db: f32,
    pub leak_detected: bool,
    pub is_udp: bool,
    /// "WebRTC UDP", "WebSocket Opus" or "WebSocket PCM"
    pub transport: String,
}

impl Default for RoomInfo {
    fn default() -> Self {
        Self {
            name: String::new(),
            host: String::new(),
            token: String::new(),
            delay_ms: -1.0,
            rtt_ms: 0.0,
            buffer_ms: 0.0,
            jitter_p95_ms: -1.0,
            underruns: 0,
            concealed_ms: 0.0,
            codec: "pcm16".into(),
            speaker_mode: false,
            erle_db: 0.0,
            leak_detected: false,
            is_udp: false,
            transport: String::new(),
        }
    }
}

/// Largest Opus packet in frames (120 ms at 48 kHz); bigger HRA3 frame counts are cut to it.
const MAX_FRAMES: usize = 5760;

struct Shared {
    state: AtomicU8,
    quit: AtomicBool,
    info: Mutex<RoomInfo>,
    // down: network thread -> ring -> audio thread (jitter buffer)
    down: Box<ChannelRing>,
    down_write: AtomicU64,
    down_rate: AtomicU32,
    reset_reader: AtomicBool,
    // up: audio thread -> ring -> network thread
    up: Box<ChannelRing>,
    up_write: AtomicU64,
    echo_pos: AtomicU64,
    // controls
    speaker_mode: AtomicBool,
    local_delay: AtomicI32,
    is_udp: AtomicBool,
    // metrics the audio / network threads publish
    buffer_ms: AtomicF32,
    jitter_p95_ms: AtomicF32,
    underruns: AtomicU32,
    concealed_ms: AtomicF32,
    erle_db: AtomicF32,
    leak: AtomicBool,
}

impl Shared {
    fn set_state(&self, s: ConnectionState) {
        self.state.store(s as u8, Release);
    }
    fn quitting(&self) -> bool {
        self.quit.load(Acquire)
    }
    /// Waits `ms` in 100 ms steps; false when asked to quit.
    fn nap(&self, ms: u64) -> bool {
        for _ in 0..ms / 100 {
            if self.quitting() {
                return false;
            }
            std::thread::sleep(Duration::from_millis(100));
        }
        !self.quitting()
    }
}

/// Makes the UI half and the audio half for one device setup (allocates; call again when the rate changes).
pub fn join_client(sample_rate: f64, max_block: usize) -> (JoinClient, JoinAudio) {
    let sr = if sample_rate > 0.0 {
        sample_rate
    } else {
        48000.0
    };
    let shared = Arc::new(Shared {
        state: AtomicU8::new(ConnectionState::Disconnected as u8),
        quit: AtomicBool::new(false),
        info: Mutex::new(RoomInfo::default()),
        down: ChannelRing::boxed(),
        down_write: AtomicU64::new(0),
        down_rate: AtomicU32::new(48000),
        reset_reader: AtomicBool::new(false),
        up: ChannelRing::boxed(),
        up_write: AtomicU64::new(0),
        echo_pos: AtomicU64::new(0),
        speaker_mode: AtomicBool::new(false),
        local_delay: AtomicI32::new(480),
        is_udp: AtomicBool::new(false),
        buffer_ms: AtomicF32::new(0.0),
        jitter_p95_ms: AtomicF32::new(-1.0),
        underruns: AtomicU32::new(0),
        concealed_ms: AtomicF32::new(0.0),
        erle_db: AtomicF32::new(0.0),
        leak: AtomicBool::new(false),
    });
    let mut reader = FriendReader::new();
    reader.prepare(sr, max_block);
    (
        JoinClient {
            shared: shared.clone(),
            worker: None,
        },
        JoinAudio {
            shared,
            reader,
            max_block,
        },
    )
}

/// The UI half.
pub struct JoinClient {
    shared: Arc<Shared>,
    worker: Option<JoinHandle<()>>,
}

impl JoinClient {
    /// Connects (and keeps reconnecting) to a friend link: `https://.../s/<token>`, a permanent link, a bare token,
    /// `has://join?u=...` or `hearaside://join?url=...`.
    pub fn connect(&mut self, url_or_token: &str) {
        self.disconnect();
        self.shared.quit.store(false, Release);
        self.shared.set_state(ConnectionState::Connecting);
        let shared = self.shared.clone();
        let input = url_or_token.to_owned();
        self.worker = std::thread::Builder::new()
            .name("HAS Karaoke join".into())
            .spawn(move || run(&shared, &input))
            .ok();
        if self.worker.is_none() {
            self.shared.set_state(ConnectionState::Failed);
        }
    }

    pub fn disconnect(&mut self) {
        self.shared.quit.store(true, Release);
        if let Some(w) = self.worker.take() {
            let _ = w.join();
        }
        self.shared.quit.store(false, Release);
        self.shared.is_udp.store(false, Relaxed);
        self.shared.reset_reader.store(true, Release);
        self.shared.set_state(ConnectionState::Disconnected);
    }

    pub fn state(&self) -> ConnectionState {
        STATES[usize::from(self.shared.state.load(Acquire)).min(STATES.len() - 1)]
    }

    pub fn is_live(&self) -> bool {
        self.state() == ConnectionState::Live
    }

    pub fn is_udp(&self) -> bool {
        self.shared.is_udp.load(Relaxed)
    }

    pub fn room_info(&self) -> RoomInfo {
        let s = &*self.shared;
        let mut info = s.info.lock().map(|i| i.clone()).unwrap_or_default();
        info.buffer_ms = s.buffer_ms.load();
        info.jitter_p95_ms = s.jitter_p95_ms.load();
        info.underruns = s.underruns.load(Relaxed);
        info.concealed_ms = s.concealed_ms.load();
        info.speaker_mode = s.speaker_mode.load(Relaxed);
        info.erle_db = s.erle_db.load();
        info.leak_detected = s.leak.load(Relaxed);
        info.is_udp = s.is_udp.load(Relaxed);
        info.transport = transport(info.is_udp, &info.codec);
        info
    }

    /// Speakers instead of headphones: the echo canceller runs on the mic, and the Hub is told.
    pub fn set_speaker_mode(&self, on: bool) {
        self.shared.speaker_mode.store(on, Relaxed);
    }

    pub fn is_speaker_mode(&self) -> bool {
        self.shared.speaker_mode.load(Relaxed)
    }

    /// The device's total in + out latency; with ~3 ms of air (speaker to mic) it is where the echo is looked for.
    pub fn set_local_latency_ms(&self, total_ms: f64) {
        let d = ((total_ms + 3.0) * 48.0) as i32;
        self.shared.local_delay.store(d.clamp(64, 9600), Relaxed);
    }
}

impl Drop for JoinClient {
    fn drop(&mut self) {
        self.disconnect();
    }
}

fn transport(udp: bool, codec: &str) -> String {
    if udp {
        "WebRTC UDP"
    } else if codec.eq_ignore_ascii_case("opus") {
        "WebSocket Opus"
    } else {
        "WebSocket PCM"
    }
    .into()
}

/// The audio-callback half.
pub struct JoinAudio {
    shared: Arc<Shared>,
    reader: FriendReader,
    max_block: usize,
}

impl JoinAudio {
    /// Audio thread: the room's mix (music + the other singers) for the headphones. False = nothing to play yet
    /// (the outputs are silent). No allocation, no lock. A block longer than `max_block` runs in pieces (the jitter
    /// buffer takes at most that many frames per call).
    pub fn pull_downstream(&mut self, out_l: &mut [f32], out_r: &mut [f32], n: usize) -> bool {
        let s = &*self.shared;
        if s.reset_reader.swap(false, Acquire) {
            self.reader.reset();
        }
        let n = n.min(out_l.len()).min(out_r.len());
        let step = self.max_block.max(1);
        let mut ok = n > 0;
        for at in (0..n).step_by(step) {
            let end = (at + step).min(n);
            ok &= self.reader.pull(
                &s.down,
                &s.down_write,
                s.down_rate.load(Relaxed),
                [&mut out_l[at..end], &mut out_r[at..end]],
                end - at,
            );
        }
        s.buffer_ms.store(self.reader.buffer_ms() as f32);
        s.jitter_p95_ms.store(self.reader.jitter_p95_ms() as f32);
        s.underruns.store(self.reader.underruns(), Relaxed);
        s.concealed_ms.store(self.reader.concealed_ms() as f32);
        ok
    }

    /// Audio thread: the singer's mic (after the vocal chain's network send) for the Hub. `playback_pos` is where the
    /// music is, sent along so the Hub can line the voice up. No allocation, no lock.
    pub fn push_upstream(&mut self, mic: &[f32], n: usize, playback_pos: u64) {
        let n = n.min(mic.len());
        if n == 0 {
            return;
        }
        ring_write(
            &self.shared.up,
            &self.shared.up_write,
            &[&mic[..n]],
            n as u32,
        );
        self.shared.echo_pos.store(playback_pos, Relaxed);
    }
}

// ---- links ------------------------------------------------------------------------------------------------------

/// Where to connect, from what the user pasted.
#[derive(Debug, Clone, PartialEq)]
struct Link {
    /// `ws://` or `wss://` URL of the friend socket (`/ws/s/<token>`)
    ws: String,
    /// `https://host[:port]` of a permanent link's site, when it has to be resolved first
    site: Option<String>,
    token: String,
}

fn percent_decode(s: &str) -> String {
    let b = s.as_bytes();
    let mut out = Vec::with_capacity(b.len());
    let mut i = 0;
    while i < b.len() {
        let hex = |c: u8| (c as char).to_digit(16);
        if b[i] == b'%'
            && let (Some(h), Some(l)) = (
                b.get(i + 1).and_then(|c| hex(*c)),
                b.get(i + 2).and_then(|c| hex(*c)),
            )
        {
            out.push((h * 16 + l) as u8);
            i += 3;
        } else {
            out.push(if b[i] == b'+' { b' ' } else { b[i] });
            i += 1;
        }
    }
    String::from_utf8_lossy(&out).into_owned()
}

fn strip_prefix_ci<'a>(s: &'a str, prefix: &str) -> Option<&'a str> {
    let head = s.get(..prefix.len())?;
    head.eq_ignore_ascii_case(prefix)
        .then(|| &s[prefix.len()..])
}

fn parse_link(input: &str) -> Option<Link> {
    let mut clean = input.trim().to_owned();
    for prefix in ["has://join?u=", "hearaside://join?url="] {
        if let Some(rest) = strip_prefix_ci(&clean, prefix) {
            clean = percent_decode(rest);
            break;
        }
    }
    let after_slash = clean.rsplit('/').next().unwrap_or("");
    let token = after_slash.split('?').next().unwrap_or("").to_owned();

    let lower = clean.to_ascii_lowercase();
    if !["http://", "https://", "ws://", "wss://"]
        .iter()
        .any(|p| lower.starts_with(p))
    {
        clean = format!("https://trycloudflare.com/s/{clean}");
    }
    let (scheme, rest) = clean.split_once("://")?;
    let secure = matches!(scheme.to_ascii_lowercase().as_str(), "https" | "wss");
    let (authority, path) = match rest.find('/') {
        Some(i) => (&rest[..i], &rest[i..]),
        None => (rest, "/"),
    };
    let host = authority.rsplit_once(':').map_or(authority, |(h, _)| h);
    if host.is_empty() {
        return None;
    }
    let ws_path = path.replacen("/s/", "/ws/s/", 1);
    let ws = format!(
        "{}://{authority}{ws_path}",
        if secure { "wss" } else { "ws" }
    );
    let directory = secure && !host.contains("trycloudflare.com") && !host.contains("127.0.0.1");
    let path_token = path
        .split_once("/s/")
        .map(|(_, t)| t.split('?').next().unwrap_or("").to_owned())
        .unwrap_or_default();
    Some(Link {
        ws,
        site: directory.then(|| format!("https://{authority}")),
        token: if path_token.is_empty() {
            token
        } else {
            path_token
        },
    })
}

enum Resolve {
    /// the Hub is online at this socket
    Ws(String),
    /// the link is known but its Hub is offline: try again later
    Offline,
    /// not a permanent-link site (no answer, or no `online` field): the URL is the Hub's own address
    Direct,
}

fn resolve(site: &str, token: &str) -> Resolve {
    let Some(j) = hs_app::get_json(
        &format!("{site}/api/resolve/{token}"),
        Duration::from_secs(2),
        Duration::from_secs(4),
    ) else {
        return Resolve::Direct;
    };
    match j.get("online") {
        None => Resolve::Direct,
        Some(serde_json::Value::Bool(true)) => j
            .get("ws")
            .and_then(|w| w.as_str())
            .filter(|w| !w.is_empty())
            .map_or(Resolve::Offline, |w| Resolve::Ws(w.to_owned())),
        Some(_) => Resolve::Offline,
    }
}

// ---- network thread ---------------------------------------------------------------------------------------------

/// A plain or TLS socket, with the TCP socket underneath (non-blocking mode is set on that one: on Windows a
/// `try_clone`d handle does not share the read timeout).
trait Stream: Read + Write + Send {
    fn tcp(&self) -> &TcpStream;
}
impl Stream for TcpStream {
    fn tcp(&self) -> &TcpStream {
        self
    }
}
impl Stream for rustls::StreamOwned<rustls::ClientConnection, TcpStream> {
    fn tcp(&self) -> &TcpStream {
        &self.sock
    }
}

type Ws = WebSocket<Box<dyn Stream>>;

/// `true` when a tungstenite call went through or only has to finish later (non-blocking socket: queued).
fn ok_or_queued<T>(r: tungstenite::Result<T>) -> bool {
    match r {
        Ok(_) => true,
        Err(tungstenite::Error::Io(e)) => e.kind() == ErrorKind::WouldBlock,
        Err(_) => false,
    }
}

/// DNS on a throwaway thread: a resolver that hangs must not hang `disconnect()` (which joins the network thread).
fn lookup(s: &Shared, host: &str, port: u16) -> Vec<SocketAddr> {
    let (tx, rx) = std::sync::mpsc::channel();
    let target = (host.to_owned(), port);
    let spawned = std::thread::Builder::new()
        .name("HAS Karaoke DNS".into())
        .spawn(move || {
            let found: Vec<SocketAddr> = target
                .to_socket_addrs()
                .map(Iterator::collect)
                .unwrap_or_default();
            let _ = tx.send(found);
        });
    if spawned.is_err() {
        return Vec::new();
    }
    for _ in 0..40 {
        match rx.recv_timeout(Duration::from_millis(100)) {
            Ok(found) => return found,
            Err(std::sync::mpsc::RecvTimeoutError::Timeout) if !s.quitting() => {}
            Err(_) => break,
        }
    }
    Vec::new() // 4 s, or asked to quit
}

/// The C++ client's two public STUN servers, as addresses (the peer must not do DNS itself, see `lookup`). None
/// resolved (offline): a closed local port, so only this computer's own candidates are offered.
fn ice_config(s: &Shared) -> RtcPeerConfig {
    let stun = |url: String| IceServerConfig {
        url,
        username: String::new(),
        password: String::new(),
    };
    let mut ice_servers: Vec<IceServerConfig> =
        [("stun.cloudflare.com", 3478), ("stun.l.google.com", 19302)]
            .iter()
            .flat_map(|(h, p)| lookup(s, h, *p))
            .filter(SocketAddr::is_ipv4)
            .map(|a| stun(format!("stun:{a}")))
            .collect();
    if ice_servers.is_empty() {
        ice_servers.push(stun("stun:127.0.0.1:9".into()));
    }
    RtcPeerConfig {
        ice_servers,
        ..RtcPeerConfig::default()
    }
}

fn connect_ws(s: &Shared, url: &str) -> Option<Ws> {
    let (scheme, rest) = url.split_once("://")?;
    let secure = scheme.eq_ignore_ascii_case("wss");
    let authority = rest.split('/').next()?;
    let (host, port) = match authority.rsplit_once(':') {
        Some((h, p)) if !h.ends_with(']') || p.parse::<u16>().is_ok() => (h, p.parse().ok()?),
        _ => (authority, if secure { 443 } else { 80 }),
    };
    let host = host.trim_start_matches('[').trim_end_matches(']');
    let addr = *lookup(s, host, port).first()?;
    let tcp = TcpStream::connect_timeout(&addr, Duration::from_secs(2)).ok()?;
    let _ = tcp.set_nodelay(true);
    tcp.set_read_timeout(Some(Duration::from_secs(2))).ok()?;
    tcp.set_write_timeout(Some(Duration::from_secs(2))).ok()?;
    let stream: Box<dyn Stream> = if secure {
        Box::new(tls(host, tcp)?)
    } else {
        Box::new(tcp)
    };
    let (ws, _) = tungstenite::client::client(url, stream).ok()?;
    // the session loop never blocks on the socket: it waits 1 ms when there was nothing to do
    ws.get_ref().tcp().set_nonblocking(true).ok()?;
    Some(ws)
}

/// TLS with the operating system's certificate store (as `hs-app`'s HTTP client and WinHTTP).
fn tls(
    host: &str,
    tcp: TcpStream,
) -> Option<rustls::StreamOwned<rustls::ClientConnection, TcpStream>> {
    use rustls_platform_verifier::BuilderVerifierExt;
    let provider = Arc::new(rustls::crypto::aws_lc_rs::default_provider());
    let config = rustls::ClientConfig::builder_with_provider(provider)
        .with_safe_default_protocol_versions()
        .ok()?
        .with_platform_verifier()
        .ok()?
        .with_no_client_auth();
    let name = rustls::pki_types::ServerName::try_from(host.to_owned()).ok()?;
    let conn = rustls::ClientConnection::new(Arc::new(config), name).ok()?;
    Some(rustls::StreamOwned::new(conn, tcp))
}

fn run(s: &Shared, input: &str) {
    let Some(link) = parse_link(input) else {
        s.set_state(ConnectionState::Failed);
        return;
    };
    if let Ok(mut i) = s.info.lock() {
        i.token = link.token.clone();
    }
    let mut site = link.site.clone();
    while !s.quitting() {
        let mut url = link.ws.clone();
        if let Some(base) = &site {
            s.set_state(ConnectionState::Resolving);
            match resolve(base, &link.token) {
                Resolve::Ws(w) => url = w,
                Resolve::Direct => site = None,
                Resolve::Offline => {
                    s.set_state(ConnectionState::Reconnecting);
                    s.nap(4000);
                    continue;
                }
            }
        }
        if s.quitting() {
            break;
        }
        s.set_state(ConnectionState::Connecting);
        let Some(ws) = connect_ws(s, &url) else {
            s.set_state(ConnectionState::Reconnecting);
            s.nap(3000);
            continue;
        };
        s.set_state(ConnectionState::Handshaking);
        Session::new(s).run(ws);
        s.is_udp.store(false, Relaxed);
        if !s.quitting() {
            s.set_state(ConnectionState::Reconnecting);
            s.nap(3000);
        }
    }
}

/// One connection: everything the network thread owns while it lasts (allocated once).
struct Session<'a> {
    s: &'a Shared,
    enc: OpusEnc,
    dec: OpusDec,
    aec: FriendEcho,
    aec_on: bool,
    opus: bool,
    up_read: u64,
    up_seq: u32,
    last_down: Option<u32>,
    pcm: [Vec<f32>; 2],
    mono: Vec<f32>,
    mic: [Vec<f32>; 2],
    clean: Vec<f32>,
    payload: Vec<u8>,
    packet: Vec<u8>,
}

impl<'a> Session<'a> {
    fn new(s: &'a Shared) -> Self {
        let mut enc = OpusEnc::new();
        enc.init(48000, 1, App::Voip);
        enc.set_bitrate(96000);
        enc.set_complexity(5);
        enc.set_inband_fec(true);
        enc.set_packet_loss_perc(5);
        let mut dec = OpusDec::new();
        dec.init(48000, 2);
        let mut aec = FriendEcho::new();
        aec.init(48000, 240, 200); // 5 ms steps, the Opus packet size
        let speaker = s.speaker_mode.load(Relaxed);
        aec.set_mode(if speaker { AecMode::On } else { AecMode::Off });
        Self {
            s,
            enc,
            dec,
            aec,
            aec_on: speaker,
            opus: false,
            up_read: s.up_write.load(Acquire),
            up_seq: 0,
            last_down: None,
            pcm: [vec![0.0; MAX_FRAMES], vec![0.0; MAX_FRAMES]],
            mono: vec![0.0; MAX_FRAMES],
            mic: [vec![0.0; 480], vec![0.0; 480]],
            clean: vec![0.0; 480],
            payload: vec![0; 1275],
            packet: Vec::with_capacity(24 + 960),
        }
    }

    fn run(mut self, mut ws: Ws) {
        let s = self.s;
        let mut rtc = RtcPeer::new(&ice_config(s)).ok();
        let hello = serde_json::json!({
            "codec": "opus",
            "speakerMode": self.aec_on,
            "webrtc": true,
            "hearMyself": false,
        });
        if !ok_or_queued(ws.send(Message::text(hello.to_string()))) {
            return;
        }
        while !s.quitting() {
            let mut busy = true;
            // 1. the WebSocket (non-blocking)
            match ws.read() {
                Ok(Message::Text(t)) => self.on_text(&t, &mut ws, rtc.as_mut()),
                Ok(Message::Binary(b)) => self.on_audio(&b),
                Ok(Message::Close(_)) => break,
                Ok(_) => {}
                Err(tungstenite::Error::Io(e)) if e.kind() == ErrorKind::WouldBlock => busy = false,
                Err(_) => break,
            }
            // 2. the DataChannel
            if let Some(peer) = rtc.as_mut() {
                while let Ok(Some(ev)) = peer.poll(Duration::ZERO) {
                    busy = true;
                    match ev {
                        RtcEvent::LocalDescription { sdp, .. } => {
                            let j = serde_json::json!({"type": "rtc_answer", "sdp": sdp});
                            let _ = ws.send(Message::text(j.to_string()));
                        }
                        RtcEvent::LocalCandidate { candidate, mid } => {
                            let j = serde_json::json!({"type": "rtc_candidate", "candidate": candidate, "mid": mid});
                            let _ = ws.send(Message::text(j.to_string()));
                        }
                        RtcEvent::DataChannelOpen => s.is_udp.store(true, Relaxed),
                        RtcEvent::DataChannelClosed => s.is_udp.store(false, Relaxed),
                        RtcEvent::Message(d) => self.on_audio(&d),
                        RtcEvent::StateChange(_) => {}
                    }
                }
            }
            // 3. speaker mode changed in the UI
            let speaker = s.speaker_mode.load(Relaxed);
            if speaker != self.aec_on {
                self.aec_on = speaker;
                self.aec
                    .set_mode(if speaker { AecMode::On } else { AecMode::Off });
                let j = serde_json::json!({"speakerMode": speaker, "aec": if speaker { "on" } else { "off" }});
                let _ = ws.send(Message::text(j.to_string()));
            }
            // 4. the mic
            match self.send_up(&mut ws, rtc.as_mut()) {
                Some(sent) => busy |= sent,
                None => break,
            }
            if !ok_or_queued(ws.flush()) {
                break;
            }
            if !busy {
                // std's sleep uses a high-resolution timer on Windows: ~1 ms, not the 15.6 ms tick
                std::thread::sleep(Duration::from_millis(1));
            }
        }
        if let Some(mut peer) = rtc {
            peer.close();
        }
        let _ = ws.close(None);
        let _ = ws.flush();
    }

    fn on_text(&mut self, text: &str, ws: &mut Ws, rtc: Option<&mut RtcPeer>) {
        let Ok(j) = serde_json::from_str::<serde_json::Value>(text) else {
            return;
        };
        let s = self.s;
        let str_of = |k: &str| j.get(k).and_then(|v| v.as_str()).unwrap_or("").to_owned();
        if let Some(peer) = rtc {
            match j.get("type").and_then(|t| t.as_str()) {
                Some("rtc_offer") if j.get("sdp").is_some() => {
                    let _ = peer.set_remote_description("offer", &str_of("sdp"));
                }
                Some("rtc_candidate") if j.get("candidate").is_some() => {
                    let _ = peer.add_remote_candidate(&str_of("candidate"), &str_of("mid"));
                }
                _ => {}
            }
        }
        if j.get("v")
            .and_then(|v| v.as_f64())
            .is_some_and(|v| v >= 2.0)
        {
            self.opus = j.get("codecs").and_then(|c| c.as_array()).is_some_and(|a| {
                a.iter()
                    .any(|c| c.as_str().is_some_and(|c| c.eq_ignore_ascii_case("opus")))
            });
            if let Ok(mut i) = s.info.lock() {
                i.name = str_of("name");
                i.host = str_of("host");
                i.codec = if self.opus { "opus" } else { "pcm16" }.into();
            }
            s.set_state(ConnectionState::Live);
        }
        if let Some(p) = j.get("ping") {
            let _ = ws.send(Message::text(format!("{{\"pong\":{p}}}")));
        }
        if let Some(d) = j.get("delay").and_then(|d| d.as_f64())
            && let Ok(mut i) = s.info.lock()
        {
            i.delay_ms = d as f32;
        }
    }

    /// One packet from the Hub: HRA3 (Opus or PCM) or HRA2 (PCM from Hub 1.0.4), into the jitter buffer's ring.
    fn on_audio(&mut self, data: &[u8]) {
        if let Some((h, payload)) = parse_hra3(data) {
            let frames = usize::from(h.frames).min(MAX_FRAMES);
            if h.codec == CODEC_OPUS {
                if frames == 0 {
                    return;
                }
                if let Some(last) = self.last_down {
                    let gap = seq_gap(h.seq, last);
                    if gap < 0 {
                        return; // late or repeated: its place has been played
                    }
                    let missed = gap.min(10) as usize;
                    for m in 0..missed {
                        // the one right before this packet comes back from this packet's FEC
                        let fec = m + 1 == missed && h.flags & FLAG_FEC != 0;
                        let [l, r] = &mut self.pcm;
                        let got = self.dec.decode(
                            fec.then_some(payload),
                            &mut [&mut l[..frames], &mut r[..frames]],
                            frames,
                            fec,
                        );
                        self.publish(got, 48000);
                    }
                }
                let [l, r] = &mut self.pcm;
                let got = self.dec.decode(
                    Some(payload),
                    &mut [&mut l[..frames], &mut r[..frames]],
                    frames,
                    false,
                );
                if got > 0 {
                    self.publish(got, 48000);
                    self.last_down = Some(h.seq);
                    self.set_codec("opus");
                }
            } else if h.codec == CODEC_PCM16 {
                let frames = frames.min(payload.len() / (2 * usize::from(h.chans)));
                self.read_pcm16(payload, usize::from(h.chans), frames);
                self.publish(frames as i32, 48000);
                self.set_codec("pcm16");
            }
            return;
        }
        // HRA2: magic, rate u32, chans u16, latencyKnown u16, frames u32, pos u64, then PCM16
        if data.len() >= 24
            && u32::from_le_bytes(data[0..4].try_into().unwrap_or_default()) == MAGIC2
        {
            let rate = u32::from_le_bytes(data[4..8].try_into().unwrap_or_default());
            let chans = usize::from(u16::from_le_bytes(
                data[8..10].try_into().unwrap_or_default(),
            ));
            let frames = u32::from_le_bytes(data[12..16].try_into().unwrap_or_default()) as usize;
            if (1..=2).contains(&chans) && frames > 0 {
                let frames = frames.min((data.len() - 24) / (2 * chans)).min(MAX_FRAMES);
                self.read_pcm16(&data[24..], chans, frames);
                self.publish(frames as i32, rate);
                self.set_codec("pcm16");
            }
        }
    }

    fn read_pcm16(&mut self, payload: &[u8], chans: usize, frames: usize) {
        for i in 0..frames {
            let at = |c: usize| {
                let o = (i * chans + c) * 2;
                f32::from(i16::from_le_bytes([payload[o], payload[o + 1]])) / 32768.0
            };
            self.pcm[0][i] = at(0);
            self.pcm[1][i] = at(chans - 1);
        }
    }

    /// `got` decoded frames of `pcm` into the ring and the echo canceller's reference.
    fn publish(&mut self, got: i32, rate: u32) {
        let Ok(n) = usize::try_from(got) else {
            return;
        };
        if n == 0 {
            return;
        }
        let s = self.s;
        s.down_rate.store(rate, Relaxed);
        ring_write(
            &s.down,
            &s.down_write,
            &[&self.pcm[0][..n], &self.pcm[1][..n]],
            n as u32,
        );
        for i in 0..n {
            self.mono[i] = 0.5 * (self.pcm[0][i] + self.pcm[1][i]);
        }
        self.aec.push_reference(&self.mono[..n]);
    }

    fn set_codec(&self, codec: &str) {
        if let Ok(mut i) = self.s.info.lock()
            && i.codec != codec
        {
            i.codec = codec.into();
        }
    }

    /// Sends every whole packet of mic the ring holds: whether it sent any, `None` when the WebSocket failed.
    fn send_up(&mut self, ws: &mut Ws, mut rtc: Option<&mut RtcPeer>) -> Option<bool> {
        let s = self.s;
        // Opus in 5 ms packets; PCM keeps 10 ms for old Hubs
        let frames = if self.opus { 240 } else { 480 };
        let mut sent = false;
        loop {
            let w = s.up_write.load(Acquire);
            if w < self.up_read || w - self.up_read > u64::from(hs_bus::protocol::kRingFrames / 2) {
                self.up_read = w; // fell behind a whole half ring (stall): start again from now
            }
            if w - self.up_read < frames as u64 {
                return Some(sent);
            }
            let [a, b] = &mut self.mic;
            ring_read_at(
                &s.up,
                &s.up_write,
                self.up_read,
                &mut [&mut a[..frames], &mut b[..frames]],
                frames as u32,
                None,
            );
            self.up_read += frames as u64;
            let mic: &[f32] = if self.aec_on {
                let d = s.local_delay.load(Relaxed);
                let step = self.aec.frame_size().max(1);
                for off in (0..frames).step_by(step) {
                    let end = (off + step).min(frames);
                    self.aec
                        .process(&self.mic[0][off..end], &mut self.clean[off..end], d);
                }
                s.erle_db.store(self.aec.erle_db());
                s.leak.store(self.aec.leak_detected(), Relaxed);
                &self.clean[..frames]
            } else {
                &self.mic[0][..frames]
            };
            let pos = s.echo_pos.load(Relaxed);
            self.packet.clear();
            if frames == 240 {
                let bytes = self.enc.encode(&[mic], frames, &mut self.payload);
                let Ok(bytes) = usize::try_from(bytes) else {
                    continue;
                };
                if bytes == 0 {
                    continue;
                }
                let h = Hra3Header {
                    magic: MAGIC3,
                    codec: CODEC_OPUS,
                    chans: 1,
                    flags: FLAG_FEC,
                    seq: self.up_seq,
                    pos,
                    frames: frames as u16,
                };
                self.up_seq = self.up_seq.wrapping_add(1);
                self.packet.resize(HRA3_HEADER_BYTES + bytes, 0);
                if write_hra3(&mut self.packet, &h, &self.payload[..bytes]).is_none() {
                    continue;
                }
            } else {
                self.packet.extend(MAGIC2.to_le_bytes());
                self.packet.extend(48000u32.to_le_bytes());
                self.packet.extend(1u16.to_le_bytes()); // mono
                self.packet.extend(1u16.to_le_bytes()); // latency known
                self.packet.extend((frames as u32).to_le_bytes());
                self.packet.extend(pos.to_le_bytes());
                for v in mic {
                    let q = (v.clamp(-1.0, 1.0) * 32767.0).round() as i16;
                    self.packet.extend(q.to_le_bytes());
                }
            }
            // the DataChannel when it is open, else the WebSocket
            let sent_udp = rtc
                .as_deref_mut()
                .is_some_and(|p| p.is_open() && p.send(&self.packet));
            if !sent_udp && !ok_or_queued(ws.send(Message::binary(self.packet.clone()))) {
                return None;
            }
            sent = true;
        }
    }
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn links() {
        let l = parse_link("http://127.0.0.1:47811/s/abc").unwrap();
        assert_eq!(l.ws, "ws://127.0.0.1:47811/ws/s/abc");
        assert_eq!((l.site, l.token.as_str()), (None, "abc"));

        let l = parse_link("  https://hearaside.netlify.app/s/TOK?x=1 ").unwrap();
        assert_eq!(l.ws, "wss://hearaside.netlify.app/ws/s/TOK?x=1");
        assert_eq!(l.site.as_deref(), Some("https://hearaside.netlify.app"));
        assert_eq!(l.token, "TOK");

        let l = parse_link("https://a-b-c.trycloudflare.com/s/T1").unwrap();
        assert_eq!(
            (l.ws.as_str(), l.site),
            ("wss://a-b-c.trycloudflare.com/ws/s/T1", None)
        );

        // a bare token goes to trycloudflare.com, as in the C++
        let l = parse_link("T2").unwrap();
        assert_eq!(l.ws, "wss://trycloudflare.com/ws/s/T2");

        for deep in [
            "has://join?u=http%3A%2F%2F127.0.0.1%3A47811%2Fs%2Fq",
            "hearaside://join?url=http%3A%2F%2F127.0.0.1%3A47811%2Fs%2Fq",
            "HEARASIDE://join?url=http%3A%2F%2F127.0.0.1%3A47811%2Fs%2Fq",
        ] {
            let l = parse_link(deep).unwrap();
            assert_eq!(
                (l.ws.as_str(), l.token.as_str()),
                ("ws://127.0.0.1:47811/ws/s/q", "q"),
                "{deep}"
            );
        }
        assert!(parse_link("https:///s/x").is_none());
    }

    #[test]
    fn late_and_repeated_packets_are_dropped_and_a_gap_is_filled() {
        let (client, _audio) = join_client(48000.0, 256);
        let s = &*client.shared;
        let mut session = Session::new(s);
        let mut enc = OpusEnc::new();
        assert!(enc.init(48000, 2, App::Voip));
        enc.set_inband_fec(true);
        let tone: Vec<f32> = (0..240).map(|i| 0.3 * (i as f32 * 0.05).sin()).collect();
        let packet = |enc: &mut OpusEnc, seq: u32| {
            let mut payload = [0u8; 1275];
            let n = enc.encode(&[&tone, &tone], 240, &mut payload) as usize;
            let h = Hra3Header {
                magic: MAGIC3,
                codec: CODEC_OPUS,
                chans: 2,
                flags: FLAG_FEC,
                seq,
                pos: 0,
                frames: 240,
            };
            let mut p = vec![0u8; HRA3_HEADER_BYTES + n];
            write_hra3(&mut p, &h, &payload[..n]).unwrap();
            p
        };
        let pk: Vec<Vec<u8>> = (0..5).map(|q| packet(&mut enc, q)).collect();
        let frames = || s.down_write.load(SeqCst) / 240;
        session.on_audio(&pk[0]);
        session.on_audio(&pk[1]);
        assert_eq!(frames(), 2);
        session.on_audio(&pk[3]); // 2 is missing: its place is filled (from 3's FEC), then 3
        assert_eq!(frames(), 4);
        session.on_audio(&pk[2]); // late: already played
        session.on_audio(&pk[3]); // repeated
        assert_eq!(
            frames(),
            4,
            "the C++ played these again and moved the sequence back"
        );
        session.on_audio(&pk[4]);
        assert_eq!(frames(), 5, "no loss concealment after the late ones");
        assert_eq!(client.room_info().codec, "opus");
    }
}
