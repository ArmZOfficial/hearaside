//! The REST API (R3.4): `ControlServer` over real loopback TCP and `rest::handle` against a fake Hub. Ports every check
//! of `ui-snapshot --test-rest` (tests/ui-snapshot main.cpp) and the C++ bugs fixed in Rust (`bug_*` tests). Every test
//! has its own server on an OS-picked port, so they run side by side.
use hs_bus::protocol::{ParamId, SourceParam};
use hs_net::control::{ControlServer, Request, Response};
use hs_net::json::Json;
use hs_net::rest::{HubControl, HubParam, HubState, RestService, SourceView, TrackView, handle};
use serde_json::{Value, json};
use std::io::{ErrorKind, Read, Write};
use std::net::{Ipv4Addr, TcpListener, TcpStream};
use std::time::{Duration, Instant};

const KEY: &str = "0123456789abcdef0123456789abcdef";

// ---- a fake Hub --------------------------------------------------------------------------------------------------

#[derive(Clone, Debug, PartialEq)]
enum Ev {
    Set(HubParam, f32),
    Send(i32, u32, f32),
    SendSource(i32, u32, f32),
    Sync(&'static str),
    Sharing(bool),
}

struct Fake {
    state: HubState,
    tracks: Vec<TrackView>,
    sources: Vec<SourceView>,
    events: Vec<Ev>,
    owner: bool,
    enabled: bool,
    key: String,
}

fn track(slot: i32, name: &str) -> TrackView {
    TrackView {
        slot,
        name: name.into(),
        active: true,
        ..TrackView::default()
    }
}

impl Fake {
    fn new() -> Fake {
        let mut vocal = track(1, "Vocal");
        vocal.you_hear = false;
        vocal.viewers_hear = true;
        Fake {
            state: HubState {
                peak_protection: true,
                loudness_lufs: -20.0,
                obs_connected: true,
                latency_to_obs_ms: Some(23.5),
                daw_buffer: 256,
                headphones_db: -6.0,
                ..HubState::default()
            },
            tracks: vec![track(0, "Music"), vocal, track(2, "Guitar")],
            sources: vec![SourceView {
                index: 0,
                name: "App Audio".into(),
                program: "brave.exe".into(),
                on: true,
                active: true,
                slot: 5,
                you_hear: true,
                viewers_hear: true,
                delay_ms: 120.0,
                ..SourceView::default()
            }],
            events: Vec::new(),
            owner: true,
            enabled: true,
            key: KEY.into(),
        }
    }

    /// The values written to `p`, in order.
    fn values(&self, p: HubParam) -> Vec<f32> {
        self.events
            .iter()
            .filter_map(|e| match e {
                Ev::Set(q, v) if *q == p => Some(*v),
                _ => None,
            })
            .collect()
    }

    fn sent(&self) -> Vec<(i32, u32, f32)> {
        self.events
            .iter()
            .filter_map(|e| match e {
                Ev::Send(s, id, v) => Some((*s, *id, *v)),
                _ => None,
            })
            .collect()
    }

