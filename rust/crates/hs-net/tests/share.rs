//! `ShareServer` (R3.3): real loopback TCP against the server, a temp shm bus, a fake cloudflared, and a Rust `RtcPeer`
//! playing the browser for the WebRTC signaling. Every test uses its own bus and its own server on the first free port.
#![allow(clippy::needless_range_loop)]

use hs_bus::bus::bits_float;
use hs_bus::protocol::{BusLayout, FriendState};
use hs_bus::ring::{ReadResult, ring_read_at, ring_write};
use hs_bus::shm::{SharedMemory, current_pid, process_alive};
use hs_net::aec::AecMode;
use hs_net::opus::{App, OpusDec, OpusEnc};
use hs_net::rtc::{IceServerConfig, RtcEvent, RtcPeer, RtcPeerConfig};
use hs_net::share::{CloudflaredSource, FriendLink, ShareServer, Tunnel};
use hs_net::wire::{
    CODEC_OPUS, FLAG_FEC, HRA3_HEADER_BYTES, Hra3Header, MAGIC3, parse_hra3, write_hra3,
};
use std::io::{ErrorKind, Read, Write};
use std::net::TcpStream;
use std::path::{Path, PathBuf};
use std::sync::Arc;
use std::sync::atomic::{AtomicU32, Ordering};
use std::time::{Duration, Instant};

// ---- fixture -----------------------------------------------------------------------------------------------------

static COUNTER: AtomicU32 = AtomicU32::new(0);

fn unique(tag: &str) -> String {
    let n = COUNTER.fetch_add(1, Ordering::Relaxed);
    format!("rs-share-{tag}-{}-{n}", current_pid())
}

/// The first 20 ports are all a machine has for servers: tests wait for a place.
struct Place(u32);
static PLACES: std::sync::Mutex<u32> = std::sync::Mutex::new(0);
static FREED: std::sync::Condvar = std::sync::Condvar::new();
const MAX_SERVERS: u32 = 8;

fn place(n: u32) -> Place {
    let mut used = PLACES.lock().unwrap();
    while *used + n > MAX_SERVERS {
        used = FREED.wait(used).unwrap();
    }
    *used += n;
    Place(n)
}

impl Drop for Place {
    fn drop(&mut self) {
        *PLACES.lock().unwrap() -= self.0;
        FREED.notify_all();
    }
}

struct T {
    _place: Place,
    server: ShareServer,
    bus: Arc<SharedMemory>,
    port: u16,
    listen: String,
    send: String,
}

impl T {
    fn layout(&self) -> &BusLayout {
        self.bus.layout()
    }
}

fn stun_config() -> Vec<IceServerConfig> {
    // no internet in tests: a closed local UDP port acts as an unavailable STUN service
    vec![IceServerConfig {
        url: "stun:127.0.0.1:9".into(),
        username: String::new(),
        password: String::new(),
    }]
}

fn start(tag: &str) -> T {
    start_placed(tag, place(1))
}

fn start_placed(tag: &str, _place: Place) -> T {
    let server = ShareServer::new();
    server.set_cloudflared_source(CloudflaredSource::Disabled);
    server.set_ice_servers(stun_config());
    let bus = Arc::new(SharedMemory::open(&unique(tag)).unwrap());
    let (listen, send) = (ShareServer::new_token(), ShareServer::new_token());
    assert!(server.start(bus.clone(), &listen, &send));
    let port = server.port();
    assert!((47810..47830).contains(&port));
    T {
        _place,
        server,
        bus,
        port,
        listen,
        send,
    }
}

fn wait_for(timeout: Duration, mut f: impl FnMut() -> bool) -> bool {
    let end = Instant::now() + timeout;
    while Instant::now() < end {
        if f() {
            return true;
        }
        std::thread::sleep(Duration::from_millis(5));
    }
    f()
}

const SEC: Duration = Duration::from_secs(1);

// ---- HTTP + WebSocket client -------------------------------------------------------------------------------------

struct Resp {
    code: u16,
    head: String,
    body: Vec<u8>,
}

impl Resp {
    fn text(&self) -> String {
        String::from_utf8_lossy(&self.body).into_owned()
    }
}

fn parse_response(raw: &[u8]) -> Resp {
    let at = raw
        .windows(4)
        .position(|w| w == b"\r\n\r\n")
        .map_or(raw.len(), |p| p + 4);
    let head = String::from_utf8_lossy(&raw[..at]).into_owned();
    let code = head
        .split(' ')
        .nth(1)
        .and_then(|c| c.parse().ok())
        .unwrap_or(0);
    Resp {
        code,
        head,
        body: raw[at..].to_vec(),
    }
}

fn request(port: u16, bytes: &[u8]) -> Resp {
    let mut s = TcpStream::connect(("127.0.0.1", port)).unwrap();
    s.set_read_timeout(Some(Duration::from_secs(8))).unwrap();
    s.write_all(bytes).unwrap();
    let mut raw = Vec::new();
    let _ = s.read_to_end(&mut raw);
    parse_response(&raw)
}

fn get_with(port: u16, path: &str, extra: &str) -> Resp {
    request(
        port,
        format!("GET {path} HTTP/1.1\r\nHost: x\r\n{extra}\r\n").as_bytes(),
    )
}

fn get(port: u16, path: &str) -> Resp {
    get_with(port, path, "")
}

fn upgrade_request(port: u16, path: &str, origin: Option<&str>, extra: &str) -> String {
    let origin = origin.map_or(String::new(), |o| format!("Origin: {o}\r\n"));
    format!(
        "GET {path} HTTP/1.1\r\nHost: 127.0.0.1:{port}\r\nUpgrade: websocket\r\nConnection: Upgrade\r\nSec-WebSocket-Key: dGhlIHNhbXBsZSBub25jZQ==\r\nSec-WebSocket-Version: 13\r\n{origin}{extra}\r\n"
    )
}

enum Recv {
    Msg(u8, Vec<u8>),
    Timeout,
    Closed,
}

struct Ws {
    s: TcpStream,
}

fn read_deadline(s: &mut TcpStream, buf: &mut [u8], end: Instant) -> Recv {
    let mut at = 0;
    while at < buf.len() {
        let left = end.saturating_duration_since(Instant::now());
        if left.is_zero() {
            return if at == 0 { Recv::Timeout } else { Recv::Closed };
        }
        s.set_read_timeout(Some(left.max(Duration::from_millis(1))))
            .unwrap();
        match s.read(&mut buf[at..]) {
            Ok(0) => return Recv::Closed,
            Ok(n) => at += n,
            Err(e) if matches!(e.kind(), ErrorKind::WouldBlock | ErrorKind::TimedOut) => {}
            Err(_) => return Recv::Closed,
        }
    }
    Recv::Msg(0, Vec::new())
}

impl Ws {
    /// The handshake; a refusal comes back as the HTTP response.
    fn open(port: u16, path: &str, origin: Option<&str>) -> Result<Ws, Resp> {
        Self::open_with(port, path, origin, "")
    }

    fn open_with(port: u16, path: &str, origin: Option<&str>, extra: &str) -> Result<Ws, Resp> {
        let mut s = TcpStream::connect(("127.0.0.1", port)).unwrap();
        s.set_nodelay(true).unwrap();
        s.write_all(upgrade_request(port, path, origin, extra).as_bytes())
            .unwrap();
        let mut raw = Vec::new();
        let end = Instant::now() + Duration::from_secs(8);
        while !raw.ends_with(b"\r\n\r\n") {
            let mut b = [0u8; 1];
            match read_deadline(&mut s, &mut b, end) {
                Recv::Msg(..) => raw.push(b[0]),
                _ => break,
            }
        }
        let head = String::from_utf8_lossy(&raw).into_owned();
        if head.starts_with("HTTP/1.1 101 Switching Protocols\r\n") {
            assert!(head.contains("Upgrade: websocket\r\n"));
            assert!(head.contains("Connection: Upgrade\r\n"));
            assert!(
                head.contains("Sec-WebSocket-Accept: s3pPLMBiTxaQ9kYGzzhZRbK+xOo=\r\n"),
                "{head}"
            );
            Ok(Ws { s })
        } else {
            let mut rest = Vec::new();
            let _ = s.read_to_end(&mut rest);
            raw.extend(rest);
            Err(parse_response(&raw))
        }
    }

    fn send(&mut self, op: u8, payload: &[u8]) {
        let mask = [0x11u8, 0x22, 0x33, 0x44];
        let mut f = vec![0x80 | op];
        match payload.len() {
            n if n < 126 => f.push(0x80 | n as u8),
            n if n < 65536 => {
                f.push(0x80 | 126);
                f.extend((n as u16).to_be_bytes());
            }
            n => {
                f.push(0x80 | 127);
                f.extend((n as u64).to_be_bytes());
            }
        }
        f.extend(mask);
        f.extend(payload.iter().enumerate().map(|(i, b)| b ^ mask[i & 3]));
        let _ = self.s.write_all(&f);
    }

    /// A masked frame with the first byte (FIN / RSV / opcode) as given.
    fn send_raw(&mut self, first: u8, payload: &[u8]) {
        assert!(payload.len() < 126);
        let mask = [5u8, 6, 7, 8];
        let mut f = vec![first, 0x80 | payload.len() as u8];
        f.extend(mask);
        f.extend(payload.iter().enumerate().map(|(i, b)| b ^ mask[i & 3]));
        let _ = self.s.write_all(&f);
    }

    fn recv(&mut self, timeout: Duration) -> Recv {
        let end = Instant::now() + timeout;
        let mut h = [0u8; 2];
        match read_deadline(&mut self.s, &mut h, end) {
            Recv::Msg(..) => {}
            other => return other,
        }
        let mut len = usize::from(h[1] & 0x7F);
        let far = end + Duration::from_secs(5);
        if len == 126 {
            let mut e = [0u8; 2];
            if !matches!(read_deadline(&mut self.s, &mut e, far), Recv::Msg(..)) {
                return Recv::Closed;
            }
            len = usize::from(u16::from_be_bytes(e));
        } else if len == 127 {
            let mut e = [0u8; 8];
            if !matches!(read_deadline(&mut self.s, &mut e, far), Recv::Msg(..)) {
                return Recv::Closed;
            }
            len = u64::from_be_bytes(e) as usize;
        }
        let mut body = vec![0u8; len];
        if !matches!(read_deadline(&mut self.s, &mut body, far), Recv::Msg(..)) {
            return Recv::Closed;
        }
        Recv::Msg(h[0] & 0x0F, body)
    }