    fn sent_source(&self) -> Vec<(i32, u32, f32)> {
        self.events
            .iter()
            .filter_map(|e| match e {
                Ev::SendSource(s, id, v) => Some((*s, *id, *v)),
                _ => None,
            })
            .collect()
    }
}

impl HubControl for Fake {
    fn version(&self) -> String {
        "1.2.3".into()
    }
    fn hub_state(&self) -> HubState {
        self.state.clone()
    }
    fn listen_link(&self) -> String {
        "https://my-hearaside.vercel.app/l/abc".into()
    }
    fn tracks(&self) -> Vec<TrackView> {
        self.tracks.clone()
    }
    fn sources(&self) -> Vec<SourceView> {
        self.sources.clone()
    }
    fn hub_range(&self, p: HubParam) -> (f32, f32) {
        match p {
            HubParam::Master => (-60.0, 12.0),
            HubParam::Headphones => (-60.0, 6.0),
            _ => (0.0, 1.0),
        }
    }
    fn track_range(&self, id: u32) -> (f32, f32) {
        match id {
            ParamId::StrGainDb => (-60.0, 12.0),
            ParamId::MonTrimDb => (-60.0, 6.0),
            ParamId::StrDelayMs => (0.0, 500.0),
            _ => (0.0, 1.0),
        }
    }
    fn set_param(&mut self, p: HubParam, plain: f32) {
        self.events.push(Ev::Set(p, plain));
        let on = plain > 0.5;
        match p {
            HubParam::Master => self.state.stream_db = plain,
            HubParam::Headphones => self.state.headphones_db = plain,
            HubParam::Panic => self.state.stream_muted = on,
            HubParam::Preview => self.state.hearing_viewers_mix = on,
            HubParam::LimiterOn => self.state.peak_protection = on,
        }
    }
    fn send(&mut self, slot: i32, id: u32, value: f32) {
        self.events.push(Ev::Send(slot, id, value));
    }
    fn send_source(&mut self, index: i32, id: u32, value: f32) {
        self.events.push(Ev::SendSource(index, id, value));
    }
    fn start_auto_sync(&mut self) {
        self.events.push(Ev::Sync("start"));
    }
    fn cancel_auto_sync(&mut self) {
        self.events.push(Ev::Sync("cancel"));
    }
    fn set_sharing(&mut self, on: bool) {
        self.events.push(Ev::Sharing(on));
        self.state.sharing = on;
    }
    fn is_bus_owner(&self) -> bool {
        self.owner
    }
    fn rest_api_enabled(&self) -> bool {
        self.enabled
    }
    fn rest_api_key(&self) -> String {
        self.key.clone()
    }
    fn set_rest_api_key(&mut self, key: &str) {
        self.key = key.into();
    }
}

// ---- the client side ---------------------------------------------------------------------------------------------

struct Reply {
    status: u16,
    head: String,
    body: String,
}

impl Reply {
    fn json(&self) -> Value {
        serde_json::from_str(&self.body).unwrap_or_else(|e| panic!("{e}: {:?}", self.body))
    }
    fn error(&self) -> String {
        self.json()["error"].as_str().unwrap_or_default().to_owned()
    }
}

fn parse_reply(out: &[u8]) -> Reply {
    let text = String::from_utf8_lossy(out).into_owned();
    let status = text
        .strip_prefix("HTTP/1.1 ")
        .and_then(|r| r.get(..3))
        .and_then(|s| s.parse().ok())
        .unwrap_or(0);
    let (head, body) = text.split_once("\r\n\r\n").unwrap_or((&text, ""));
    Reply {
        status,
        head: head.to_owned(),
        body: body.to_owned(),
    }
}

fn send_raw(port: u16, bytes: &[u8]) -> Reply {
    let mut s = TcpStream::connect((Ipv4Addr::LOCALHOST, port)).expect("connect");
    s.set_read_timeout(Some(Duration::from_secs(15))).unwrap();
    let _ = s.write_all(bytes);
    let mut out = Vec::new();
    let _ = s.read_to_end(&mut out);
    parse_reply(&out)
}

fn good_headers(port: u16) -> Vec<String> {
    vec![
        format!("Host: 127.0.0.1:{port}"),
        format!("Authorization: Bearer {KEY}"),
    ]
}

/// Content-Length is added unless `headers` has one.
fn call(port: u16, method: &str, path: &str, headers: &[String], body: &str) -> Reply {
    let mut req = format!("{method} {path} HTTP/1.1\r\n");
    for h in headers {
        req.push_str(h);
        req.push_str("\r\n");
    }
    if !headers
        .iter()
        .any(|h| h.to_ascii_lowercase().starts_with("content-length"))
    {
        req.push_str(&format!("Content-Length: {}\r\n", body.len()));
    }
    req.push_str("Connection: close\r\n\r\n");
    req.push_str(body);
    send_raw(port, req.as_bytes())
}

fn req(port: u16, method: &str, path: &str, body: &str) -> Reply {
    call(port, method, path, &good_headers(port), body)
}

fn with(port: u16, extra: &[&str]) -> Vec<String> {
    let mut h = good_headers(port);
    h.extend(extra.iter().map(|s| (*s).to_owned()));
    h
}

/// The server is up and asks for a key (no service thread needed for that).
fn answers(port: u16) -> bool {
    call(
        port,
        "GET",
        "/api/v1",
        &[format!("Host: 127.0.0.1:{port}")],
        "",
    )
    .status
        == 401
}

fn server() -> (ControlServer, u16) {
    let s = ControlServer::with_ports(&[0]);
    assert!(s.start(KEY));
    let port = s.port();
    (s, port)
}

/// The test thread is the "message thread": it services the queue while `client` runs on another thread.
fn drive<T: Send>(srv: &ControlServer, hub: &mut Fake, client: impl FnOnce() -> T + Send) -> T {
    std::thread::scope(|sc| {
        let h = sc.spawn(client);
        while !h.is_finished() {
            srv.service(|r| handle(hub, r));
            std::thread::sleep(Duration::from_millis(2));
        }
        h.join().expect("client thread")
    })
}

/// The server closed the connection: end of stream, or a reset. A timeout is "still open", not closed.
fn closed_by_peer(r: &std::io::Result<usize>) -> bool {
    match r {
        Ok(0) => true,
        Ok(_) => false,
        Err(e) => matches!(
            e.kind(),
            ErrorKind::ConnectionReset | ErrorKind::ConnectionAborted | ErrorKind::BrokenPipe
        ),
    }
}

fn wait_until(what: &str, mut f: impl FnMut() -> bool) {
    let start = Instant::now();
    while !f() {
        assert!(
            start.elapsed() < Duration::from_secs(10),
            "timed out: {what}"
        );
        std::thread::sleep(Duration::from_millis(5));
    }
}

// ---- ui-snapshot --test-rest, check by check ----------------------------------------------------------------------

#[test]
fn test_rest_every_check_of_the_c_plus_plus_test() {
    let (srv, port) = server();
    let mut hub = Fake::new();
    let host = format!("Host: 127.0.0.1:{port}");
    let key = format!("Authorization: Bearer {KEY}");
    let track_count = hub.tracks.len();
    drive(&srv, &mut hub, || {
        let r = call(
            port,
            "GET",
            "/api/v1/state",
            std::slice::from_ref(&host),
            "",
        );
        assert_eq!(r.status, 401, "no key -> 401");
        let wrong = "Authorization: Bearer wrongwrongwrongwrongwrongwrong00".to_owned();
        let r = call(port, "GET", "/api/v1/state", &[host.clone(), wrong], "");
        assert_eq!(r.status, 401, "wrong key -> 401");
        let r = call(
            port,
            "GET",
            "/api/v1/state",
            &[
                host.clone(),
                key.clone(),
                "Origin: https://evil.example".into(),
            ],
            "",
        );
        assert_eq!(r.status, 403, "a web page (Origin) -> 403");
        let r = call(
            port,
            "GET",
            "/api/v1/state",
            &[format!("Host: evil.example:{port}"), key.clone()],
            "",
        );
        assert_eq!(r.status, 403, "another host name (DNS rebinding) -> 403");
        let r = req(port, "GET", "/api/v1/state", "");
        assert_eq!(r.status, 200, "GET /api/v1/state -> 200");
        assert_eq!(r.json()["tracks"].as_array().unwrap().len(), track_count);
        let r = req(port, "POST", "/api/v1/mute", "{\"on\": true}");
        assert_eq!(r.status, 200);
        assert_eq!(
            r.json()["stream_muted"],
            json!(true),
            "mute {{on: true}} -> stream_muted"
        );
        let r = req(
            port,
            "POST",
            "/api/v1/tracks/Vocal",
            "{\"you_hear\": true, \"viewers_db\": -6}",
        );
        assert_eq!(r.status, 200, "POST /api/v1/tracks/<name> -> 200");
        let r = req(port, "POST", "/api/v1/tracks/nope", "{}");
        assert_eq!(r.status, 404, "unknown track -> 404");
        let r = req(port, "POST", "/api/v1/stream", "not json");
        assert_eq!(r.status, 400, "bad JSON -> 400");
    });
    // "the Hub parameter changed (host sees it)": one change gesture
    assert_eq!(hub.values(HubParam::Panic), [1.0]);
    assert!(hub.state.stream_muted);
    // "the Track's you hear / viewers level changed": commands through the mailbox of slot 1
    assert_eq!(
        hub.sent(),
        [(1, ParamId::Mon, 1.0), (1, ParamId::StrGainDb, -6.0)]
    );
    assert_eq!(hub.values(HubParam::Master), Vec::<f32>::new());
}

// ---- lifecycle ---------------------------------------------------------------------------------------------------

#[test]
fn start_stop_and_keys() {
    let (a, b) = (ControlServer::new_key(), ControlServer::new_key());
    assert_eq!(a.len(), 32);
    assert!(a.bytes().all(|c| matches!(c, b'0'..=b'9' | b'a'..=b'f')));
    assert_ne!(a, b);

    let s = ControlServer::with_ports(&[0]);
    assert!(!s.running() && s.port() == 0);
    assert!(!s.start("short"), "a key under 16 characters");
    assert!(!s.running());
    assert!(s.start(KEY));
    assert!(s.running() && s.port() != 0);
    assert_eq!(s.base_url(), format!("http://127.0.0.1:{}", s.port()));
    let port = s.port();
    assert!(answers(port));
    s.stop();
    s.stop(); // twice is fine
    assert!(!s.running() && s.port() == 0);
    wait_until("the port is closed", || {
        TcpStream::connect_timeout(
            &(Ipv4Addr::LOCALHOST, port).into(),
            Duration::from_millis(200),
        )
        .is_err()
    });
    // start again, on a free port
    assert!(s.start(KEY));
    assert!(s.running());
    // start while running = restart with the new key
    let key2 = "fedcba9876543210fedcba9876543210";
    assert!(s.start(key2));
    let p2 = s.port();
    let old = req(p2, "GET", "/api/v1", "");
    assert_eq!(old.status, 401, "the old key is gone");
    let new = call(
        p2,
        "GET",
        "/api/v1",
        &[
            format!("Host: 127.0.0.1:{p2}"),
            format!("Authorization: Bearer {key2}"),
        ],
        "",
    );
    // (the index is answered by the message thread: nobody services here, so 503 is the proof it got that far)
    assert_eq!(new.status, 503);
}

#[test]
fn default_port_range_is_47800_to_47809() {
    assert_eq!(hs_net::control::FIRST_PORT, 47800);
    assert_eq!(hs_net::control::PORT_COUNT, 10);
}

#[test]
fn port_fallback_when_the_first_is_taken() {
    let blocker = TcpListener::bind((Ipv4Addr::LOCALHOST, 0)).unwrap();
    let taken = blocker.local_addr().unwrap().port();
    let s = ControlServer::with_ports(&[taken, 0]);
    assert!(s.start(KEY));
    assert_ne!(s.port(), taken);
    assert!(answers(s.port()));
    s.stop();
    let only = ControlServer::with_ports(&[taken]);
    assert!(!only.start(KEY), "no free port");
    assert!(!only.running());
    drop(blocker);
    assert!(only.start(KEY), "free again");
    assert_eq!(only.port(), taken);
}

#[test]
fn listens_on_loopback_only() {
    let (srv, port) = server();
    // the listener is bound to 127.0.0.1: a connection to any other local address is refused
    for ip in own_non_loopback_ips() {
        assert!(
            TcpStream::connect_timeout(&(ip, port).into(), Duration::from_millis(500)).is_err(),
            "{ip} reached the REST API"
        );
    }
    drop(srv);
}

fn own_non_loopback_ips() -> Vec<Ipv4Addr> {
    // the computer's own addresses, found without a new crate: connect a UDP socket outward (no packet is sent)
    let mut v = Vec::new();
    if let Ok(s) = std::net::UdpSocket::bind((Ipv4Addr::UNSPECIFIED, 0))
        && s.connect((Ipv4Addr::new(192, 0, 2, 1), 9)).is_ok()
        && let Ok(a) = s.local_addr()
        && let std::net::IpAddr::V4(ip) = a.ip()
        && !ip.is_loopback()
        && !ip.is_unspecified()
    {
        v.push(ip);
    }
    v
}

// ---- who may call ------------------------------------------------------------------------------------------------

#[test]
fn rejects_missing_and_wrong_keys_with_the_c_plus_plus_text() {
    let (_srv, port) = server();
    let host = format!("Host: 127.0.0.1:{port}");
    let r = call(
        port,
        "GET",
        "/api/v1/state",
        std::slice::from_ref(&host),
        "",
    );
    assert_eq!(r.status, 401);
    assert_eq!(
        r.body,
        "{\"error\": \"missing or wrong API key (Hub settings > REST API)\"}\n"
    );
    for bad in [
        "Authorization: Bearer 0123456789abcdef0123456789abcde",
        "Authorization: Bearer 0123456789abcdef0123456789abcdeff",
        "Authorization: Bearer 0123456789abcdef0123456789abcdee",
        "Authorization: Bearer ",
        "Authorization:",
        "Authorization: 0123456789abcdef0123456789abcdef",
        "Authorization: Basic 0123456789abcdef0123456789abcdef",
        "Authorization: Basic x Bearer 0123456789abcdef0123456789abcdef", // C++ bug
        "Authorization: xBearer 0123456789abcdef0123456789abcdef",
        "Authorization: Bearer0123456789abcdef0123456789abcdef",
    ] {
        let r = call(
            port,
            "GET",
            "/api/v1/state",
            &[host.clone(), bad.into()],
            "",
        );
        assert_eq!(r.status, 401, "{bad}");
    }
    // two Authorization headers: not guessed which counts (C++ bug: the last one did)
    let r = call(
        port,
        "GET",
        "/api/v1/state",
        &[
            host.clone(),
            format!("Authorization: Bearer {KEY}"),
            format!("Authorization: Bearer {KEY}"),
        ],
        "",
    );
    assert_eq!(r.status, 401);
}

#[test]
fn bearer_scheme_is_any_case_and_spaces_are_trimmed() {
    let (srv, port) = server();
    let mut hub = Fake::new();
    drive(&srv, &mut hub, || {
        for auth in [
            format!("Authorization: bearer {KEY}"),
            format!("Authorization: BEARER   {KEY}  "),
            format!("authorization:Bearer {KEY}"),
        ] {
            let r = call(
                port,
                "GET",
                "/api/v1",
                &[format!("Host: 127.0.0.1:{port}"), auth.clone()],
                "",
            );
            assert_eq!(r.status, 200, "{auth}");
        }
    });
}

#[test]
fn bug_host_header_edge_cases() {
    let (srv, port) = server();
    let mut hub = Fake::new();
    let auth = format!("Authorization: Bearer {KEY}");
    drive(&srv, &mut hub, || {
        let with_host = |host: Option<&str>| {
            let mut h = vec![auth.clone()];
            if let Some(host) = host {
                h.push(format!("Host: {host}"));
            }
            call(port, "GET", "/api/v1", &h, "").status
        };
        assert_eq!(with_host(Some(&format!("127.0.0.1:{port}"))), 200);
        assert_eq!(with_host(Some(&format!("localhost:{port}"))), 200);
        assert_eq!(
            with_host(Some(&format!("LocalHost:{port}"))),
            200,
            "any case"
        );
        assert_eq!(with_host(Some(&format!("  127.0.0.1:{port}  "))), 200);
        assert_eq!(with_host(None), 403, "no Host");
        assert_eq!(with_host(Some("")), 403);
        assert_eq!(with_host(Some("127.0.0.1")), 403, "no port");
        assert_eq!(with_host(Some("localhost")), 403);
        assert_eq!(
            with_host(Some(&format!("127.0.0.1:{}", port.wrapping_add(1)))),
            403,
            "other port"
        );
        assert_eq!(
            with_host(Some(&format!("[::1]:{port}"))),
            403,
            "IPv6 loopback is not where we listen"
        );
        assert_eq!(with_host(Some(&format!("localhost.:{port}"))), 403);
        assert_eq!(with_host(Some(&format!("evil.example:{port}"))), 403);
        assert_eq!(
            with_host(Some(&format!("127.0.0.1:{port}.evil.example"))),
            403
        );
        assert_eq!(
            with_host(Some(&format!("127.0.0.1:{port}@evil.example"))),
            403
        );
        // two Host headers: the last one counted in the C++, so a forged first one went unnoticed
        let two = |a: String, b: String| {
            call(
                port,
                "GET",
                "/api/v1",
                &[auth.clone(), format!("Host: {a}"), format!("Host: {b}")],
                "",
            )
            .status
        };
        assert_eq!(
            two(format!("evil.example:{port}"), format!("127.0.0.1:{port}")),
            403
        );
        assert_eq!(
            two(format!("127.0.0.1:{port}"), format!("evil.example:{port}")),
            403
        );
        assert_eq!(
            two(format!("127.0.0.1:{port}"), format!("127.0.0.1:{port}")),
            403
        );
    });
}

#[test]
fn bug_origin_and_preflight_are_always_rejected() {
    let (srv, port) = server();
    let mut hub = Fake::new();
    drive(&srv, &mut hub, || {
        // any Origin, also the Hub's own and an empty one (C++ bug: an empty value passed)
        for origin in [
            "Origin: https://evil.example",
            &format!("Origin: http://127.0.0.1:{port}"),
            "Origin: null",
            "Origin:",
            "origin:   ",
        ] {
            let r = call(port, "GET", "/api/v1/state", &with(port, &[origin]), "");
            assert_eq!(r.status, 403, "{origin}");
            assert_eq!(
                r.error(),
                "only programs on this computer may use the HEARASIDE API"
            );
            assert!(
                !r.head.to_lowercase().contains("access-control"),
                "no CORS headers"
            );
        }
        // the CORS preflight a browser sends first (also in lower case: C++ bug)
        for method in ["OPTIONS", "options", "Options"] {
            let r = call(
                port,
                method,
                "/api/v1/mute",
                &with(
                    port,
                    &[
                        "Origin: https://evil.example",
                        "Access-Control-Request-Method: POST",
                    ],
                ),
                "",
            );
            assert_eq!(r.status, 403, "{method}");
            let r = call(port, method, "/api/v1/mute", &good_headers(port), "");
            assert_eq!(r.status, 403, "{method} without Origin");
        }
    });
    assert!(hub.events.is_empty(), "nothing reached the Hub");
}

#[test]
fn the_403_comes_before_the_401() {
    let (_srv, port) = server();
    let r = call(
        port,
        "GET",
        "/api/v1/state",
        &["Host: evil.example".into()],
        "",
    );
    assert_eq!(r.status, 403);
}

// ---- request limits ----------------------------------------------------------------------------------------------

#[test]
fn bug_content_length_is_strict() {
    let (srv, port) = server();
    let mut hub = Fake::new();
    drive(&srv, &mut hub, || {
        let post = |len: &[&str], body: &str, extra: &[&str]| {
            let mut h = good_headers(port);
            h.extend(len.iter().map(|l| format!("Content-Length: {l}")));
            h.extend(extra.iter().map(|s| (*s).to_owned()));
            call(port, "POST", "/api/v1/mute", &h, body)
        };
        assert_eq!(post(&["2"], "{}", &[]).status, 200);
        assert_eq!(post(&["0"], "", &[]).status, 200);
        assert_eq!(post(&["16385"], "", &[]).status, 413);
        assert_eq!(
            post(&["99999999999999999999999999"], "", &[]).status,
            413,
            "no wrapping"
        );
        assert_eq!(post(&["4294967297"], "", &[]).status, 413);
        for bad in ["-1", "-0", "+2", "abc", "12abc", "", "1 2", "0x2", "2.0"] {
            let r = post(&[bad], "{}", &[]);
            assert_eq!(r.status, 400, "Content-Length: {bad:?}");
            assert_eq!(r.error(), "bad Content-Length");
        }
        // repeated: the same is fine, different is not (C++ bug: the last one counted)
        assert_eq!(post(&["2", "2"], "{}", &[]).status, 200);
        assert_eq!(post(&["2", "9"], "{}", &[]).status, 400);
        assert_eq!(post(&["0", "2"], "{}", &[]).status, 400);
        // no chunked bodies
        assert_eq!(
            post(&["2"], "{}", &["Transfer-Encoding: chunked"]).status,
            400
        );
        // exactly Content-Length bytes are the body (C++ bug: whatever followed was)
        assert_eq!(post(&["2"], "{}garbage", &[]).status, 200);
        assert_eq!(post(&["2"], "{}{}", &[]).status, 200);
        // C++ bug: bytes without any Content-Length were the body; now there is no body (a toggle)
        let r = call(port, "POST", "/api/v1/mute", &good_headers(port), "");
        assert_eq!(r.status, 200);
    });
    // 6 successful POST /mute calls: each one toggled
    assert_eq!(hub.values(HubParam::Panic).len(), 6);
}

#[test]
fn bug_content_length_without_the_header_means_no_body() {
    let (srv, port) = server();
    let mut hub = Fake::new();
    drive(&srv, &mut hub, || {
        // headers + a body but no Content-Length: the body is not read (it would have been `{"on": false}`)
        let raw = format!(
            "POST /api/v1/mute HTTP/1.1\r\nHost: 127.0.0.1:{port}\r\nAuthorization: Bearer {KEY}\r\n\r\n{{\"on\": false}}"
        );
        let r = send_raw(port, raw.as_bytes());
        assert_eq!(r.status, 200);
        assert_eq!(
            r.json()["stream_muted"],
            json!(true),
            "toggled from off, not set to false"
        );
    });
}

#[test]
fn large_and_malformed_requests() {
    let (_srv, port) = server();
    // a head over 8 KiB (C++ bug: closed without an answer)
    let r = call(
        port,
        "GET",
        "/api/v1/state",
        &with(port, &[&format!("X-Pad: {}", "a".repeat(9000))]),
        "",
    );
    assert_eq!(r.status, 431);
    // not even a request line
    let r = send_raw(port, b"HELLO\r\n\r\n");
    assert_eq!(r.status, 400);
    assert_eq!(r.error(), "bad request");
    // a head that never ends is closed without an answer
    let r = send_raw(port, &vec![b'a'; 20000]);
    assert_eq!(r.status, 431);
}

#[test]
fn json_bodies() {
    let (srv, port) = server();
    let mut hub = Fake::new();
    drive(&srv, &mut hub, || {
        let bad = |body: &str| req(port, "POST", "/api/v1/mute", body);
        for body in [
            "not json",
            "{",
            "{\"on\": }",
            "5",
            "\"x\"",
            "null",
            "true",
            "{} {}",
            "\u{feff}{}",
        ] {
            let r = bad(body);
            assert_eq!(r.status, 400, "{body:?}");
            assert_eq!(r.error(), "the body is not JSON");
        }
        // an array is JSON but not what the routes want
        let r = bad("[1]");
        assert_eq!(r.status, 400);
        assert_eq!(r.error(), "the body must be a JSON object");
        // a body that is not JSON is refused on GET too
        assert_eq!(req(port, "GET", "/api/v1/state", "nope").status, 400);
        // ... and an array is ignored there
        assert_eq!(req(port, "GET", "/api/v1/state", "[1]").status, 200);
        // no body, an empty body and a body of white space all mean "no body"
        assert_eq!(bad("").status, 200);
        assert_eq!(bad(" \r\n").status, 200);
        // not UTF-8
        let raw = format!(
            "POST /api/v1/mute HTTP/1.1\r\nHost: 127.0.0.1:{port}\r\nAuthorization: Bearer {KEY}\r\nContent-Length: 3\r\n\r\n{{\"\u{ff}"
        );
        let mut bytes = raw.into_bytes();
        let n = bytes.len();
        bytes[n - 2] = 0xC3;
        bytes[n - 1] = 0x28;
        assert_eq!(send_raw(port, &bytes).status, 400);
    });
}

// ---- slow clients ------------------------------------------------------------------------------------------------

/// Sends `bytes` one at a time every 80 ms and returns when the server closes the connection (None = still open).
fn trickle(port: u16, bytes: &[u8], limit: Duration) -> Option<Duration> {
    let mut s = TcpStream::connect((Ipv4Addr::LOCALHOST, port)).unwrap();
    s.set_read_timeout(Some(Duration::from_millis(30))).unwrap();
    let start = Instant::now();
    for b in bytes.iter().cycle() {
        if start.elapsed() > limit {
            return None;
        }
        if s.write_all(&[*b]).is_err() {
            return Some(start.elapsed());
        }
        let mut buf = [0u8; 64];
        match s.read(&mut buf) {
            Ok(0) => return Some(start.elapsed()),
            Ok(_) => {}
            Err(e) if matches!(e.kind(), ErrorKind::WouldBlock | ErrorKind::TimedOut) => {}
            Err(_) => return Some(start.elapsed()),
        }
        std::thread::sleep(Duration::from_millis(50));
    }
    None
}

#[test]
fn bug_a_slow_head_or_body_does_not_keep_a_place() {
    let s = ControlServer::with_ports(&[0])
        .with_timeouts(Duration::from_secs(2), Duration::from_millis(500));
    assert!(s.start(KEY));
    let port = s.port();
    // one byte every 80 ms never completes the head: the C++ waited another 3 s after every byte
    let closed = trickle(
        port,
        b"GET /api/v1/state HTTP/1.1\r\nX-Slow: aaaaaaaaaaaaaaaaaaaaaaaa",
        Duration::from_secs(4),
    );
    let t = closed.expect("closed");
    assert!(t < Duration::from_millis(2500), "{t:?}");
    // nothing at all
    let mut idle = TcpStream::connect((Ipv4Addr::LOCALHOST, port)).unwrap();
    idle.set_read_timeout(Some(Duration::from_secs(5))).unwrap();
    let start = Instant::now();
    let mut buf = [0u8; 8];
    assert!(matches!(idle.read(&mut buf), Ok(0) | Err(_)));
    assert!(start.elapsed() < Duration::from_secs(3));
    // a body that arrives too slowly
    let head = format!(
        "POST /api/v1/mute HTTP/1.1\r\nHost: 127.0.0.1:{port}\r\nAuthorization: Bearer {KEY}\r\nContent-Length: 400\r\n\r\n"
    );
    let mut c = TcpStream::connect((Ipv4Addr::LOCALHOST, port)).unwrap();
    c.set_read_timeout(Some(Duration::from_millis(30))).unwrap();
    c.write_all(head.as_bytes()).unwrap();
    let start = Instant::now();
    let mut closed_at = None;
    while start.elapsed() < Duration::from_secs(4) {
        if c.write_all(b" ").is_err() {
            closed_at = Some(start.elapsed());
            break;
        }
        let mut buf = [0u8; 8];
        match c.read(&mut buf) {
            Ok(0) => {
                closed_at = Some(start.elapsed());
                break;
            }
            Err(e) if matches!(e.kind(), ErrorKind::WouldBlock | ErrorKind::TimedOut) => {}
            Err(_) => {
                closed_at = Some(start.elapsed());
                break;
            }
            Ok(_) => {}
        }
        std::thread::sleep(Duration::from_millis(50));
    }
    assert!(
        closed_at.is_some_and(|t| t < Duration::from_millis(2500)),
        "{closed_at:?}"
    );
}

#[test]
fn at_most_eight_connections_at_once() {
    let (srv, port) = server();
    let mut hub = Fake::new();
    let idle: Vec<TcpStream> = (0..8)
        .map(|_| TcpStream::connect((Ipv4Addr::LOCALHOST, port)).unwrap())
        .collect();
    std::thread::sleep(Duration::from_millis(300)); // the server has them all
    let mut ninth = TcpStream::connect((Ipv4Addr::LOCALHOST, port)).unwrap();
    ninth
        .set_read_timeout(Some(Duration::from_secs(3)))
        .unwrap();
    let mut buf = [0u8; 16];
    let t = Instant::now();
    assert!(
        closed_by_peer(&ninth.read(&mut buf)),
        "the ninth is closed, not left waiting"
    );
    assert!(t.elapsed() < Duration::from_secs(2), "{:?}", t.elapsed());
    drop(idle);
    wait_until("places free again", || {
        drive(&srv, &mut hub, || {
            req(port, "GET", "/api/v1", "").status == 200
        })
    });
}

// ---- routing -----------------------------------------------------------------------------------------------------

#[test]
fn routes_and_status_codes() {
    let (srv, port) = server();
    let mut hub = Fake::new();
    drive(&srv, &mut hub, || {
        let status = |m: &str, p: &str, b: &str| req(port, m, p, b).status;
        let msg = |m: &str, p: &str, b: &str| req(port, m, p, b).error();
        assert_eq!(status("GET", "/api/v1", ""), 200);
        assert_eq!(status("GET", "/api/v1/", ""), 200);
        assert_eq!(status("GET", "/api/v1//", ""), 200);
        assert_eq!(status("GET", "/api/v1/state?x=1&y=2", ""), 200);
        assert_eq!(
            status("get", "/api/v1/state", ""),
            200,
            "the method in lower case"
        );
        assert_eq!(status("GET", "/api/v1/tracks", ""), 200);
        assert_eq!(status("GET", "/api/v1/sources", ""), 200);
        assert_eq!(status("GET", "/nope", ""), 404);
        assert_eq!(msg("GET", "/nope", ""), "unknown path (try GET /api/v1)");
        assert_eq!(status("GET", "/", ""), 404);
        assert_eq!(status("GET", "/api", ""), 404);
        assert_eq!(status("GET", "/api/v2/state", ""), 404);
        assert_eq!(status("GET", "http://127.0.0.1/api/v1/state", ""), 404);
        // C++ bug: a GET of an unknown path was "405 POST only"
        assert_eq!(status("GET", "/api/v1/nope", ""), 404);
        assert_eq!(status("POST", "/api/v1/nope", "{}"), 404);
        // C++ bug: only the start of the path was checked
        assert_eq!(status("GET", "/api/v1state", ""), 404);
        assert_eq!(status("GET", "/api/v1x", ""), 404);
        assert_eq!(status("GET", "/api/v1/state/more", ""), 404);
        assert_eq!(status("POST", "/api/v1/mute/more", "{}"), 404);
        assert_eq!(status("POST", "/api/v1/tracks/Vocal/more", "{}"), 404);
        // known paths, wrong method
        assert_eq!(msg("POST", "/api/v1/state", "{}"), "GET only");
        assert_eq!(status("POST", "/api/v1", "{}"), 405);
        // as the C++: a POST to the list paths is an unknown path
        assert_eq!(status("POST", "/api/v1/tracks", "{}"), 404);
        assert_eq!(
            msg("POST", "/api/v1/sources", "{}"),
            "unknown path (try GET /api/v1)"
        );
        assert_eq!(
            msg("POST", "/api/v1/tracks", "[1]"),
            "the body must be a JSON object"
        );
        assert_eq!(msg("DELETE", "/api/v1/tracks", ""), "POST only");
        assert_eq!(msg("PUT", "/api/v1/sources", "{}"), "POST only");
        assert_eq!(msg("DELETE", "/api/v1", ""), "GET only");
        assert_eq!(status("PUT", "/api/v1/nope", "{}"), 404);
        assert_eq!(msg("GET", "/api/v1/mute", ""), "POST only");
        assert_eq!(status("GET", "/api/v1/tracks/Vocal", ""), 405);
        assert_eq!(status("PUT", "/api/v1/mute", "{}"), 405);
        assert_eq!(status("DELETE", "/api/v1/state", ""), 405);
        assert_eq!(status("PATCH", "/api/v1/tracks/Vocal", "{}"), 405);
        // the body of a POST must be an object, whatever the path (a path that exists)
        assert_eq!(
            msg("POST", "/api/v1/share", "[true]"),
            "the body must be a JSON object"
        );
    });
    assert!(hub.events.is_empty());
}

#[test]
fn index_text_is_the_c_plus_plus_one() {
    let (srv, port) = server();
    let mut hub = Fake::new();
    let r = drive(&srv, &mut hub, || req(port, "GET", "/api/v1", ""));
    assert_eq!(r.status, 200);
    assert!(
        r.head
            .contains("Content-Type: application/json; charset=utf-8")
    );
    assert!(r.head.contains("Cache-Control: no-store"));
    assert!(r.head.contains("Connection: close"));
    assert!(r.head.contains("X-Content-Type-Options: nosniff"));
    assert!(
        r.head
            .contains(&format!("Content-Length: {}", r.body.len()))
    );
    assert!(r.head.starts_with("HTTP/1.1 200 OK\r\n"));
    assert_eq!(
        r.body,
        "{\"name\": \"HEARASIDE Hub\", \"version\": \"1.2.3\", \"docs\": \"docs/rest-api.md\", \"endpoints\": [\"GET  /api/v1/state\", \"POST /api/v1/mute {on?}\", \"POST /api/v1/preview {on?}\", \"POST /api/v1/limiter {on?}\", \"POST /api/v1/stream {db}\", \"POST /api/v1/headphones {db}\", \"GET  /api/v1/tracks\", \"POST /api/v1/tracks/<slot|name> {you_hear?, viewers_hear?, headphone_db?, viewers_db?, delay_ms?, solo?}\", \"GET  /api/v1/sources\", \"POST /api/v1/sources/<index|name> {on?, you_hear?, viewers_hear?, headphone_db?, viewers_db?, record?}\", \"POST /api/v1/sync {action: start|cancel}\", \"POST /api/v1/share {on?}\"]}\n"
    );
}

// ---- JSON of the state, byte for byte -----------------------------------------------------------------------------

#[test]
fn state_json_is_the_juce_text() {
    let (srv, port) = server();
    let mut hub = Fake::new();
    hub.state.sharing = true;
    hub.tracks[0].you_hear = true;
    hub.tracks[0].viewers_db = -0.1; // a float: shows its binary value, as JUCE does
    hub.tracks[0].delay_ms = 120.0;
    hub.tracks[1].headphone_db = -6.0;
    hub.tracks[1].solo = true;
    hub.tracks.truncate(2);
    hub.tracks[1].name = "Vox \"ก\"".into();
    let r = drive(&srv, &mut hub, || req(port, "GET", "/api/v1/state", ""));
    assert_eq!(r.status, 200);
    assert_eq!(
        r.body,
        "{\"hub\": {\"stream_muted\": false, \"hearing_viewers_mix\": false, \"stream_db\": 0.0, \"headphones_db\": -6.0, \"peak_protection\": true, \"loudness_lufs\": -20.0, \"obs_connected\": true, \"viewers_silent\": false, \"latency_to_obs_ms\": 23.5, \"daw_buffer\": 256, \"sharing\": true, \"listen_link\": \"https://my-hearaside.vercel.app/l/abc\"}, \"tracks\": [{\"slot\": 0, \"name\": \"Music\", \"active\": true, \"you_hear\": true, \"viewers_hear\": false, \"headphone_db\": 0.0, \"viewers_db\": -0.100000001490116, \"delay_ms\": 120.0, \"solo\": false, \"bypassed\": false}, {\"slot\": 1, \"name\": \"Vox \\\"ก\\\"\", \"active\": true, \"you_hear\": false, \"viewers_hear\": true, \"headphone_db\": -6.0, \"viewers_db\": 0.0, \"delay_ms\": 0.0, \"solo\": true, \"bypassed\": false}], \"sources\": [{\"index\": 0, \"name\": \"App Audio\", \"program\": \"brave.exe\", \"on\": true, \"active\": true, \"you_hear\": true, \"viewers_hear\": true, \"headphone_db\": 0.0, \"viewers_db\": 0.0, \"delay_ms\": 120.0, \"recording\": false}]}\n"
    );
    // the same members, in the same order, in the lists
    let t = drive(&srv, &mut hub, || req(port, "GET", "/api/v1/tracks", ""));
    assert!(t.body.starts_with("[{\"slot\": 0, \"name\": \"Music\""));
    assert!(t.body.ends_with("\"bypassed\": false}]\n"));
    let s = drive(&srv, &mut hub, || req(port, "GET", "/api/v1/sources", ""));
    assert!(
        s.body
            .starts_with("[{\"index\": 0, \"name\": \"App Audio\", \"program\": \"brave.exe\"")
    );
    hub.tracks.clear();
    hub.sources.clear();
    let e = drive(&srv, &mut hub, || req(port, "GET", "/api/v1/tracks", ""));
    assert_eq!(e.body, "[]\n");
}

#[test]
fn loudness_latency_and_link_in_the_hub_object() {
    let (srv, port) = server();
    let mut hub = Fake::new();
    let get = |srv: &ControlServer, hub: &mut Fake| {
        drive(srv, hub, || req(port, "GET", "/api/v1/state", "")).json()["hub"].clone()
    };
    let h = get(&srv, &mut hub);
    assert_eq!(h["listen_link"], json!(""), "not sharing = no link");
    hub.state.loudness_lufs = -70.5;
    assert_eq!(
        get(&srv, &mut hub)["loudness_lufs"],
        Value::Null,
        "below -70 = null"
    );
    hub.state.loudness_lufs = -100.0;
    assert_eq!(get(&srv, &mut hub)["loudness_lufs"], Value::Null);
    hub.state.loudness_lufs = -69.94;
    assert_eq!(get(&srv, &mut hub)["loudness_lufs"], json!(-69.9));
    hub.state.loudness_lufs = f32::NAN;
    assert_eq!(
        get(&srv, &mut hub)["loudness_lufs"],
        Value::Null,
        "not a number = null"
    );
    hub.state.latency_to_obs_ms = None;
    hub.state.obs_connected = false;
    let h = get(&srv, &mut hub);
    assert_eq!(h["latency_to_obs_ms"], Value::Null);
    assert_eq!(h["obs_connected"], json!(false));
    // the rounding to 0.1 is on the double: -16.24 is -16.2
    hub.state.loudness_lufs = -16.24;
    let r = drive(&srv, &mut hub, || req(port, "GET", "/api/v1/state", ""));
    assert!(
        r.body.contains("\"loudness_lufs\": -16.199999999999999,"),
        "{}",
        r.body
    );
}

// ---- Hub switches and levels -------------------------------------------------------------------------------------

#[test]
fn switches_set_toggle_and_go_through_one_gesture() {
    let (srv, port) = server();
    let mut hub = Fake::new();
    drive(&srv, &mut hub, || {
        // absent `on` = toggle
        let r = req(port, "POST", "/api/v1/mute", "");
        assert_eq!(r.json()["stream_muted"], json!(true));
        let r = req(port, "POST", "/api/v1/mute", "{}");
        assert_eq!(r.json()["stream_muted"], json!(false));
        // null = toggle
        let r = req(port, "POST", "/api/v1/mute", "{\"on\": null}");
        assert_eq!(r.json()["stream_muted"], json!(true));
        // explicit: setting what already is changes nothing visible but is still one gesture
        let r = req(port, "POST", "/api/v1/mute", "{\"on\": true}");
        assert_eq!(r.json()["stream_muted"], json!(true));
        let r = req(port, "POST", "/api/v1/mute", "{\"on\": false}");
        assert_eq!(r.json()["stream_muted"], json!(false));
        // numbers: 0 is off, anything else on
        assert_eq!(
            req(port, "POST", "/api/v1/mute", "{\"on\": 1}").json()["stream_muted"],
            json!(true)
        );
        assert_eq!(
            req(port, "POST", "/api/v1/mute", "{\"on\": 0}").json()["stream_muted"],
            json!(false)
        );
        assert_eq!(
            req(port, "POST", "/api/v1/mute", "{\"on\": 0.5}").json()["stream_muted"],
            json!(true)
        );
        assert_eq!(
            req(port, "POST", "/api/v1/mute", "{\"on\": 5000000000}").json()["stream_muted"],
            json!(true)
        );
        // the other two switches
        assert_eq!(
            req(port, "POST", "/api/v1/preview", "{\"on\": true}").json()["hearing_viewers_mix"],
            json!(true)
        );
        assert_eq!(
            req(port, "POST", "/api/v1/preview", "{}").json()["hearing_viewers_mix"],
            json!(false)
        );
        assert_eq!(
            req(port, "POST", "/api/v1/limiter", "{}").json()["peak_protection"],
            json!(false)
        );
        assert_eq!(
            req(port, "POST", "/api/v1/limiter", "{\"on\": true}").json()["peak_protection"],
            json!(true)
        );
        // an extra member is ignored
        assert_eq!(
            req(port, "POST", "/api/v1/mute", "{\"on\": false, \"x\": 1}").status,
            200
        );
    });
    assert_eq!(
        hub.values(HubParam::Panic),
        [1.0, 0.0, 1.0, 1.0, 0.0, 1.0, 0.0, 1.0, 1.0, 0.0]
    );
    assert_eq!(hub.values(HubParam::Preview), [1.0, 0.0]);
    assert_eq!(hub.values(HubParam::LimiterOn), [0.0, 1.0]);
}

#[test]
fn boolean_members_follow_the_c_plus_plus_toggle_rules() {
    let (srv, port) = server();
    let mut hub = Fake::new(); // not muted; Vocal: you_hear false, solo false; App Audio not recording; not sharing
    drive(&srv, &mut hub, || {
        // "true" / "false" set that value (C++ bug: they flipped, so "false" on a muted stream un-muted it) ...
        for body in [
            "{\"on\": \"false\"}",
            "{\"on\": \"true\"}",
            "{\"on\": \" TRUE \"}",
            // ... anything else that is not a boolean or a number flips, as in the C++
            "{\"on\": \"toggle\"}",
            "{\"on\": \"x\"}",
            "{\"on\": []}",
            "{\"on\": {}}",
            "{\"on\": \"\"}",
            "{\"on\": \"true\"}",
            "{\"on\": \"False\"}",
        ] {
            assert_eq!(
                req(port, "POST", "/api/v1/mute", body).status,
                200,
                "{body}"
            );
        }
        let post = |path: &str, body: &str| {
            let r = req(port, "POST", path, body);
            assert_eq!(r.status, 200, "{body}");
        };
        post("/api/v1/tracks/Vocal", "{\"you_hear\": \"yes\"}");
        post("/api/v1/tracks/Vocal", "{\"solo\": [true]}");
        // numbers that are not numbers for dB fields are ignored, like the C++ (the other members still apply)
        post("/api/v1/tracks/Vocal", "{\"headphone_db\": \"loud\"}");
        post("/api/v1/tracks/Vocal", "{\"delay_ms\": true}");
        post(
            "/api/v1/tracks/Vocal",
            "{\"you_hear\": true, \"viewers_hear\": false, \"headphone_db\": -3, \"viewers_db\": \"x\"}",
        );
        post("/api/v1/sources/0", "{\"on\": false, \"record\": \"x\"}");
        post("/api/v1/share", "{\"on\": \"x\"}");
        // the Hub levels need a number
        let r = req(port, "POST", "/api/v1/stream", "{\"db\": \"3\"}");
        assert_eq!(r.status, 400);
        assert_eq!(r.error(), "needs {\"db\": number}");
    });
    // not muted at the start: false, true, true, then flips from true: false, true, false, true, false; then
    // true, false
    assert_eq!(
        hub.values(HubParam::Panic),
        [0.0, 1.0, 1.0, 0.0, 1.0, 0.0, 1.0, 0.0, 1.0, 0.0]
    );
    assert_eq!(
        hub.sent(),
        [
            (1, ParamId::Mon, 1.0),
            (1, ParamId::StrSolo, 1.0),
            (1, ParamId::Mon, 1.0),
            (1, ParamId::Str, 0.0),
            (1, ParamId::MonTrimDb, -3.0),
        ]
    );
    assert_eq!(
        hub.sent_source(),
        [(0, SourceParam::On, 0.0), (0, SourceParam::Record, 1.0)]
    );
    assert!(hub.events.contains(&Ev::Sharing(true)));
}

#[test]
fn stream_and_headphones_levels_are_clamped_to_the_parameter() {
    let (srv, port) = server();
    let mut hub = Fake::new();
    drive(&srv, &mut hub, || {
        let post = |p: &str, b: &str| req(port, "POST", p, b);
        let r = post("/api/v1/stream", "{\"db\": -3}");
        assert_eq!(r.status, 200);
        assert_eq!(r.json()["stream_db"], json!(-3.0));
        assert_eq!(
            post("/api/v1/stream", "{\"db\": 99}").json()["stream_db"],
            json!(12.0)
        );
        assert_eq!(
            post("/api/v1/stream", "{\"db\": -999}").json()["stream_db"],
            json!(-60.0)
        );
        assert_eq!(
            post("/api/v1/stream", "{\"db\": 1e300}").json()["stream_db"],
            json!(12.0),
            "C++ bug: inf"
        );
        assert_eq!(
            post("/api/v1/stream", "{\"db\": -1e300}").json()["stream_db"],
            json!(-60.0)
        );
        assert_eq!(
            post("/api/v1/stream", "{\"db\": 2.5}").json()["stream_db"],
            json!(2.5)
        );
        assert_eq!(
            post("/api/v1/headphones", "{\"db\": -6.5}").json()["headphones_db"],
            json!(-6.5)
        );
        assert_eq!(
            post("/api/v1/headphones", "{\"db\": 99}").json()["headphones_db"],
            json!(6.0)
        );
        assert_eq!(
            post("/api/v1/headphones", "{\"db\": -99}").json()["headphones_db"],
            json!(-60.0)
        );
        for bad in [
            "",
            "{}",
            "{\"db\": null}",
            "{\"db\": \"3\"}",
            "{\"db\": true}",
            "{\"db\": [1]}",
            "{\"dB\": 1}",
        ] {
            let r = post("/api/v1/stream", bad);
            assert_eq!(r.status, 400, "{bad}");
            assert_eq!(r.error(), "needs {\"db\": number}");
        }
        // JSON cannot say NaN / Infinity: those are not JSON
        for bad in [
            "{\"db\": NaN}",
            "{\"db\": Infinity}",
            "{\"db\": -Infinity}",
            "{\"db\": 1e999}",
        ] {
            assert_eq!(post("/api/v1/headphones", bad).status, 400, "{bad}");
        }
    });
    assert_eq!(
        hub.values(HubParam::Master),
        [-3.0, 12.0, -60.0, 12.0, -60.0, 2.5]
    );
    assert_eq!(hub.values(HubParam::Headphones), [-6.5, 6.0, -60.0]);
}

// ---- tracks ------------------------------------------------------------------------------------------------------

#[test]
fn track_commands_go_to_the_track_mailbox() {
    let (srv, port) = server();
    let mut hub = Fake::new();
    let r = drive(&srv, &mut hub, || {
        req(
            port,
            "POST",
            "/api/v1/tracks/Vocal",
            "{\"you_hear\": true, \"viewers_hear\": false, \"headphone_db\": -6, \"viewers_db\": 3.5, \"delay_ms\": 40, \"solo\": true}",
        )
    });
    assert_eq!(r.status, 200);
    assert_eq!(r.body, "{\"ok\": true, \"slot\": 1, \"name\": \"Vocal\"}\n");
    assert_eq!(
        hub.sent(),
        [
            (1, ParamId::Mon, 1.0),
            (1, ParamId::Str, 0.0),
            (1, ParamId::MonTrimDb, -6.0),
            (1, ParamId::StrGainDb, 3.5),
            (1, ParamId::StrDelayMs, 40.0),
            (1, ParamId::StrSolo, 1.0),
        ]
    );
    assert!(
        hub.events.iter().all(|e| matches!(e, Ev::Send(..))),
        "no Hub parameter was touched"
    );
}

#[test]
fn track_toggles_use_the_current_value() {
    let (srv, port) = server();
    let mut hub = Fake::new(); // Vocal: you_hear false, viewers_hear true, solo false
    drive(&srv, &mut hub, || {
        let r = req(
            port,
            "POST",
            "/api/v1/tracks/1",
            "{\"you_hear\": null, \"viewers_hear\": null, \"solo\": null}",
        );
        assert_eq!(r.status, 200);
        // an empty body changes nothing but is still a valid request
        assert_eq!(req(port, "POST", "/api/v1/tracks/1", "").status, 200);
        assert_eq!(req(port, "POST", "/api/v1/tracks/1", "{}").status, 200);
    });
    assert_eq!(
        hub.sent(),
        [
            (1, ParamId::Mon, 1.0),
            (1, ParamId::Str, 0.0),
            (1, ParamId::StrSolo, 1.0)
        ]
    );
}

#[test]
fn bug_db_values_use_the_parameter_ranges() {
    let (srv, port) = server();
    let mut hub = Fake::new();
    drive(&srv, &mut hub, || {
        let r = req(
            port,
            "POST",
            "/api/v1/tracks/0",
            "{\"headphone_db\": 99, \"viewers_db\": 99, \"delay_ms\": 99999}",
        );
        assert_eq!(r.status, 200);
        let r = req(
            port,
            "POST",
            "/api/v1/tracks/0",
            "{\"headphone_db\": -99, \"viewers_db\": -99, \"delay_ms\": -5}",
        );
        assert_eq!(r.status, 200);
        let r = req(
            port,
            "POST",
            "/api/v1/tracks/0",
            "{\"headphone_db\": 1e300, \"viewers_db\": -1e300, \"delay_ms\": 1e300}",
        );
        assert_eq!(r.status, 200);
    });
    assert_eq!(
        hub.sent(),
        [
            // viewers_db goes to +12 like the Track's own parameter (the C++ stopped at +6)
            (0, ParamId::MonTrimDb, 6.0),
            (0, ParamId::StrGainDb, 12.0),
            (0, ParamId::StrDelayMs, 500.0),
            (0, ParamId::MonTrimDb, -60.0),
            (0, ParamId::StrGainDb, -60.0),
            (0, ParamId::StrDelayMs, 0.0),
            (0, ParamId::MonTrimDb, 6.0),
            (0, ParamId::StrGainDb, -60.0),
            (0, ParamId::StrDelayMs, 500.0),
        ]
    );
}

#[test]
fn tracks_are_found_by_slot_or_name() {
    let (srv, port) = server();
    let mut hub = Fake::new();
    drive(&srv, &mut hub, || {
        let find = |key: &str| {
            let r = req(port, "POST", &format!("/api/v1/tracks/{key}"), "{}");
            (r.status, r.json())
        };
        assert_eq!(find("2").1["name"], json!("Guitar"));
        assert_eq!(find("0").1["slot"], json!(0));
        assert_eq!(find("guitar").1["slot"], json!(2), "any case");
        assert_eq!(find("GUITAR").1["slot"], json!(2));
        assert_eq!(find("Vocal").1["slot"], json!(1));
        let (s, j) = find("nope");
        assert_eq!(s, 404);
        assert_eq!(j["error"], json!("no track \"nope\" (GET /api/v1/tracks)"));
        assert_eq!(find("7").0, 404, "a slot nobody has");
        assert_eq!(find("-1").0, 404);
        assert_eq!(find("1.0").0, 404);
        assert_eq!(
            find("").0,
            404,
            "/api/v1/tracks/ is the list, and a POST to it is an unknown path (as the C++)"
        );
    });
}

#[test]
fn names_are_percent_decoded_like_juce() {
    let (srv, port) = server();
    let mut hub = Fake::new();
    hub.tracks[2].name = "Lead Vox ก".into();
    hub.tracks.push(track(3, "A+B"));
    drive(&srv, &mut hub, || {
        let slot = |key: &str| {
            req(port, "POST", &format!("/api/v1/tracks/{key}"), "{}").json()["slot"].clone()
        };
        assert_eq!(slot("Lead%20Vox%20%E0%B8%81"), json!(2));
        assert_eq!(
            slot("lead+vox+%E0%B8%81"),
            json!(2),
            "a + is a space, as in the C++"
        );
        assert_eq!(slot("A%2BB"), json!(3), "%2B is a real +");
        assert_eq!(slot("%56ocal"), json!(1));
    });
}

#[test]
fn bug_slot_numbers_do_not_wrap_and_do_not_hide_names() {
    let (srv, port) = server();
    let mut hub = Fake::new();
    hub.tracks[2].name = "1".into(); // a Track called "1" at slot 2
    drive(&srv, &mut hub, || {
        let find = |key: &str| req(port, "POST", &format!("/api/v1/tracks/{key}"), "{}");
        // C++ bug: getIntValue wrapped, so slot 4294967296 was slot 0
        assert_eq!(find("4294967296").status, 404);
        assert_eq!(find("4294967297").status, 404);
        assert_eq!(find("99999999999999999999999999").status, 404);
        // the slot number wins over a name made of digits (C++: whichever came first)
        assert_eq!(find("1").json()["slot"], json!(1));
        assert_eq!(
            find("2").json()["name"],
            json!("1"),
            "slot 2 is the Track called 1"
        );
    });
}

#[test]
fn two_tracks_with_one_name_take_the_first_like_the_c_plus_plus() {
    let (srv, port) = server();
    let mut hub = Fake::new();
    hub.tracks[2].name = "vocal".into();
    drive(&srv, &mut hub, || {
        let r = req(port, "POST", "/api/v1/tracks/Vocal", "{\"solo\": true}");
        assert_eq!(r.status, 200);
        assert_eq!(r.json()["slot"], json!(1), "the first in the list");
        assert_eq!(
            req(port, "POST", "/api/v1/tracks/2", "{\"solo\": true}").status,
            200
        );
    });
    assert_eq!(
        hub.sent(),
        [(1, ParamId::StrSolo, 1.0), (2, ParamId::StrSolo, 1.0)]
    );
}

// ---- App Audio ---------------------------------------------------------------------------------------------------

#[test]
fn source_commands() {
    let (srv, port) = server();
    let mut hub = Fake::new();
    let r = drive(&srv, &mut hub, || {
        req(
            port,
            "POST",
            "/api/v1/sources/0",
            "{\"on\": false, \"you_hear\": false, \"viewers_hear\": null, \"headphone_db\": -12, \"viewers_db\": 20, \"record\": true}",
        )
    });
    assert_eq!(r.status, 200);
    assert_eq!(
        r.body,
        "{\"ok\": true, \"index\": 0, \"name\": \"App Audio\"}\n"
    );
    // the levels go to its headphone slot (5), `on` and `record` to the App Audio itself
    assert_eq!(
        hub.sent(),
        [
            (5, ParamId::Mon, 0.0),
            (5, ParamId::Str, 0.0),
            (5, ParamId::MonTrimDb, -12.0),
            (5, ParamId::StrGainDb, 12.0),
        ]
    );
    assert_eq!(
        hub.sent_source(),
        [(0, SourceParam::On, 0.0), (0, SourceParam::Record, 1.0)]
    );
}

#[test]
fn source_toggles_and_lookup() {
    let (srv, port) = server();
    let mut hub = Fake::new();
    hub.sources.push(SourceView {
        index: 1,
        name: "Spotify".into(),
        program: "spotify.exe".into(),
        slot: 6,
        recording: true,
        ..SourceView::default()
    });
    drive(&srv, &mut hub, || {
        // on: no member = nothing; null = toggle; by name in any case; by index
        assert_eq!(
            req(
                port,
                "POST",
                "/api/v1/sources/spotify",
                "{\"on\": null, \"record\": null}"
            )
            .status,
            200
        );
        assert_eq!(req(port, "POST", "/api/v1/sources/1", "").status, 200);
        let r = req(port, "POST", "/api/v1/sources/nope", "{}");
        assert_eq!(r.status, 404);
        assert_eq!(r.error(), "no App Audio \"nope\" (GET /api/v1/sources)");
        assert_eq!(
            req(port, "POST", "/api/v1/sources/4294967296", "{}").status,
            404
        );
        assert_eq!(req(port, "POST", "/api/v1/sources/9", "{}").status, 404);
    });
    assert_eq!(
        hub.sent_source(),
        [(1, SourceParam::On, 1.0), (1, SourceParam::Record, 0.0)]
    );
    assert!(hub.sent().is_empty());
}

#[test]
fn old_app_audio_cannot_take_remote_levels() {
    let (srv, port) = server();
    let mut hub = Fake::new();
    hub.sources[0].slot = -1;
    drive(&srv, &mut hub, || {
        for member in ["you_hear", "viewers_hear", "headphone_db", "viewers_db"] {
            let value = if member.ends_with("db") { "-3" } else { "true" };
            let r = req(
                port,
                "POST",
                "/api/v1/sources/0",
                &format!("{{\"{member}\": {value}}}"),
            );
            assert_eq!(r.status, 400, "{member}");
            assert_eq!(r.error(), "this App Audio is too old for remote levels");
        }
        // its own switches still work
        assert_eq!(
            req(port, "POST", "/api/v1/sources/0", "{\"on\": false}").status,
            200
        );
    });
    assert_eq!(hub.sent_source(), [(0, SourceParam::On, 0.0)]);
    assert!(hub.sent().is_empty());
}

#[test]
fn two_app_audio_with_one_name_take_the_first_like_the_c_plus_plus() {
    let (srv, port) = server();
    let mut hub = Fake::new();
    hub.sources.push(SourceView {
        index: 1,
        name: "app audio".into(),
        slot: 6,
        ..SourceView::default()
    });
    drive(&srv, &mut hub, || {
        let r = req(
            port,
            "POST",
            "/api/v1/sources/App%20Audio",
            "{\"on\": false}",
        );
        assert_eq!(r.status, 200);
        assert_eq!(r.json()["index"], json!(0), "the first in the list");
        assert_eq!(
            req(port, "POST", "/api/v1/sources/1", "{\"on\": false}").status,
            200
        );
    });
    assert_eq!(
        hub.sent_source(),
        [(0, SourceParam::On, 0.0), (1, SourceParam::On, 0.0)]
    );
}

// ---- sync and share ----------------------------------------------------------------------------------------------

#[test]
fn sync_start_and_cancel() {
    let (srv, port) = server();
    let mut hub = Fake::new();
    drive(&srv, &mut hub, || {
        let post = |b: &str| req(port, "POST", "/api/v1/sync", b);
        let r = post("{\"action\": \"start\"}");
        assert_eq!((r.status, r.body.as_str()), (200, "{\"ok\": true}\n"));
        assert_eq!(post("{\"action\": \"cancel\"}").status, 200);
        assert_eq!(post("").status, 200, "no action = start");
        assert_eq!(post("{}").status, 200);
        for bad in [
            "{\"action\": \"stop\"}",
            "{\"action\": \"START\"}",
            "{\"action\": null}",
            "{\"action\": 5}",
            "{\"action\": true}",
            "{\"action\": \"\"}",
        ] {
            let r = post(bad);
            assert_eq!(r.status, 400, "{bad}");
            assert_eq!(r.error(), "action is start or cancel");
        }
    });
    assert_eq!(
        hub.events,
        [
            Ev::Sync("start"),
            Ev::Sync("cancel"),
            Ev::Sync("start"),
            Ev::Sync("start")
        ]
    );
}

#[test]
fn share_on_off_and_toggle() {
    let (srv, port) = server();
    let mut hub = Fake::new();
    drive(&srv, &mut hub, || {
        let post = |b: &str| req(port, "POST", "/api/v1/share", b);
        let r = post("{\"on\": true}");
        assert_eq!(r.status, 200);
        let j = r.json();
        assert_eq!(j["sharing"], json!(true));
        assert_eq!(
            j["listen_link"],
            json!("https://my-hearaside.vercel.app/l/abc")
        );
        let r = post("").json();
        assert_eq!(r["sharing"], json!(false), "toggle");
        assert_eq!(r["listen_link"], json!(""), "no link while not sharing");
        assert_eq!(post("{\"on\": null}").json()["sharing"], json!(true));
        assert_eq!(post("{\"on\": false}").json()["sharing"], json!(false));
    });
    assert_eq!(
        hub.events,
        [
            Ev::Sharing(true),
            Ev::Sharing(false),
            Ev::Sharing(true),
            Ev::Sharing(false)
        ]
    );
}

// ---- the message thread ------------------------------------------------------------------------------------------

#[test]
fn no_answer_within_two_seconds_is_503() {
    let (srv, port) = server(); // the default 2 s
    let start = Instant::now();
    let r = req(port, "GET", "/api/v1/state", "");
    let took = start.elapsed();
    assert_eq!(r.status, 503);
    assert_eq!(r.error(), "the DAW did not answer (busy?)");
    assert!(
        took >= Duration::from_millis(1900) && took < Duration::from_secs(5),
        "{took:?}"
    );
    assert_eq!(srv.pending(), 0, "the request left the queue");
}

#[test]
fn bug_a_request_that_got_503_is_never_run_late() {
    let s = ControlServer::with_ports(&[0])
        .with_timeouts(Duration::from_millis(300), Duration::from_secs(5));
    assert!(s.start(KEY));
    let port = s.port();
    // a toggle nobody serviced in time: the client has its 503 ...
    let r = req(port, "POST", "/api/v1/mute", "{}");
    assert_eq!(r.status, 503);
    // ... and when the message thread comes back it finds nothing to do (C++: it ran it, so the retry toggled twice)
    let mut calls = 0;
    s.service(|_| {
        calls += 1;
        Response::error(500, "must not run")
    });
    assert_eq!(calls, 0);
    assert_eq!(s.pending(), 0);
}

#[test]
fn bug_the_queue_is_bounded_while_the_message_thread_stalls() {
    let s = ControlServer::with_ports(&[0])
        .with_timeouts(Duration::from_millis(700), Duration::from_secs(5));
    assert!(s.start(KEY));
    let port = s.port();
    let mut peak = 0;
    let replies: Vec<u16> = std::thread::scope(|sc| {
        let clients: Vec<_> = (0..24)
            .map(|_| sc.spawn(move || req(port, "GET", "/api/v1/state", "").status))
            .collect();
        while !clients.iter().all(|c| c.is_finished()) {
            peak = peak.max(s.pending());
            std::thread::sleep(Duration::from_millis(3));
        }
        clients.into_iter().map(|c| c.join().unwrap()).collect()
    });
    assert!(
        peak <= 8,
        "at most one waiting request per connection, saw {peak}"
    );
    assert!(replies.iter().all(|&c| c == 503 || c == 0), "{replies:?}");
    assert!(replies.contains(&503));
    assert_eq!(s.pending(), 0);
    // and it works again once someone services
    let mut hub = Fake::new();
    assert_eq!(
        drive(&s, &mut hub, || req(port, "GET", "/api/v1", "").status),
        200
    );
}

#[test]
fn service_runs_requests_in_the_order_they_came_and_answers_on_the_servicing_thread() {
    let (srv, port) = server();
    let me = std::thread::current().id();
    let seen = std::sync::Mutex::new(Vec::new());
    std::thread::scope(|sc| {
        let c = sc.spawn(|| {
            (0..5)
                .map(|i| req(port, "POST", &format!("/api/v1/nope{i}"), "{}").status)
                .collect::<Vec<_>>()
        });
        while !c.is_finished() {
            srv.service(|r: &Request| {
                assert_eq!(std::thread::current().id(), me);
                seen.lock()
                    .unwrap()
                    .push((r.method.clone(), r.path.clone(), r.body.clone()));
                Response::error(404, "x")
            });
            std::thread::sleep(Duration::from_millis(2));
        }
        assert_eq!(c.join().unwrap(), [404; 5]);
    });
    let seen = seen.into_inner().unwrap();
    assert_eq!(seen.len(), 5);
    for (i, (m, p, b)) in seen.iter().enumerate() {
        assert_eq!(
            (m.as_str(), p.as_str()),
            ("POST", format!("/api/v1/nope{i}").as_str())
        );
        assert_eq!(b, &json!({}));
    }
}

#[test]
fn request_fields_reach_the_handler() {
    let (srv, port) = server();
    let seen = std::sync::Mutex::new(Vec::new());
    std::thread::scope(|sc| {
        let c = sc.spawn(|| {
            req(port, "get", "/api/v1/state?x=1", "");
            req(port, "POST", "/api/v1/x%20y", "{\"a\": [1, 2]}");
        });
        while !c.is_finished() {
            srv.service(|r: &Request| {
                seen.lock()
                    .unwrap()
                    .push((r.method.clone(), r.path.clone(), r.body.clone()));
                Response::new(200, Json::Null)
            });
            std::thread::sleep(Duration::from_millis(2));
        }
    });
    let seen = seen.into_inner().unwrap();
    assert_eq!(seen[0], ("GET".into(), "/api/v1/state".into(), Value::Null));
    assert_eq!(
        seen[1],
        ("POST".into(), "/api/v1/x%20y".into(), json!({"a": [1, 2]}))
    );
}

#[test]
fn a_panicking_handler_does_not_leave_the_client_waiting() {
    let (srv, port) = server();
    let r = std::thread::scope(|sc| {
        let c = sc.spawn(|| req(port, "GET", "/api/v1/state", ""));
        while !c.is_finished() {
            let _ = std::panic::catch_unwind(std::panic::AssertUnwindSafe(|| {
                srv.service(|_| panic!("handler bug"));
            }));
            std::thread::sleep(Duration::from_millis(2));
        }
        c.join().unwrap()
    });
    assert_eq!(r.status, 503);
    assert_eq!(r.error(), "the Hub failed to answer");
}

#[test]
fn bug_stop_answers_queued_requests_and_joins_every_thread_quickly() {
    let s = ControlServer::with_ports(&[0])
        .with_timeouts(Duration::from_secs(30), Duration::from_secs(30));
    assert!(s.start(KEY));
    let port = s.port();
    std::thread::scope(|sc| {
        // a request waiting for the message thread, a half-sent request and an idle connection
        let waiting = sc.spawn(|| req(port, "GET", "/api/v1/state", ""));
        let mut half = TcpStream::connect((Ipv4Addr::LOCALHOST, port)).unwrap();
        half.write_all(b"GET /api/v1/state HTTP/1.1\r\nHost: x")
            .unwrap();
        let idle = TcpStream::connect((Ipv4Addr::LOCALHOST, port)).unwrap();
        wait_until("the request is queued", || s.pending() == 1);
        let start = Instant::now();
        s.stop();
        assert!(
            start.elapsed() < Duration::from_secs(2),
            "{:?}",
            start.elapsed()
        );
        let r = waiting.join().unwrap();
        assert_eq!(r.status, 503);
        assert_eq!(r.error(), "the Hub is closing");
        let mut buf = [0u8; 8];
        half.set_read_timeout(Some(Duration::from_secs(2))).unwrap();
        let t = Instant::now();
        assert!(
            closed_by_peer(&half.read(&mut buf)),
            "the half-sent request is closed"
        );
        assert!(t.elapsed() < Duration::from_secs(1), "{:?}", t.elapsed());
        drop(idle);
    });
    assert!(!s.running());
    assert_eq!(s.pending(), 0);
    // a service call after stop is harmless
    s.service(|_| panic!("nothing is queued"));
    // the port is free for the next start
    assert!(s.start(KEY));
    s.stop();
}

#[test]
fn dropping_the_server_stops_it() {
    let (srv, port) = server();
    drop(srv);
    wait_until("the port is closed", || {
        TcpStream::connect_timeout(
            &(Ipv4Addr::LOCALHOST, port).into(),
            Duration::from_millis(200),
        )
        .is_err()
    });
}

// ---- RestService (serviceControl) --------------------------------------------------------------------------------

/// Runs `svc.service` on the test thread (every 2 ms, `now` moving 1 ms each time) while `client` runs.
fn tick_while<T: Send>(
    svc: &mut RestService,
    hub: &mut Fake,
    now: &mut u32,
    client: impl FnOnce() -> T + Send,
) -> T {
    std::thread::scope(|sc| {
        let h = sc.spawn(client);
        while !h.is_finished() {
            svc.service(hub, *now);
            *now = now.wrapping_add(1);
            std::thread::sleep(Duration::from_millis(2));
        }
        h.join().unwrap()
    })
}

fn call_with_key(port: u16, key: &str, path: &str) -> u16 {
    call(
        port,
        "GET",
        path,
        &[
            format!("Host: 127.0.0.1:{port}"),
            format!("Authorization: Bearer {key}"),
        ],
        "",
    )
    .status
}

#[test]
fn only_the_bus_owner_with_the_switch_on_opens_the_api() {
    let mut svc = RestService::with_server(ControlServer::with_ports(&[0]));
    let mut hub = Fake::new();
    let mut now = 10_000u32;
    hub.key = String::new();
    // switch off
    hub.enabled = false;
    svc.service(&mut hub, now);
    assert!(!svc.server().running());
    assert!(hub.key.is_empty(), "no key is made while the API is off");
    // a second Hub on the same bus (not the owner)
    hub.enabled = true;
    hub.owner = false;
    svc.service(&mut hub, now);
    assert!(!svc.server().running());
    assert!(hub.key.is_empty());
    // the owner with the switch on: the key is made (32 hex characters) and kept, the server runs
    hub.owner = true;
    svc.service(&mut hub, now);
    assert!(svc.server().running());
    assert_eq!(hub.key.len(), 32);
    let key = hub.key.clone();
    let port = svc.server().port();
    assert_eq!(
        tick_while(&mut svc, &mut hub, &mut now, || call_with_key(
            port, &key, "/api/v1"
        )),
        200
    );
    assert_eq!(hub.key, key, "the key is not made again");
    // the Hub stops owning the bus: the API closes
    hub.owner = false;
    svc.service(&mut hub, now);
    assert!(!svc.server().running());
    wait_until("the port is closed", || {
        TcpStream::connect_timeout(
            &(Ipv4Addr::LOCALHOST, port).into(),
            Duration::from_millis(200),
        )
        .is_err()
    });
    // owner again: back, after the retry time (a start is tried at most every 5 s, as in the C++)
    hub.owner = true;
    now += 1;
    svc.service(&mut hub, now);
    assert!(!svc.server().running());
    now += 6_000;
    svc.service(&mut hub, now);
    assert!(svc.server().running());
    // the switch goes off
    hub.enabled = false;
    svc.service(&mut hub, now);
    assert!(!svc.server().running());
}

#[test]
fn bug_a_new_key_applies_at_once() {
    let mut svc = RestService::with_server(ControlServer::with_ports(&[0]));
    let mut hub = Fake::new();
    let mut now = 50_000u32;
    svc.service(&mut hub, now);
    let old = hub.key.clone();
    assert!(svc.server().running());
    // the user makes a new key (the old one leaked): C++ kept answering to the old one until its 5 s timer
    hub.key = "fedcba9876543210fedcba9876543210".into();
    now += 10;
    svc.service(&mut hub, now);
    let port = svc.server().port();
    let new = hub.key.clone();
    assert_eq!(
        tick_while(&mut svc, &mut hub, &mut now, || call_with_key(
            port, &old, "/api/v1"
        )),
        401
    );
    assert_eq!(
        tick_while(&mut svc, &mut hub, &mut now, || call_with_key(
            port, &new, "/api/v1"
        )),
        200
    );
}

#[test]
fn a_busy_port_is_retried_every_five_seconds_not_every_tick() {
    let blocker = TcpListener::bind((Ipv4Addr::LOCALHOST, 0)).unwrap();
    let taken = blocker.local_addr().unwrap().port();
    let mut svc = RestService::with_server(ControlServer::with_ports(&[taken]));
    let mut hub = Fake::new();
    let start = u32::MAX - 2_000; // the clock wraps during the test
    svc.service(&mut hub, start);
    assert!(!svc.server().running(), "first try: busy");
    drop(blocker);
    svc.service(&mut hub, start.wrapping_add(1_000));
    assert!(!svc.server().running(), "not yet: 5 s");
    svc.service(&mut hub, start.wrapping_add(4_999));
    assert!(!svc.server().running());
    svc.service(&mut hub, start.wrapping_add(5_001));
    assert!(
        svc.server().running(),
        "5 s over (across the wrap of the clock)"
    );
}

#[test]
fn rest_service_answers_with_the_hub_it_is_given() {
    let mut svc = RestService::with_server(ControlServer::with_ports(&[0]));
    let mut hub = Fake::new();
    let mut now = 1u32;
    svc.service(&mut hub, now);
    let port = svc.server().port();
    let r = tick_while(&mut svc, &mut hub, &mut now, || {
        req(port, "POST", "/api/v1/mute", "{}")
    });
    assert_eq!(r.status, 200);
    assert_eq!(hub.values(HubParam::Panic), [1.0]);
}

// ---- clients that send things in odd ways ------------------------------------------------------------------------

#[test]
fn expect_100_continue_and_http_1_0() {
    let (srv, port) = server();
    let mut hub = Fake::new();
    drive(&srv, &mut hub, || {
        // PowerShell 5.1 / curl: head with Expect, a pause, then the body. The C++ sent no "100 Continue" and read
        // the body when it came; so does this.
        let body = "{\"on\": true}";
        let head = format!(
            "POST /api/v1/mute HTTP/1.1\r\nHost: 127.0.0.1:{port}\r\nAuthorization: Bearer {KEY}\r\nExpect: 100-continue\r\nContent-Length: {}\r\n\r\n",
            body.len()
        );
        let mut s = TcpStream::connect((Ipv4Addr::LOCALHOST, port)).unwrap();
        s.set_read_timeout(Some(Duration::from_secs(15))).unwrap();
        s.write_all(head.as_bytes()).unwrap();
        std::thread::sleep(Duration::from_millis(400));
        s.write_all(body.as_bytes()).unwrap();
        let mut out = Vec::new();
        let _ = s.read_to_end(&mut out);
        let r = parse_reply(&out);
        assert_eq!(r.status, 200);
        assert_eq!(r.json()["stream_muted"], json!(true));
        assert!(!r.head.contains("100"), "no interim response: {}", r.head);
        // HTTP/1.0 (no keep-alive, no Host needed by the protocol, but the Host check still applies)
        let raw = format!(
            "GET /api/v1 HTTP/1.0\r\nHost: 127.0.0.1:{port}\r\nAuthorization: Bearer {KEY}\r\n\r\n"
        );
        let r = send_raw(port, raw.as_bytes());
        assert_eq!(r.status, 200);
        assert!(r.body.starts_with("{\"name\": \"HEARASIDE Hub\""));
        let r = send_raw(
            port,
            format!("GET /api/v1 HTTP/1.0\r\nAuthorization: Bearer {KEY}\r\n\r\n").as_bytes(),
        );
        assert_eq!(
            r.status, 403,
            "HTTP/1.0 without Host: not a request from this computer's name"
        );
    });
    assert_eq!(hub.values(HubParam::Panic), [1.0]);
}

#[test]
fn only_a_chunked_transfer_encoding_is_refused() {
    let (srv, port) = server();
    let mut hub = Fake::new();
    drive(&srv, &mut hub, || {
        let post = |te: &str| {
            let mut h = good_headers(port);
            h.push(format!("Transfer-Encoding: {te}"));
            call(port, "POST", "/api/v1/mute", &h, "{}")
        };
        assert_eq!(post("identity").status, 200);
        assert_eq!(post("IDENTITY").status, 200);
        assert_eq!(post("chunked").status, 400);
        assert_eq!(post("Chunked").status, 400);
        assert_eq!(post("gzip, chunked").status, 400);
        assert_eq!(post("gzip").status, 200);
        assert_eq!(post("chunked").error(), "bad Content-Length");
    });
    assert_eq!(hub.values(HubParam::Panic).len(), 3);
}