    /// The next message with this opcode (others are skipped); `None` on timeout or when the server closed.
    fn recv_op(&mut self, op: u8, timeout: Duration) -> Option<Vec<u8>> {
        let end = Instant::now() + timeout;
        loop {
            match self.recv(end.saturating_duration_since(Instant::now())) {
                Recv::Msg(o, d) if o == op => return Some(d),
                Recv::Msg(..) => {}
                _ => return None,
            }
        }
    }

    /// The server closed the connection within `timeout` (anything it still sent is skipped).
    fn closed_within(&mut self, timeout: Duration) -> bool {
        let end = Instant::now() + timeout;
        loop {
            match self.recv(end.saturating_duration_since(Instant::now())) {
                Recv::Msg(..) => {}
                Recv::Closed => return true,
                Recv::Timeout => return false,
            }
        }
    }
}

fn open_ok(port: u16, path: &str) -> Ws {
    match Ws::open(port, path, None) {
        Ok(w) => w,
        Err(r) => panic!("{path}: {} {}", r.code, r.text()),
    }
}

// ---- bus + packets -----------------------------------------------------------------------------------------------

fn sine(n: usize, start: usize, freq: f32, rate: f32) -> Vec<f32> {
    (0..n)
        .map(|i| 0.5 * (2.0 * std::f32::consts::PI * freq * (start + i) as f32 / rate).sin())
        .collect()
}

fn write_stream(l: &BusLayout, left: &[f32], right: &[f32]) {
    ring_write(
        &l.streamAudio.0[0],
        &l.streamHeader.writePos.0[0],
        &[left, right],
        left.len() as u32,
    );
}

fn write_mix(l: &BusLayout, left: &[f32], right: &[f32]) {
    ring_write(
        &l.friendMixAudio.0,
        &l.friendMixWrite.0,
        &[left, right],
        left.len() as u32,
    );
}

fn pcm_i16(f: f32) -> i16 {
    (f.clamp(-1.0, 1.0) * 32767.0).round() as i16
}

/// (rate, channels, frames, samples) of an HRA1 packet; panics when it is not one.
fn parse_hra1(p: &[u8]) -> (u32, u16, u32, Vec<i16>) {
    assert_eq!(
        &p[..4],
        b"HRA1",
        "packet starts with {:?}",
        &p[..4.min(p.len())]
    );
    let rate = u32::from_le_bytes(p[4..8].try_into().unwrap());
    let chans = u16::from_le_bytes(p[8..10].try_into().unwrap());
    let frames = u32::from_le_bytes(p[12..16].try_into().unwrap());
    let pcm = p[16..]
        .as_chunks::<2>()
        .0
        .iter()
        .map(|c| i16::from_le_bytes(*c))
        .collect();
    (rate, chans, frames, pcm)
}

fn hra2(rate: u32, chans: u16, samples: &[i16], echo: u64) -> Vec<u8> {
    let frames = samples.len() / usize::from(chans);
    let mut p = b"HRA2".to_vec();
    p.extend(rate.to_le_bytes());
    p.extend(chans.to_le_bytes());
    p.extend(0u16.to_le_bytes());
    p.extend((frames as u32).to_le_bytes());
    p.extend(echo.to_le_bytes());
    p.extend(samples.iter().flat_map(|s| s.to_le_bytes()));
    p
}

fn hra1(rate: u32, chans: u16, samples: &[i16]) -> Vec<u8> {
    let frames = samples.len() / usize::from(chans);
    let mut p = b"HRA1".to_vec();
    p.extend(rate.to_le_bytes());
    p.extend(chans.to_le_bytes());
    p.extend(0u16.to_le_bytes());
    p.extend((frames as u32).to_le_bytes());
    p.extend(samples.iter().flat_map(|s| s.to_le_bytes()));
    p
}

fn opus_packet(enc: &mut OpusEnc, chans: u8, seq: u32, pos: u64, pcm: &[f32]) -> Vec<u8> {
    let mut payload = [0u8; 1275];
    let n = enc.encode(&[pcm, pcm], pcm.len(), &mut payload);
    assert!(n > 0);
    let h = Hra3Header {
        magic: MAGIC3,
        codec: CODEC_OPUS,
        chans,
        flags: FLAG_FEC,
        seq,
        pos,
        frames: pcm.len() as u16,
    };
    let mut p = vec![0u8; HRA3_HEADER_BYTES + n as usize];
    write_hra3(&mut p, &h, &payload[..n as usize]).unwrap();
    p
}

fn read_ring(
    ring: &hs_bus::protocol::ChannelRing,
    wp: &std::sync::atomic::AtomicU64,
    from: u64,
    n: usize,
) -> (Vec<f32>, Vec<f32>) {
    let (mut a, mut b) = (vec![0.0f32; n], vec![0.0f32; n]);
    let mut dst: [&mut [f32]; 2] = [&mut a[..], &mut b[..]];
    assert_eq!(
        ring_read_at(ring, wp, from, &mut dst, n as u32, None),
        ReadResult::Ok
    );
    (a, b)
}

// ---- pages, handshake, tokens ------------------------------------------------------------------------------------

fn page(name: &str) -> Vec<u8> {
    let p = Path::new(env!("CARGO_MANIFEST_DIR"))
        .join("../../../plugins/hub/web")
        .join(name);
    std::fs::read(p).unwrap()
}

fn friend_link(slot: i32, id: u32, name: &str, host: &str) -> FriendLink {
    FriendLink {
        slot,
        id,
        token: ShareServer::new_token(),
        name: name.into(),
        host: host.into(),
    }
}

#[test]
fn pages_are_served_for_valid_tokens_and_wrong_ones_are_404() {
    let t = start("pages");
    let r = get(t.port, &format!("/l/{}", t.listen));
    assert_eq!(r.code, 200);
    assert_eq!(r.body, page("listen.html"));
    for h in [
        "HTTP/1.1 200 OK\r\n",
        "Content-Type: text/html; charset=utf-8\r\n",
        "Cache-Control: no-store\r\n",
        "Connection: close\r\n",
        "X-Content-Type-Options: nosniff\r\n",
    ] {
        assert!(r.head.contains(h), "{h} in {}", r.head);
    }
    assert!(
        r.head
            .contains(&format!("Content-Length: {}\r\n", r.body.len()))
    );
    assert_eq!(
        get(t.port, &format!("/s/{}", t.send)).body,
        page("send.html")
    );
    // the query string is not part of the path
    assert_eq!(get(t.port, &format!("/l/{}?x=1", t.listen)).code, 200);

    let mut wrong = t.listen.clone();
    wrong.pop();
    wrong.push(if t.listen.ends_with('a') { 'b' } else { 'a' });
    for path in [
        format!("/l/{wrong}"),
        format!("/l/{}x", t.listen),
        format!("/l/{}", &t.listen[..25]),
        "/l/".into(),
        "/".into(),
        format!("/ws/l/{}", t.listen), // a WebSocket path without the upgrade
        format!("/s/{}", t.listen),
        format!("/ws/s/{}", t.send),
        "/info/s/short".into(),
    ] {
        let r = get(t.port, &path);
        assert_eq!(r.code, 404, "{path}");
        assert_eq!(
            r.text(),
            "HEARASIDE: this link is not valid (any more).",
            "{path}"
        );
        assert!(
            r.head
                .contains("Content-Type: text/plain; charset=utf-8\r\n")
        );
    }
    // not a GET
    let r = request(
        t.port,
        format!("POST /l/{} HTTP/1.1\r\n\r\n", t.listen).as_bytes(),
    );
    assert_eq!((r.code, r.body.len()), (404, 0));
}

#[test]
fn request_head_is_read_in_pieces_and_limited() {
    let t = start("head");
    // the terminator split across two TCP segments
    let mut s = TcpStream::connect(("127.0.0.1", t.port)).unwrap();
    s.set_nodelay(true).unwrap();
    s.write_all(format!("GET /l/{} HTTP/1.1\r\nHost: x\r\n\r", t.listen).as_bytes())
        .unwrap();
    std::thread::sleep(Duration::from_millis(150));
    s.write_all(b"\n").unwrap();
    let mut raw = Vec::new();
    s.set_read_timeout(Some(Duration::from_secs(5))).unwrap();
    let _ = s.read_to_end(&mut raw);
    assert_eq!(parse_response(&raw).code, 200);
    // a head that never ends is answered 431 and closed (C++ bug: it went on with the cut head)
    let mut s = TcpStream::connect(("127.0.0.1", t.port)).unwrap();
    s.set_read_timeout(Some(Duration::from_secs(5))).unwrap();
    let _ = s.write_all(format!("GET /l/{} HTTP/1.1\r\n", t.listen).as_bytes());
    let _ = s.write_all(&vec![b'a'; 9000]);
    let mut raw = Vec::new();
    let _ = s.read_to_end(&mut raw);
    let r = parse_response(&raw);
    assert_eq!(r.code, 431, "{}", String::from_utf8_lossy(&raw));
    assert!(
        r.head
            .starts_with("HTTP/1.1 431 Request Header Fields Too Large\r\n")
    );
    // 7 KB of header that does end is fine
    let long = format!("X-Pad: {}\r\n", "a".repeat(7000));
    assert_eq!(
        get_with(t.port, &format!("/l/{}", t.listen), &long).code,
        200
    );
}

#[test]
fn friend_pages_and_info_json() {
    let t = start("info");
    let ann = friend_link(1, 11, "Ann \"A\" ก", "Bob");
    t.server
        .set_friend_links(vec![ann.clone(), friend_link(9, 12, "Bad slot", "")]);
    let r = get(t.port, &format!("/s/{}", ann.token));
    assert_eq!(r.body, page("send.html"));
    let r = get(t.port, &format!("/info/s/{}", ann.token));
    assert_eq!(r.code, 200);
    assert!(
        r.head
            .contains("Content-Type: application/json; charset=utf-8\r\n")
    );
    // `juce::JSON::toString(var, true)`: members separated by ", ", UTF-8 text as it is
    assert_eq!(
        r.text(),
        "{\"name\": \"Ann \\\"A\\\" ก\", \"host\": \"Bob\"}"
    );
    assert_eq!(
        get(t.port, &format!("/info/s/{}", t.send)).code,
        404,
        "the send-in link has no info"
    );
    let mut other = ann.token.clone();
    other.replace_range(..1, if other.starts_with('a') { "b" } else { "a" });
    assert_eq!(get(t.port, &format!("/info/s/{other}")).code, 404);
    assert_eq!(get(t.port, &format!("/s/{other}")).code, 404);
}

#[test]
fn empty_tokens_mean_no_link() {
    let _place = place(1);
    // C++ bug: an empty token matched "/l/" and "/s/" (a Hub that passed "" to switch a link off opened it to everyone)
    let server = ShareServer::new();
    server.set_cloudflared_source(CloudflaredSource::Disabled);
    let bus = Arc::new(SharedMemory::open(&unique("empty")).unwrap());
    assert!(server.start(bus, "", ""));
    let port = server.port();
    for p in ["/l/", "/s/", "/l", "/ws/l/", "/ws/s/"] {
        assert_eq!(get(port, p).code, 404, "{p}");
    }
    assert!(Ws::open(port, "/ws/s/", None).is_err());
    assert!(Ws::open(port, "/ws/l/", None).is_err());
    server.stop();
}

#[test]
fn websocket_handshake_and_listener_hello() {
    let t = start("hello");
    let mut ws = open_ok(t.port, &format!("/ws/l/{}", t.listen)); // checks the accept value
    let hello = ws.recv_op(1, 2 * SEC).expect("hello");
    assert_eq!(hello, br#"{"v":3,"codecs":["opus","pcm16"],"webrtc":true}"#);
    // a ping is answered with a pong of the same payload
    ws.send(9, b"abc");
    assert_eq!(ws.recv_op(10, 2 * SEC).as_deref(), Some(&b"abc"[..]));
    assert!(wait_for(2 * SEC, || t.server.listeners() == 1));
    // C++ bug: after answering a ping the reader waited for a data frame and gave up after 1 s, ending the connection
    std::thread::sleep(Duration::from_millis(1600));
    assert_eq!(
        t.server.listeners(),
        1,
        "a ping does not end the connection"
    );
    ws.send(10, b"unsolicited pong");
    std::thread::sleep(Duration::from_millis(1600));
    ws.send(9, b"again");
    assert_eq!(ws.recv_op(10, 2 * SEC).as_deref(), Some(&b"again"[..]));
    drop(ws);
    assert!(wait_for(3 * SEC, || t.server.listeners() == 0));
}

#[test]
fn origin_allow_and_deny() {
    let t = start("origin");
    let path = format!("/ws/l/{}", t.listen);
    let own = format!("http://127.0.0.1:{}", t.port);
    for ok in [
        None,
        Some(own.as_str()),
        Some(&format!("http://localhost:{}", t.port)[..]),
        Some(&format!("HTTP://LOCALHOST:{}", t.port)[..]),
    ] {
        assert!(Ws::open(t.port, &path, ok).is_ok(), "{ok:?}");
    }
    for bad in [
        "http://evil.example",
        "null",
        &format!("http://127.0.0.1:{}", t.port + 1),
        &format!("{own}.evil.example"),
    ] {
        let r = Ws::open(t.port, &path, Some(bad)).err().expect(bad);
        assert_eq!(r.code, 403, "{bad}");
        assert_eq!(r.text(), "HEARASIDE: this page may not connect.");
    }
    assert!(!t.server.origin_allowed("https://share.example"));
    t.server.set_allowed_origin("HTTPS://Share.Example");
    assert!(t.server.origin_allowed("https://share.example"));
    assert!(Ws::open(t.port, &path, Some("https://share.example")).is_ok());
    // the Origin is only checked on WebSockets: pages load from anywhere
    assert_eq!(
        get_with(
            t.port,
            &format!("/l/{}", t.listen),
            "Origin: http://evil.example\r\n"
        )
        .code,
        200
    );
    // the pages of the Hub's other addresses (LAN, Radmin)
    let lan = t.server.lan_base();
    assert!(t.server.origin_allowed(&lan), "{lan}");
}

#[test]
fn token_change_listen_disabled_and_friends_unaffected() {
    let t = start("tokens");
    let f = friend_link(0, 5, "Ann", "");
    t.server.set_friend_links(vec![f.clone()]);
    let old = t.listen.clone();
    let new = ShareServer::new_token();
    let mut live = open_ok(t.port, &format!("/ws/l/{old}"));
    assert!(live.recv_op(1, 2 * SEC).is_some());
    t.server.set_listen_token(&new);
    assert_eq!(get(t.port, &format!("/l/{old}")).code, 404);
    assert!(Ws::open(t.port, &format!("/ws/l/{old}"), None).is_err());
    assert_eq!(get(t.port, &format!("/l/{new}")).code, 200);
    assert!(t.server.listen_url().ends_with(&format!("/l/{new}")));
    assert!(
        t.server
            .local_listen_url()
            .ends_with(&format!(":{}/l/{new}", t.port))
    );
    assert!(Ws::open(t.port, &format!("/ws/l/{new}"), None).is_ok());
    // the open listener stays connected (friends stay connected too)
    live.send(9, b"x");
    assert!(live.recv_op(10, 2 * SEC).is_some());

    // the listen links switched off: pages and WebSockets, not the send-in link and not the friends
    t.server.set_listen_enabled(false);
    assert!(!t.server.listen_enabled());
    for p in [
        format!("/l/{new}"),
        format!("/ws/l/{new}"),
        "/l/anything".into(),
        "/ws/l/anything".into(),
    ] {
        let r = get(t.port, &p);
        assert_eq!(r.code, 404, "{p}");
        assert_eq!(r.text(), "HEARASIDE: this link is not valid (any more).");
    }
    assert!(Ws::open(t.port, &format!("/ws/l/{new}"), None).is_err());
    assert_eq!(get(t.port, &format!("/s/{}", t.send)).code, 200);
    assert_eq!(get(t.port, &format!("/s/{}", f.token)).code, 200);
    assert!(Ws::open(t.port, &format!("/ws/s/{}", f.token), None).is_ok());
    t.server.set_listen_enabled(true);
    assert_eq!(get(t.port, &format!("/l/{new}")).code, 200);
}

// ---- listener streaming --------------------------------------------------------------------------------------------

#[test]
fn listener_receives_hra1_from_the_stream_mix() {
    let t = start("stream");
    let l = t.layout();
    l.streamHeader.sampleRate.store(48000, Ordering::SeqCst);
    let mut ws = open_ok(t.port, &format!("/ws/l/{}", t.listen));
    assert!(ws.recv_op(1, 2 * SEC).is_some());
    // audio written after the listener joined (it starts at the newest sample)
    let left = sine(4800, 0, 440.0, 48000.0);
    let right: Vec<f32> = left.iter().map(|v| -v * 0.5).collect();
    l.streamHeader.active.store(1, Ordering::SeqCst);
    write_stream(l, &left, &right);
    let (mut got_l, mut got_r) = (Vec::new(), Vec::new());
    let end = Instant::now() + Duration::from_secs(5);
    while got_l.len() < 4800 && Instant::now() < end {
        let Some(p) = ws.recv_op(2, SEC) else {
            continue;
        };
        let (rate, chans, frames, pcm) = parse_hra1(&p);
        assert_eq!((rate, chans), (48000, 2));
        assert_eq!(pcm.len(), frames as usize * 2);
        assert!((240..=4096).contains(&frames), "{frames}");
        for c in pcm.as_chunks::<2>().0 {
            got_l.push(c[0]);
            got_r.push(c[1]);
        }
    }
    assert_eq!(got_l.len(), 4800);
    for i in 0..4800 {
        assert_eq!(got_l[i], pcm_i16(left[i]), "L {i}");
        assert_eq!(got_r[i], pcm_i16(right[i]), "R {i}");
    }
    // quiet DAW: the keep-alive HRA1 packet with zero frames comes after ~1 s
    l.streamHeader.active.store(0, Ordering::SeqCst);
    let p = ws.recv_op(2, 3 * SEC).expect("keep-alive");
    assert_eq!(parse_hra1(&p).2, 0);
}

#[test]
fn listener_gets_hra3_opus_when_it_asks_and_keeps_up() {
    let t = start("opus");
    let l = t.layout();
    l.streamHeader.sampleRate.store(48000, Ordering::SeqCst);
    let mut ws = open_ok(t.port, &format!("/ws/l/{}", t.listen));
    assert!(ws.recv_op(1, 2 * SEC).is_some());
    ws.send(1, br#"{"codec":"opus","bitrate":64000}"#);
    std::thread::sleep(Duration::from_millis(100));
    l.streamHeader.active.store(1, Ordering::SeqCst);
    let tone = sine(48000, 0, 1000.0, 48000.0);
    let t0 = Instant::now();
    write_stream(l, &tone, &tone);
    let mut dec = OpusDec::new();
    assert!(dec.init(48000, 2));
    let (mut packets, mut energy, mut last_seq) = (0, 0.0f32, None);
    let end = Instant::now() + Duration::from_secs(6);
    while packets < 100 && Instant::now() < end {
        let Some(p) = ws.recv_op(2, SEC) else {
            continue;
        };
        let (h, payload) = parse_hra3(&p).expect("HRA3");
        assert_eq!(
            (h.codec, h.chans, h.frames, h.flags),
            (CODEC_OPUS, 2, 480, 0)
        );
        if let Some(prev) = last_seq {
            assert_eq!(h.seq, prev + 1);
        } else {
            assert_eq!(h.seq, 0);
        }
        last_seq = Some(h.seq);
        assert_eq!(
            h.pos,
            480 * u64::from(h.seq + 1),
            "pos is the cursor after the packet"
        );
        let (mut a, mut b) = (vec![0.0f32; 480], vec![0.0f32; 480]);
        let mut ch: [&mut [f32]; 2] = [&mut a[..], &mut b[..]];
        assert_eq!(dec.decode(Some(payload), &mut ch, 480, false), 480);
        energy += a.iter().map(|v| v * v).sum::<f32>();
        packets += 1;
    }
    assert_eq!(packets, 100, "one second of audio is 100 packets of 10 ms");
    assert!(energy > 10.0, "decoded tone, energy {energy}");
    // C++ bug: one packet per loop round (each waits up to 10 ms for the browser) made the lag grow; here the
    // backlog of a late loop goes out at once
    assert!(
        t0.elapsed() < Duration::from_millis(1500),
        "{:?}",
        t0.elapsed()
    );
}

// ---- send-in link --------------------------------------------------------------------------------------------------

#[test]
fn send_in_link_writes_the_microphone_into_the_bus_and_newest_wins() {
    let t = start("sendin");
    let l = t.layout();
    let mut a = open_ok(t.port, &format!("/ws/s/{}", t.send));
    let mono: Vec<i16> = (0..480).map(|i| (i * 40 - 9000) as i16).collect();
    a.send(2, &hra1(44100, 1, &mono));
    assert!(wait_for(2 * SEC, || l.linkIn.active.load(Ordering::SeqCst) == 1));
    assert!(t.server.sender_active());
    assert_eq!(l.linkIn.sampleRate.load(Ordering::SeqCst), 44100);
    assert_eq!(l.linkIn.writePos.0.load(Ordering::SeqCst), 480);
    let (x, y) = read_ring(&l.linkInAudio.0, &l.linkIn.writePos.0, 0, 480);
    for i in 0..480 {
        assert_eq!(x[i], f32::from(mono[i]) / 32768.0);
        assert_eq!(y[i], x[i], "mono fills both channels");
    }
    // junk is ignored: wrong magic, short body, bad channel count, bad rate
    let mut bad = hra1(44100, 1, &mono);
    bad[0] = b'X';
    a.send(2, &bad);
    a.send(2, &hra1(44100, 1, &mono)[..15]);
    a.send(2, &hra1(100, 1, &mono));
    a.send(1, b"text");
    // a header that claims more frames than the body holds is cut to the body
    let mut short = hra1(48000, 2, &[1, 2, 3, 4]);
    short[12..16].copy_from_slice(&1000u32.to_le_bytes());
    a.send(2, &short);
    assert!(wait_for(2 * SEC, || l
        .linkIn
        .writePos
        .0
        .load(Ordering::SeqCst)
        == 482));

    // a second sender takes over; the first is dropped
    let mut b = open_ok(t.port, &format!("/ws/s/{}", t.send));
    b.send(2, &hra1(48000, 2, &[1000, -1000, 2000, -2000]));
    assert!(wait_for(2 * SEC, || l
        .linkIn
        .writePos
        .0
        .load(Ordering::SeqCst)
        == 484));
    assert!(a.closed_within(3 * SEC), "the older sender is closed");
    assert_eq!(
        l.linkIn.active.load(Ordering::SeqCst),
        1,
        "the old one leaving does not switch the link off"
    );
    drop(b);
    assert!(wait_for(3 * SEC, || l.linkIn.active.load(Ordering::SeqCst) == 0));
    assert!(wait_for(2 * SEC, || !t.server.sender_active()));
}

// ---- friends ------------------------------------------------------------------------------------------------------

fn friend_ws(t: &T, f: &FriendLink) -> Ws {
    open_ok(t.port, &format!("/ws/s/{}", f.token))
}

fn json(bytes: &[u8]) -> serde_json::Value {
    serde_json::from_slice(bytes)
        .unwrap_or_else(|e| panic!("{e}: {}", String::from_utf8_lossy(bytes)))
}

#[test]
fn friend_session_slot_hello_microphone_delay_stats_and_downlink() {
    let t = start("friend");
    let l = t.layout();
    let f = friend_link(2, 77, "Ann", "Bob");
    l.friends.0[2].id.store(77, Ordering::SeqCst);
    l.streamHeader.sampleRate.store(48000, Ordering::SeqCst);
    l.friendMixWrite.0.store(48000, Ordering::SeqCst);
    t.server.set_friend_links(vec![f.clone()]);
    let mut ws = friend_ws(&t, &f);
    let hello = json(&ws.recv_op(1, 2 * SEC).expect("hello"));
    assert_eq!(hello["v"], 3);
    assert_eq!(hello["name"], "Ann");
    assert_eq!(hello["host"], "Bob");
    assert_eq!(hello["codecs"], serde_json::json!(["opus", "pcm16"]));
    assert_eq!(hello["webrtc"], true);
    let h = &l.friends.0[2];
    assert!(wait_for(2 * SEC, || h.state.load(Ordering::SeqCst)
        == FriendState::kFriendLive));
    assert_eq!(t.server.friends_connected(), 1);
    assert_eq!(h.sampleRate.load(Ordering::SeqCst), 48000);

    // microphone: HRA2 mono with the Stream Mix position the friend was hearing (100 ms behind)
    let mono: Vec<i16> = (0..480).map(|i| ((i % 100) * 100 - 5000) as i16).collect();
    let mut sent = 0usize;
    let mut ping = None;
    let mut rtt_seen = false;
    let start_t = Instant::now();
    let mut first_down = None;
    while start_t.elapsed() < Duration::from_millis(1700) {
        ws.send(2, &hra2(48000, 1, &mono, 48000 - 4800));
        sent += 1;
        loop {
            match ws.recv(Duration::from_millis(8)) {
                Recv::Msg(1, d) => {
                    let text = String::from_utf8_lossy(&d).into_owned();
                    if let Some(rest) = text.strip_prefix("{\"ping\":") {
                        ping = Some(rest.trim_end_matches('}').to_owned());
                        ws.send(
                            1,
                            format!("{{\"pong\":{}}}", ping.as_ref().unwrap()).as_bytes(),
                        );
                    } else if text.starts_with("{\"delay\":") {
                        assert!(
                            text.trim_start_matches("{\"delay\":")
                                .trim_end_matches('}')
                                .parse::<f32>()
                                .is_ok()
                                && text.contains('.'),
                            "{text}"
                        );
                    }
                }
                Recv::Msg(2, d) => {
                    // the writer joined 20 ms before the newest sample and sent that at once (HRA2, 960 frames)
                    first_down.get_or_insert((
                        u64::from_le_bytes(d[16..24].try_into().unwrap()),
                        u32::from_le_bytes(d[12..16].try_into().unwrap()),
                    ));
                }
                Recv::Msg(..) => {}
                _ => break,
            }
        }
        let rtt = t.server.friend_stats(2).rtt_ms;
        rtt_seen |= rtt >= 0.0;
    }
    assert!(ping.is_some(), "the writer pings once a second");
    assert!(rtt_seen, "{:?}", t.server.friend_stats(2));
    let st = t.server.friend_stats(2);
    assert!((0.0..1000.0).contains(&st.rtt_ms), "{st:?}");
    assert!(st.up_kbps > 100.0, "{st:?}");
    assert!(
        st.mic_gap_max_ms > 0.0 && st.mic_gap_max_ms < 1500.0,
        "{st:?}"
    );
    // the voice is in the friend's ring, both channels, and the measured delay is 100 ms - 5 ms
    assert_eq!(h.writePos.0.load(Ordering::SeqCst), 480 * sent as u64);
    let (x, y) = read_ring(
        &l.friendAudio.0[2],
        &h.writePos.0,
        480 * (sent as u64 - 1),
        480,
    );
    for i in 0..480 {
        assert_eq!(x[i], f32::from(mono[i]) / 32768.0);
        assert_eq!(y[i], x[i]);
    }
    let d = bits_float(h.delayBits.load(Ordering::SeqCst));
    assert!((94.0..96.0).contains(&d), "{d}");
    assert!(bits_float(h.peakBits.load(Ordering::SeqCst)) > 0.1);
    assert_eq!(st.down_skips, 0);

    // remeasure: forget the delay
    t.server.remeasure_friend(2);
    assert!(wait_for(2 * SEC, || bits_float(
        h.delayBits.load(Ordering::SeqCst)
    ) < 0.0));

    // downlink: the Stream Mix the DAW makes, as HRA2 PCM from the newest sample
    let left = sine(4800, 48000, 330.0, 48000.0);
    let right = sine(4800, 48000, 660.0, 48000.0);
    write_mix(l, &left, &right);
    let mut got = 0usize;
    let mut first_pos = None;
    let end = Instant::now() + Duration::from_secs(4);
    while got < 4800 && Instant::now() < end {
        let Some(p) = ws.recv_op(2, SEC) else {
            continue;
        };
        assert_eq!(&p[..4], b"HRA2");
        let rate = u32::from_le_bytes(p[4..8].try_into().unwrap());
        let frames = u32::from_le_bytes(p[12..16].try_into().unwrap()) as usize;
        let pos = u64::from_le_bytes(p[16..24].try_into().unwrap());
        assert_eq!(
            (rate, u16::from_le_bytes(p[8..10].try_into().unwrap())),
            (48000, 2)
        );
        assert_eq!(p.len(), 24 + frames * 4);
        assert_eq!(pos, first_pos.unwrap_or(pos) + got as u64);
        first_pos.get_or_insert(pos);
        if pos >= 48000 {
            let k = (pos - 48000) as usize;
            for i in 0..frames {
                let s = |o: usize| {
                    i16::from_le_bytes([p[24 + (i * 2 + o) * 2], p[25 + (i * 2 + o) * 2]])
                };
                assert_eq!(s(0), pcm_i16(left[k + i]));
                assert_eq!(s(1), pcm_i16(right[k + i]));
            }
        }
        got += frames;
    }
    assert_eq!(first_pos, Some(48000));
    assert_eq!(got, 4800);
    assert_eq!(first_down, Some((48000 - 960, 960)));
}

#[test]
fn friend_microphone_formats_hra1_stereo_and_control_messages() {
    let t = start("friendfmt");
    let l = t.layout();
    let f = friend_link(3, 9, "Cy", "");
    t.server.set_friend_links(vec![f.clone()]);
    let mut ws = friend_ws(&t, &f);
    assert!(ws.recv_op(1, 2 * SEC).is_some());
    let h = &l.friends.0[3];
    // HRA1 stereo 96 kHz: left and right differ; the echo canceller stage then copies left to right (C++ behaviour)
    let st: Vec<i16> = (0..960)
        .flat_map(|i| [(i * 10) as i16, -(i as i16) * 5])
        .collect();
    ws.send(2, &hra1(96000, 2, &st));
    assert!(wait_for(2 * SEC, || h.writePos.0.load(Ordering::SeqCst) == 960));
    assert_eq!(h.sampleRate.load(Ordering::SeqCst), 96000);
    let (x, y) = read_ring(&l.friendAudio.0[3], &h.writePos.0, 0, 960);
    for i in 0..960 {
        assert_eq!(x[i], f32::from(st[2 * i]) / 32768.0);
        assert_eq!(y[i], x[i]);
    }
    // control messages
    assert_eq!(t.server.friend_aec_mode(3), AecMode::Auto);
    ws.send(1, br#"{"aec":"on"}"#);
    assert!(wait_for(2 * SEC, || t.server.friend_aec_mode(3) == AecMode::On));
    ws.send(1, br#"{"aec":"off"}"#);
    assert!(wait_for(2 * SEC, || t.server.friend_aec_mode(3) == AecMode::Off));
    ws.send(1, br#"{"aec":"auto"}"#);
    assert!(wait_for(2 * SEC, || t.server.friend_aec_mode(3) == AecMode::Auto));
    ws.send(1, br#"{"speakerMode":true}"#);
    assert!(wait_for(2 * SEC, || t.server.friend_aec_mode(3) == AecMode::On));
    ws.send(1, b"not json at all");
    ws.send(1, br#"[1,2]"#);
    ws.send(1, br#"{"aec":"bogus","codec":"bogus"}"#);
    assert_eq!(t.server.friend_aec_mode(3), AecMode::On);
    // the echo canceller state is reachable from the Hub
    t.server.set_friend_aec_mode(3, AecMode::Off);
    assert_eq!(t.server.friend_aec_mode(3), AecMode::Off);
    assert!(!t.server.friend_leak_detected(3));
    for _ in 0..hs_net::aec::LEAK_LATCH_TICKS {
        t.server.set_friend_leak_detected(3, true);
    }
    assert!(t.server.friend_leak_detected(3));
    assert_eq!(t.server.friend_erle_db(3), 0.0);
    // out-of-range slots are harmless
    t.server.set_friend_aec_mode(-1, AecMode::On);
    t.server.set_friend_aec_mode(8, AecMode::On);
    assert_eq!(t.server.friend_aec_mode(8), AecMode::Auto);
    assert_eq!(t.server.friend_stats(8).rtt_ms, -1.0);
    t.server.kick_friend(-1);
    t.server.kick_friend(99);
    t.server.remeasure_friend(99);
    // "hear myself": with it on, the friend's own voice comes back in the Stream Mix (PCM downlink)
    l.streamHeader.sampleRate.store(48000, Ordering::SeqCst);
    ws.send(1, br#"{"hearMyself":true}"#);
    std::thread::sleep(Duration::from_millis(100));
}

#[test]
fn friend_opus_over_websocket_with_loss_concealment_and_bad_frames() {
    let t = start("friendopus");
    let l = t.layout();
    let f = friend_link(1, 4, "Di", "");
    t.server.set_friend_links(vec![f.clone()]);
    let mut ws = friend_ws(&t, &f);
    assert!(ws.recv_op(1, 2 * SEC).is_some());
    let h = &l.friends.0[1];
    let mut enc = OpusEnc::new();
    assert!(enc.init(48000, 2, App::Voip));
    let tone = |k: usize| sine(480, 480 * k, 500.0, 48000.0);
    ws.send(2, &opus_packet(&mut enc, 1, 0, 0, &tone(0)));
    assert!(wait_for(2 * SEC, || h.writePos.0.load(Ordering::SeqCst) == 480));
    // seq 3 after seq 0: two packets lost, concealed (one from the FEC of this packet), then this packet
    ws.send(2, &opus_packet(&mut enc, 1, 3, 0, &tone(3)));
    assert!(wait_for(2 * SEC, || h.writePos.0.load(Ordering::SeqCst) == 480 * 4));
    // late and repeated packets are dropped
    ws.send(2, &opus_packet(&mut enc, 1, 2, 0, &tone(2)));
    ws.send(2, &opus_packet(&mut enc, 1, 3, 0, &tone(3)));
    // C++ bug: a packet that claims 60000 frames made ten concealment writes of 60000 frames into the ring
    let mut huge = opus_packet(&mut enc, 1, 9, 0, &tone(9));
    huge[20..22].copy_from_slice(&60000u16.to_le_bytes());
    ws.send(2, &huge);
    let mut zero = opus_packet(&mut enc, 1, 10, 0, &tone(10));
    zero[20..22].copy_from_slice(&0u16.to_le_bytes());
    ws.send(2, &zero);
    // a PCM codec inside HRA3 is not microphone audio
    let mut pcm = opus_packet(&mut enc, 1, 11, 0, &tone(11));
    pcm[4] = 0;
    ws.send(2, &pcm);
    // a good packet proves the session still runs
    ws.send(2, &opus_packet(&mut enc, 1, 4, 0, &tone(4)));
    assert!(wait_for(2 * SEC, || h.writePos.0.load(Ordering::SeqCst) == 480 * 5));
    assert_eq!(h.writePos.0.load(Ordering::SeqCst), 480 * 5);
    let (x, _) = read_ring(&l.friendAudio.0[1], &h.writePos.0, 480 * 4, 480);
    assert!(x.iter().map(|v| v * v).sum::<f32>() > 1.0, "decoded tone");
}

#[test]
fn friend_newest_connection_wins_kick_and_link_removal() {
    let t = start("slots");
    let l = t.layout();
    let a = friend_link(4, 40, "A", "");
    let b = friend_link(5, 50, "B", "");
    l.friends.0[4].id.store(40, Ordering::SeqCst);
    l.friends.0[5].id.store(50, Ordering::SeqCst);
    t.server.set_friend_links(vec![a.clone(), b.clone()]);
    let state = |s: usize| l.friends.0[s].state.load(Ordering::SeqCst);

    let mut a1 = friend_ws(&t, &a);
    let mut b1 = friend_ws(&t, &b);
    assert!(wait_for(2 * SEC, || t.server.friends_connected() == 2));
    assert_eq!(
        (state(4), state(5)),
        (FriendState::kFriendLive, FriendState::kFriendLive)
    );

    // the same friend again: the older connection ends, the slot stays live, the other friend is not touched
    let mut a2 = friend_ws(&t, &a);
    assert!(
        a1.closed_within(3 * SEC),
        "older connection of the same friend is closed"
    );
    assert_eq!(state(4), FriendState::kFriendLive);
    assert!(wait_for(2 * SEC, || t.server.friends_connected() == 2));
    assert!(a2.recv_op(1, 2 * SEC).is_some());
    // (the signaling of its WebRTC offer counts as waiting control bytes for a moment: ping after it went out)
    assert!(b1.recv_op(1, 2 * SEC).is_some());
    std::thread::sleep(Duration::from_millis(500));
    b1.send(9, b"p");
    assert!(b1.recv_op(10, 2 * SEC).is_some(), "B is still connected");
    assert_eq!(state(5), FriendState::kFriendLive);

    // kick: the connection ends and the slot goes offline
    t.server.kick_friend(5);
    assert!(b1.closed_within(3 * SEC));
    assert!(wait_for(2 * SEC, || state(5) == FriendState::kFriendOffline));
    assert_eq!(state(4), FriendState::kFriendLive);
    // a new connection after a kick works (the flag is reset)
    let mut b2 = friend_ws(&t, &b);
    assert!(b2.recv_op(1, 2 * SEC).is_some());
    assert!(wait_for(2 * SEC, || state(5) == FriendState::kFriendLive));

    // the link removed from the table (or given to someone else): the session ends within a moment
    t.server.set_friend_links(vec![b.clone()]);
    assert!(a2.closed_within(3 * SEC));
    assert!(wait_for(2 * SEC, || state(4) == FriendState::kFriendOffline));
    let mut changed = b.clone();
    changed.id = 51;
    t.server.set_friend_links(vec![changed]);
    assert!(b2.closed_within(3 * SEC));
    assert!(Ws::open(t.port, &format!("/ws/s/{}", a.token), None).is_err());

    // stopping the server takes every friend offline and closes the sockets
    t.server.set_friend_links(vec![a.clone()]);
    let mut a3 = friend_ws(&t, &a);
    assert!(a3.recv_op(1, 2 * SEC).is_some());
    t.server.stop();
    assert!(a3.closed_within(3 * SEC));
    assert_eq!(state(4), FriendState::kFriendOffline);
    assert_eq!(t.server.friends_connected(), 0);
    assert!(!t.server.running());
    assert_eq!(t.server.port(), 0);
}

#[test]
fn friend_send_queue_stays_bounded_with_a_stalled_reader() {
    let t = start("stall");
    let l = t.layout();
    let f = friend_link(6, 60, "S", "");
    t.server.set_friend_links(vec![f.clone()]);
    l.streamHeader.sampleRate.store(48000, Ordering::SeqCst);
    let mut ws = friend_ws(&t, &f); // never read from again
    assert!(wait_for(2 * SEC, || t.server.friends_connected() == 1));
    let tone = sine(2400, 0, 300.0, 48000.0);
    let mut max_queue = 0usize;
    let end = Instant::now() + Duration::from_secs(4);
    let mut cycles = 0u64;
    while Instant::now() < end {
        write_mix(l, &tone, &tone); // 50 ms of audio every ~10 ms: five times faster than real time
        cycles += 1;
        max_queue = max_queue.max(t.server.friend_queue_bytes(6));
        std::thread::sleep(Duration::from_millis(10));
        if t.server.friend_stats(6).down_skips > 0 && cycles > 200 {
            break;
        }
    }
    let st = t.server.friend_stats(6);
    assert!(
        st.down_skips > 0,
        "a line that cannot keep up makes the Hub jump ahead: {st:?}"
    );
    // one PCM packet (4096 frames) + the control messages + the signaling, never a backlog
    assert!(max_queue < 48 * 1024, "queue held {max_queue} bytes");
    assert!(st.down_lag_max_ms > 0.0);
    // the stalled friend does not hold the server up: stopping still works and is quick
    let t0 = Instant::now();
    t.server.stop();
    assert!(t0.elapsed() < Duration::from_secs(4), "{:?}", t0.elapsed());
    ws.send(9, b"x");
}

// ---- limits -------------------------------------------------------------------------------------------------------

#[test]
fn listener_cap_and_per_visitor_limit_behind_the_tunnel() {
    let t = start("limits");
    t.server.set_webrtc_enabled(false);
    let path = format!("/ws/l/{}", t.listen);
    // behind the tunnel every connection comes from 127.0.0.1; the visitor is Cf-Connecting-Ip: 6 at once
    let mut held = Vec::new();
    for i in 0..6 {
        let mut w = Ws::open_with(t.port, &path, None, "Cf-Connecting-Ip: 203.0.113.9\r\n")
            .unwrap_or_else(|r| panic!("{i}: {}", r.code));
        assert!(w.recv_op(1, 2 * SEC).is_some());
        held.push(w);
    }
    let r = get_with(
        t.port,
        &format!("/l/{}", t.listen),
        "Cf-Connecting-Ip: 203.0.113.9\r\n",
    );
    assert_eq!(r.code, 503);
    assert_eq!(
        r.text(),
        "HEARASIDE: too many connections from your address."
    );
    assert_eq!(
        get_with(
            t.port,
            &format!("/l/{}", t.listen),
            "Cf-Connecting-Ip: 203.0.113.10\r\n"
        )
        .code,
        200
    );
    // an over-long header value is not trusted as a visitor id
    assert_eq!(
        get_with(
            t.port,
            &format!("/l/{}", t.listen),
            &format!("Cf-Connecting-Ip: {}\r\n", "9".repeat(60))
        )
        .code,
        200
    );

    // listeners: 30 (40 places - 8 friends - the send-in link), the 31st is told to come back later
    for i in 0..24 {
        let mut w = Ws::open_with(
            t.port,
            &path,
            None,
            &format!("Cf-Connecting-Ip: 198.51.100.{i}\r\n"),
        )
        .unwrap_or_else(|r| panic!("{i}: {}", r.code));
        assert!(w.recv_op(1, 2 * SEC).is_some());
        held.push(w);
    }
    assert_eq!(held.len(), 30);
    assert!(wait_for(2 * SEC, || t.server.listeners() == 30));
    let r = Ws::open_with(t.port, &path, None, "Cf-Connecting-Ip: 192.0.2.1\r\n")
        .err()
        .expect("31st listener");
    assert_eq!(r.code, 503);
    assert_eq!(
        r.text(),
        "HEARASIDE: too many listeners right now, try again later."
    );
    drop(held.pop());
    assert!(wait_for(3 * SEC, || t.server.listeners() == 29));
    assert!(Ws::open_with(t.port, &path, None, "Cf-Connecting-Ip: 192.0.2.2\r\n").is_ok());
}

#[test]
fn connections_without_a_whole_head_are_limited_and_time_out() {
    let t = start("pending");
    let mut idle: Vec<TcpStream> = (0..24)
        .map(|_| TcpStream::connect(("127.0.0.1", t.port)).unwrap())
        .collect();
    std::thread::sleep(Duration::from_millis(400));
    // the 25th place is not given: the server closes it at once
    let mut extra = TcpStream::connect(("127.0.0.1", t.port)).unwrap();
    extra
        .set_read_timeout(Some(Duration::from_secs(2)))
        .unwrap();
    let mut b = [0u8; 8];
    let r = extra.read(&mut b);
    assert!(
        matches!(r, Ok(0))
            || r.as_ref().is_err_and(
                |e| e.kind() != ErrorKind::WouldBlock && e.kind() != ErrorKind::TimedOut
            ),
        "{r:?}"
    );
    // a connection that sends nothing is closed after 5 s (slow loris), which frees its place
    let first = &mut idle[0];
    first
        .set_read_timeout(Some(Duration::from_secs(8)))
        .unwrap();
    let t0 = Instant::now();
    let r = first.read(&mut b);
    assert!(matches!(r, Ok(0)) || r.is_err(), "{r:?}");
    assert!(
        t0.elapsed() > Duration::from_secs(3) && t0.elapsed() < Duration::from_secs(8),
        "{:?}",
        t0.elapsed()
    );
    assert_eq!(get(t.port, &format!("/l/{}", t.listen)).code, 200);
}

// ---- WebRTC signaling inside a friend session ---------------------------------------------------------------------

fn client_config() -> RtcPeerConfig {
    RtcPeerConfig {
        ice_servers: stun_config(),
        ..RtcPeerConfig::default()
    }
}

/// The browser's side of the signaling: answers the Hub's offer over the WebSocket until the DataChannel is open.
fn datachannel(ws: &mut Ws) -> RtcPeer {
    let mut browser = RtcPeer::new(&client_config()).unwrap();
    let mut got_offer = false;
    let deadline = Instant::now() + Duration::from_secs(15);
    while !browser.is_open() && Instant::now() < deadline {
        if let Recv::Msg(1, d) = ws.recv(Duration::from_millis(5)) {
            let m = json(&d);
            match m["type"].as_str() {
                Some("rtc_offer") => {
                    browser
                        .set_remote_description("offer", m["sdp"].as_str().unwrap())
                        .unwrap();
                    got_offer = true;
                }
                Some("rtc_candidate") => {
                    let _ = browser.add_remote_candidate(
                        m["candidate"].as_str().unwrap(),
                        m["mid"].as_str().unwrap_or(""),
                    );
                }
                _ => {}
            }
        }
        while let Some(ev) = browser.poll(Duration::ZERO).unwrap() {
            match ev {
                RtcEvent::LocalDescription { sdp, .. } => {
                    ws.send(
                        1,
                        serde_json::json!({"type": "rtc_answer", "sdp": sdp})
                            .to_string()
                            .as_bytes(),
                    );
                }
                RtcEvent::LocalCandidate { candidate, mid } => {
                    ws.send(
                        1,
                        serde_json::json!({"type": "rtc_candidate", "candidate": candidate, "mid": mid})
                            .to_string()
                            .as_bytes(),
                    );
                }
                _ => {}
            }
        }
    }
    assert!(got_offer, "the Hub offers a DataChannel");
    assert!(
        browser.is_open(),
        "DataChannel up, state {:?}",
        browser.state()
    );
    browser
}

#[test]
fn listener_webrtc_signaling_carries_opus_over_the_datachannel() {
    let t = start("rtclisten");
    let l = t.layout();
    l.streamHeader.sampleRate.store(48000, Ordering::SeqCst);
    let mut ws = open_ok(t.port, &format!("/ws/l/{}", t.listen));
    assert!(ws.recv_op(1, 2 * SEC).is_some());
    let mut browser = datachannel(&mut ws);
    ws.send(1, br#"{"codec":"opus"}"#);
    std::thread::sleep(Duration::from_millis(100));
    l.streamHeader.active.store(1, Ordering::SeqCst);
    let tone = sine(4800, 0, 900.0, 48000.0);
    write_stream(l, &tone, &tone);
    let mut dec = OpusDec::new();
    assert!(dec.init(48000, 2));
    let (mut packets, mut energy) = (0, 0.0f32);
    let end = Instant::now() + Duration::from_secs(8);
    while packets < 10 && Instant::now() < end {
        while let Some(ev) = browser.poll(Duration::from_millis(2)).unwrap() {
            if let RtcEvent::Message(d) = ev {
                let (h, payload) = parse_hra3(&d).expect("HRA3 over the DataChannel");
                assert_eq!(
                    (h.codec, h.chans, h.frames, h.flags),
                    (CODEC_OPUS, 2, 480, 0)
                );
                let (mut a, mut b) = (vec![0.0f32; 480], vec![0.0f32; 480]);
                let mut ch: [&mut [f32]; 2] = [&mut a[..], &mut b[..]];
                assert_eq!(dec.decode(Some(payload), &mut ch, 480, false), 480);
                energy += a.iter().map(|v| v * v).sum::<f32>();
                packets += 1;
            }
        }
        let _ = ws.recv(Duration::from_millis(1));
    }
    assert!(
        packets >= 10 && energy > 1.0,
        "{packets} packets, energy {energy}"
    );
    drop(ws);
    assert!(wait_for(3 * SEC, || t.server.listeners() == 0));
}

#[test]
fn friend_webrtc_signaling_brings_up_a_datachannel_in_both_directions() {
    let t = start("rtc");
    let l = t.layout();
    let f = friend_link(7, 70, "Rtc", "");
    t.server.set_friend_links(vec![f.clone()]);
    l.streamHeader.sampleRate.store(48000, Ordering::SeqCst);
    let mut ws = friend_ws(&t, &f);
    assert!(ws.recv_op(1, 2 * SEC).is_some());
    let mut browser = datachannel(&mut ws);

    // browser -> Hub: an Opus microphone packet over the DataChannel lands in the friend's ring
    let mut enc = OpusEnc::new();
    assert!(enc.init(48000, 2, App::Voip));
    let h = &l.friends.0[7];
    let tone = sine(480, 0, 700.0, 48000.0);
    assert!(browser.send(&opus_packet(&mut enc, 1, 0, 0, &tone)));
    // Hub -> browser: ask for Opus, then the Stream Mix comes as HRA3 over the DataChannel (5 ms packets, FEC flag)
    ws.send(1, br#"{"codec":"opus"}"#);
    std::thread::sleep(Duration::from_millis(100));
    let mix = sine(2400, 0, 800.0, 48000.0);
    write_mix(l, &mix, &mix);
    let mut dec = OpusDec::new();
    assert!(dec.init(48000, 2));
    let (mut packets, mut energy, mut writes) = (0, 0.0f32, 0);
    let end = Instant::now() + Duration::from_secs(8);
    while (packets < 10 || h.writePos.0.load(Ordering::SeqCst) < 480) && Instant::now() < end {
        while let Some(ev) = browser.poll(Duration::from_millis(2)).unwrap() {
            if let RtcEvent::Message(d) = ev {
                let (hd, payload) = parse_hra3(&d).expect("HRA3 over the DataChannel");
                assert_eq!(
                    (hd.codec, hd.chans, hd.frames, hd.flags),
                    (CODEC_OPUS, 2, 240, FLAG_FEC)
                );
                let (mut a, mut b) = (vec![0.0f32; 240], vec![0.0f32; 240]);
                let mut ch: [&mut [f32]; 2] = [&mut a[..], &mut b[..]];
                assert_eq!(dec.decode(Some(payload), &mut ch, 240, false), 240);
                energy += a.iter().map(|v| v * v).sum::<f32>();
                packets += 1;
                writes += 1;
            }
        }
        let _ = ws.recv(Duration::from_millis(1));
    }
    assert!(writes >= 10, "{writes} packets");
    assert!(energy > 1.0, "decoded the mix, energy {energy}");
    assert_eq!(
        h.writePos.0.load(Ordering::SeqCst),
        480,
        "the DataChannel microphone reached the bus"
    );
    let (x, _) = read_ring(&l.friendAudio.0[7], &h.writePos.0, 0, 480);
    assert!(x.iter().map(|v| v * v).sum::<f32>() > 1.0);
    // ending the session closes the peer
    drop(ws);
    assert!(wait_for(3 * SEC, || t.server.friends_connected() == 0));
}

// ---- cloudflared tunnel ------------------------------------------------------------------------------------------

fn example(name: &str) -> PathBuf {
    let exe = std::env::current_exe().unwrap();
    let dir = exe.parent().unwrap().parent().unwrap().join("examples");
    let file = dir.join(format!("{name}{}", std::env::consts::EXE_SUFFIX));
    if !file.exists() {
        // the same profile as this test binary, so the example lands next to it
        let mut cmd =
            std::process::Command::new(std::env::var("CARGO").unwrap_or_else(|_| "cargo".into()));
        cmd.args(["build", "-p", "hs-net", "--example", name]);
        if !cfg!(debug_assertions) {
            cmd.arg("--release");
        }
        let ok = cmd.status().is_ok_and(|s| s.success());
        assert!(
            ok && file.exists(),
            "build the {name} example: {}",
            file.display()
        );
    }
    file
}

/// A folder holding the fake as `cloudflared` and its mode file.
fn fake_dir(tag: &str, mode: &str) -> PathBuf {
    let dir = std::env::temp_dir().join(unique(tag));
    std::fs::create_dir_all(&dir).unwrap();
    let exe = dir.join(format!("cloudflared{}", std::env::consts::EXE_SUFFIX));
    std::fs::copy(example("fake-cloudflared"), &exe).unwrap();
    std::fs::write(dir.join("cloudflared.mode"), mode).unwrap();
    dir
}

fn tunnel_server(dir: &Path) -> (ShareServer, Arc<SharedMemory>) {
    let server = ShareServer::new();
    server.set_cloudflared_source(CloudflaredSource::Dirs(vec![dir.to_path_buf()]));
    (
        server,
        Arc::new(SharedMemory::open(&unique("tunnel")).unwrap()),
    )
}

fn fake_pid(dir: &Path) -> u32 {
    assert!(wait_for(5 * SEC, || dir.join("cloudflared.pid").exists()));
    std::thread::sleep(Duration::from_millis(100));
    std::fs::read_to_string(dir.join("cloudflared.pid"))
        .unwrap()
        .trim()
        .parse()
        .unwrap()
}

#[test]
fn tunnel_starts_becomes_ready_and_dies_with_the_server() {
    let _place = place(1);
    let dir = fake_dir("cf-ready", "ready");
    let (server, bus) = tunnel_server(&dir);
    assert!(server.start(bus, "listen-token-abcdefghij", "send-token-abcdefghijk"));
    let port = server.port();
    assert!(matches!(server.tunnel(), Tunnel::Starting | Tunnel::Ready));
    assert!(
        wait_for(10 * SEC, || server.tunnel() == Tunnel::Ready),
        "{:?}",
        server.tunnel()
    );
    let base = "https://fake-test-tunnel.trycloudflare.com";
    assert_eq!(server.public_base(), base);
    assert_eq!(
        server.listen_url(),
        format!("{base}/l/listen-token-abcdefghij")
    );
    assert_eq!(
        server.send_url(),
        format!("{base}/s/send-token-abcdefghijk")
    );
    assert_eq!(server.friend_url("tok"), format!("{base}/s/tok"));
    assert!(
        server
            .local_listen_url()
            .starts_with(&format!("http://127.0.0.1:{port}/l/"))
    );
    assert!(server.origin_allowed(base));
    assert!(!server.origin_allowed("https://other-tunnel.trycloudflare.com"));
    // the arguments the C++ used
    let pid = fake_pid(&dir);
    assert_eq!(
        std::fs::read_to_string(dir.join("cloudflared.args")).unwrap(),
        format!("tunnel --no-autoupdate --url http://127.0.0.1:{port}")
    );
    assert!(process_alive(pid));
    server.stop();
    assert!(
        wait_for(5 * SEC, || !process_alive(pid)),
        "cloudflared (pid {pid}) must die with the server"
    );
    assert_eq!(server.tunnel(), Tunnel::Off);
    assert_eq!(server.public_base(), "");
    // started again: a new tunnel
    let (server2, bus2) = (
        server,
        Arc::new(SharedMemory::open(&unique("tunnel2")).unwrap()),
    );
    assert!(server2.start(bus2, "a", "b"));
    assert!(wait_for(10 * SEC, || server2.tunnel() == Tunnel::Ready));
    drop(server2); // Drop stops
    std::fs::remove_dir_all(&dir).ok();
}

#[test]
fn tunnel_url_split_over_two_writes() {
    let _place = place(1);
    let dir = fake_dir("cf-split", "split");
    let (server, bus) = tunnel_server(&dir);
    assert!(server.start(bus, "l", "s"));
    assert_eq!(
        server.listen_url(),
        "",
        "links wait for the tunnel (20 s) instead of handing out a LAN link"
    );
    assert!(wait_for(10 * SEC, || server.tunnel() == Tunnel::Ready));
    assert_eq!(server.public_base(), "https://split-test.trycloudflare.com");
    drop(server);
    std::fs::remove_dir_all(&dir).ok();
}

#[test]
fn tunnel_api_error_is_not_a_tunnel_and_exit_is_failed() {
    let _place = place(1);
    // C++ bug: the error line "Post https://api.trycloudflare.com/tunnel: ..." matched the URL pattern
    let dir = fake_dir("cf-api", "api-error");
    let (server, bus) = tunnel_server(&dir);
    assert!(server.start(bus, "l", "s"));
    let pid = fake_pid(&dir);
    std::thread::sleep(Duration::from_millis(800));
    assert_eq!(server.tunnel(), Tunnel::Starting);
    assert_eq!(server.public_base(), "");
    assert_eq!(server.listen_url(), "");
    server.stop();
    assert!(wait_for(5 * SEC, || !process_alive(pid)));
    std::fs::remove_dir_all(&dir).ok();

    let dir = fake_dir("cf-exit", "exit");
    let (server, bus) = tunnel_server(&dir);
    assert!(server.start(bus, "l", "s"));
    assert!(
        wait_for(10 * SEC, || server.tunnel() == Tunnel::Failed),
        "{:?}",
        server.tunnel()
    );
    assert!(
        server.listen_url().starts_with("http://"),
        "no tunnel: the LAN link is handed out"
    );
    assert!(
        server
            .listen_url()
            .ends_with(&format!(":{}/l/l", server.port()))
    );
    assert!(
        get(server.port(), "/l/l").code == 200,
        "the local server keeps working"
    );
    drop(server);
    std::fs::remove_dir_all(&dir).ok();
}

#[test]
fn tunnel_missing_and_unstartable() {
    let _place = place(1);
    let empty = std::env::temp_dir().join(unique("cf-none"));
    std::fs::create_dir_all(&empty).unwrap();
    let (server, bus) = tunnel_server(&empty);
    assert!(server.start(bus, "l", "s"));
    assert_eq!(server.tunnel(), Tunnel::Missing);
    assert!(server.listen_url().starts_with("http://"));
    drop(server);
    // a "cloudflared" that cannot run
    std::fs::write(
        empty.join(format!("cloudflared{}", std::env::consts::EXE_SUFFIX)),
        b"not a program",
    )
    .unwrap();
    let (server, bus) = tunnel_server(&empty);
    assert!(server.start(bus, "l", "s"));
    assert_eq!(server.tunnel(), Tunnel::Failed);
    assert_eq!(get(server.port(), "/l/l").code, 200);
    drop(server);
    // relative folders are not searched
    let (server, bus) = tunnel_server(Path::new("."));
    assert!(server.start(bus, "l", "s"));
    assert_eq!(server.tunnel(), Tunnel::Missing);
    // Disabled never looks
    server.set_cloudflared_source(CloudflaredSource::Disabled);
    let bus = Arc::new(SharedMemory::open(&unique("cf-off")).unwrap());
    assert!(server.start(bus, "l", "s"));
    assert_eq!(server.tunnel(), Tunnel::Off);
    std::fs::remove_dir_all(&empty).ok();
}

#[test]
#[cfg(windows)]
fn a_crashed_host_takes_cloudflared_with_it() {
    let _place = place(1);
    // the job object (KILL_ON_JOB_CLOSE) of the C++ `Tunnelled`: the host process is killed, not stopped
    let dir = fake_dir("cf-crash", "ready");
    let mut host = std::process::Command::new(example("share-host"))
        .arg(unique("crashhost"))
        .arg(&dir)
        .stdout(std::process::Stdio::piped())
        .spawn()
        .unwrap();
    let mut out = host.stdout.take().unwrap();
    let mut line = [0u8; 5];
    out.read_exact(&mut line).unwrap();
    assert_eq!(&line, b"ready");
    let pid = fake_pid(&dir);
    assert!(process_alive(pid));
    host.kill().unwrap();
    host.wait().unwrap();
    assert!(
        wait_for(8 * SEC, || !process_alive(pid)),
        "cloudflared (pid {pid}) outlived its host"
    );
    std::fs::remove_dir_all(&dir).ok();
}

// ---- misc ---------------------------------------------------------------------------------------------------------

#[test]
fn urls_ports_and_restart() {
    let t = start_placed("urls", place(2));
    assert!(t.server.running());
    assert!(t.server.lan_base().ends_with(&format!(":{}", t.port)));
    assert!(
        t.server.listen_url().starts_with("http://"),
        "no tunnel: LAN link at once"
    );
    assert!(t.server.send_url().ends_with(&format!("/s/{}", t.send)));
    assert!(t.server.friend_url("abc").ends_with("/s/abc"));
    let radmin = t.server.radmin_base();
    assert!(radmin.is_empty() || radmin.starts_with("http://26."));
    assert_eq!(
        t.server.radmin_friend_url("abc").is_empty(),
        radmin.is_empty()
    );
    // a second server on the same machine takes the next port
    let other = ShareServer::new();
    other.set_cloudflared_source(CloudflaredSource::Disabled);
    let bus = Arc::new(SharedMemory::open(&unique("urls2")).unwrap());
    assert!(other.start(bus, "a", "b"));
    assert_ne!(other.port(), t.port);
    // start twice = restart
    let bus = Arc::new(SharedMemory::open(&unique("urls3")).unwrap());
    assert!(other.start(bus, "c", "d"));
    assert_eq!(get(other.port(), "/l/c").code, 200);
    assert_eq!(get(other.port(), "/l/a").code, 404);
    other.stop();
    other.stop();
    assert_eq!(other.port(), 0);
    assert!(get_refused(t.port + 100));
    assert_eq!(ShareServer::new_token().len(), 26);
}

fn get_refused(port: u16) -> bool {
    TcpStream::connect(("127.0.0.1", port)).is_err()
}

#[test]
fn stop_is_quick_with_connections_open_and_leaves_ports_free() {
    let t = start("stopping");
    t.server.set_webrtc_enabled(false);
    let mut socks: Vec<Ws> = (0..5)
        .map(|_| open_ok(t.port, &format!("/ws/l/{}", t.listen)))
        .collect();
    let half = TcpStream::connect(("127.0.0.1", t.port)).unwrap();
    let t0 = Instant::now();
    t.server.stop();
    assert!(t0.elapsed() < Duration::from_secs(2), "{:?}", t0.elapsed());
    for s in &mut socks {
        assert!(s.closed_within(2 * SEC));
    }
    drop(half);
    assert_eq!(t.server.port(), 0);
    assert!(!t.server.running());
}

#[test]
fn websocket_fragments_join_and_protocol_violations_close() {
    let t = start("frames");
    let l = t.layout();
    let path = format!("/ws/s/{}", t.send);
    let mut a = open_ok(t.port, &path);
    // one binary message in three fragments with a ping between them
    let msg = hra1(48000, 1, &[100, 200, 300, 400]);
    a.send_raw(0x02, &msg[..10]); // binary, not final
    a.send_raw(0x89, b"hi"); // ping in the middle
    a.send_raw(0x00, &msg[10..20]); // continuation
    a.send_raw(0x80, &msg[20..]); // final continuation
    assert!(wait_for(2 * SEC, || l
        .linkIn
        .writePos
        .0
        .load(Ordering::SeqCst)
        == 4));
    assert_eq!(a.recv_op(10, 2 * SEC).as_deref(), Some(&b"hi"[..]));
    // a reserved bit (no extension was negotiated), a fragmented control frame and a reserved opcode end the connection
    for first in [0xC2u8, 0x09, 0x83] {
        let mut v = open_ok(t.port, &path);
        v.send_raw(first, b"x");
        assert!(v.closed_within(3 * SEC), "first byte {first:#x}");
    }
    // the close frame ends it politely
    let mut c = open_ok(t.port, &path);
    c.send_raw(0x88, &[]);
    assert!(c.closed_within(3 * SEC));
}

#[test]
fn the_server_can_be_shared_between_threads() {
    fn send_sync<T: Send + Sync>() {}
    send_sync::<ShareServer>();
}

// ---- review fixes ---------------------------------------------------------------------------------------------------

#[test]
fn bytes_before_a_fin_are_not_lost() {
    // C++ bug (and the first Rust port): HUP together with data counted as "closed" and the data was dropped
    let t = start("halfclose");
    let l = t.layout();
    let mut s = TcpStream::connect(("127.0.0.1", t.port)).unwrap();
    s.set_read_timeout(Some(Duration::from_secs(5))).unwrap();
    s.write_all(format!("GET /l/{} HTTP/1.1\r\nHost: x\r\n\r\n", t.listen).as_bytes())
        .unwrap();
    s.shutdown(std::net::Shutdown::Write).unwrap();
    let mut raw = Vec::new();
    let _ = s.read_to_end(&mut raw);
    assert_eq!(parse_response(&raw).code, 200, "request head, then FIN");
    // the last WebSocket frame before the FIN is processed
    for round in 0..5u64 {
        let mut a = open_ok(t.port, &format!("/ws/s/{}", t.send));
        let before = l.linkIn.writePos.0.load(Ordering::SeqCst);
        a.send(2, &hra1(48000, 1, &[1, 2, 3, 4, 5, 6, 7, 8]));
        a.s.shutdown(std::net::Shutdown::Write).unwrap();
        assert!(
            wait_for(2 * SEC, || l.linkIn.writePos.0.load(Ordering::SeqCst)
                == before + 8),
            "round {round}"
        );
        assert!(a.closed_within(3 * SEC));
    }
}

#[test]
fn a_lone_ping_does_not_end_the_connection_after_the_read_timeout() {
    let t = start("lonelyping");
    t.server.set_webrtc_enabled(false);
    let f = friend_link(1, 1, "P", "");
    t.server.set_friend_links(vec![f.clone()]);
    // listener (message read timeout 1 s) and friend (5 s): ping each once, then wait past the timeout (a send-in link is
    // dropped after 5 s of silence by design, so it is not part of this)
    let mut listener = open_ok(t.port, &format!("/ws/l/{}", t.listen));
    let mut friend = friend_ws(&t, &f);
    for w in [&mut listener, &mut friend] {
        w.send(9, b"hi");
    }
    std::thread::sleep(Duration::from_millis(5600));
    assert_eq!(t.server.listeners(), 1);
    assert_eq!(t.server.friends_connected(), 1);
    for w in [&mut listener, &mut friend] {
        assert_eq!(w.recv_op(10, 2 * SEC).as_deref(), Some(&b"hi"[..]));
        w.send(9, b"again");
        assert_eq!(w.recv_op(10, 2 * SEC).as_deref(), Some(&b"again"[..]));
    }
}

#[test]
fn pcm_microphone_packets_are_capped() {
    let t = start("pcmcap");
    t.server.set_webrtc_enabled(false);
    let l = t.layout();
    let f = friend_link(2, 2, "C", "");
    t.server.set_friend_links(vec![f.clone()]);
    let mut sender = open_ok(t.port, &format!("/ws/s/{}", t.send));
    let mut friend = friend_ws(&t, &f);
    let big = |n: usize| hra1(48000, 1, &vec![100i16; n]);
    sender.send(2, &big(5761));
    sender.send(2, &big(5760));
    assert!(wait_for(2 * SEC, || l
        .linkIn
        .writePos
        .0
        .load(Ordering::SeqCst)
        == 5760));
    friend.send(2, &big(5761));
    friend.send(2, &hra2(48000, 1, &vec![100i16; 5761], 0));
    friend.send(2, &big(5760));
    assert!(wait_for(2 * SEC, || l.friends.0[2]
        .writePos
        .0
        .load(Ordering::SeqCst)
        == 5760));
    std::thread::sleep(Duration::from_millis(200));
    assert_eq!(
        l.friends.0[2].writePos.0.load(Ordering::SeqCst),
        5760,
        "the longer ones were dropped"
    );
    assert_eq!(l.linkIn.writePos.0.load(Ordering::SeqCst), 5760);
}

#[test]
fn takeover_never_leaves_a_live_friend_marked_offline() {
    // C++ bug: the older session could store Offline after the newer one stored Live
    let t = start("takeover");
    t.server.set_webrtc_enabled(false);
    let l = t.layout();
    let f = friend_link(3, 33, "T", "");
    l.friends.0[3].id.store(33, Ordering::SeqCst);
    t.server.set_friend_links(vec![f.clone()]);
    let h = &l.friends.0[3];
    let mut old = friend_ws(&t, &f);
    for round in 0..25 {
        let new = friend_ws(&t, &f);
        // the older one ends by itself; its end must not touch the slot
        assert!(old.closed_within(3 * SEC), "round {round}");
        std::thread::sleep(Duration::from_millis(30));
        assert_eq!(
            h.state.load(Ordering::SeqCst),
            FriendState::kFriendLive,
            "round {round}"
        );
        assert_eq!(t.server.friends_connected(), 1, "round {round}");
        old = new;
    }
}

#[test]
fn stop_joins_every_webrtc_thread() {
    // WebRTC enabled: every listener and friend has a worker thread; stop() leaves none running (they hold the bus)
    let t = start_placed("rtcjoin", place(2));
    let f = friend_link(0, 1, "J", "");
    t.server.set_friend_links(vec![f.clone()]);
    let mut socks: Vec<Ws> = (0..4)
        .map(|_| open_ok(t.port, &format!("/ws/l/{}", t.listen)))
        .collect();
    socks.push(friend_ws(&t, &f));
    assert!(
        wait_for(5 * SEC, || t.server.rtc_threads() == 5),
        "{}",
        t.server.rtc_threads()
    );
    let t0 = Instant::now();
    t.server.stop();
    assert!(t0.elapsed() < Duration::from_secs(3), "{:?}", t0.elapsed());
    assert_eq!(
        t.server.rtc_threads(),
        0,
        "no worker thread outlives stop()"
    );
    // the same with the STUN host names of the config resolved by the server's own thread (no internet needed)
    t.server.set_ice_servers(vec![IceServerConfig {
        url: "stun:localhost:3478".into(),
        username: String::new(),
        password: String::new(),
    }]);
    let bus = Arc::new(SharedMemory::open(&unique("rtcjoin2")).unwrap());
    assert!(t.server.start(bus, &t.listen, &t.send));
    let port = t.server.port();
    let mut w = open_ok(port, &format!("/ws/l/{}", t.listen));
    let _browser = datachannel(&mut w);
    assert_eq!(t.server.rtc_threads(), 1);
    let t0 = Instant::now();
    t.server.stop();
    assert!(t0.elapsed() < Duration::from_secs(3), "{:?}", t0.elapsed());
    assert_eq!(t.server.rtc_threads(), 0);
}
