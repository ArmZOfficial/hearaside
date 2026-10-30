//! `ui-snapshot --test-directory` (ctest `share_directory`) for the Rust client, plus the bug regressions, plus the same
//! client against the real `web/share-vercel` server logic (ctest `share_api`'s server, run with Node when it is there).
mod common;
use common::{Fake, Resp, holds_for, wait_for};
use hs_app::{DirectoryState, Resolved, ShareDirectory};
use serde_json::{Value, json};
use std::io::{Read, Write};
use std::net::TcpStream;
use std::sync::Arc;
use std::sync::atomic::{AtomicU16, Ordering};
use std::time::{Duration, Instant};

const LISTEN: &str = "listentokenlisten000000000";
const SEND: &str = "sendtokensendtoken00000000";
const TUNNEL: &str = "https://abc-def.trycloudflare.com";

fn secret() -> String {
    "ab".repeat(32)
}

/// A mock share site: answers with `status` (shared, changeable) for everything.
fn mock(status: Arc<AtomicU16>) -> Fake {
    Fake::start(move |_| {
        let s = status.load(Ordering::SeqCst);
        Resp::json(
            s,
            if s == 200 {
                json!({"ok": true})
            } else {
                json!({"error": "x"})
            },
        )
    })
}

fn state_is(dir: &ShareDirectory, s: DirectoryState, ms: u64) -> bool {
    wait_for(ms, || dir.state() == s)
}

#[test]
fn register_heartbeat_retry_unregister() {
    let status = Arc::new(AtomicU16::new(200));
    let site = mock(Arc::clone(&status));
    let base = site.base();
    let dir = ShareDirectory::new();
    dir.set_timings(300, 100);

    dir.update(&base, LISTEN, SEND, &secret(), "", &[]);
    std::thread::sleep(Duration::from_millis(300));
    assert!(
        site.count("POST /api/register") == 0 && dir.state() == DirectoryState::Off,
        "no tunnel yet: nothing sent"
    );

    dir.update(&base, LISTEN, SEND, &secret(), TUNNEL, &[]);
    assert!(
        state_is(&dir, DirectoryState::Online, 3000),
        "tunnel ready: registered, online"
    );
    let last = site.requests("POST /api/register").pop().unwrap();
    let j = last.json();
    assert_eq!(j["tunnel"], TUNNEL);
    assert_eq!(j["tokens"]["l"], LISTEN);
    assert_eq!(j["tokens"]["s"], SEND);
    assert_eq!(
        j["secret"],
        secret().as_str(),
        "body: tunnel, both tokens, the secret"
    );
    assert!(j["tokens"].get("f").is_none(), "no friends: no f");
    assert_eq!(last.header("content-type"), Some("application/json"));
    assert_eq!(
        dir.listen_url(),
        format!("{base}/l/{LISTEN}"),
        "permanent link address"
    );
    assert_eq!(dir.send_url(), format!("{base}/s/{SEND}"));

    let before = site.count("POST /api/register");
    assert!(
        wait_for(3000, || site.count("POST /api/register") >= before + 2),
        "heartbeat keeps registering"
    );

    status.store(503, Ordering::SeqCst);
    assert!(
        state_is(&dir, DirectoryState::Unreachable, 3000),
        "site errors: unreachable"
    );
    status.store(200, Ordering::SeqCst);
    assert!(
        state_is(&dir, DirectoryState::Online, 5000),
        "site back: online again (retry with backoff)"
    );

    dir.stop();
    assert!(
        wait_for(3000, || site.count("POST /api/unregister") == 1),
        "sharing off: unregistered"
    );
    assert!(state_is(&dir, DirectoryState::Off, 2000), "then off");
    let un = site.requests("POST /api/unregister").pop().unwrap().json();
    assert_eq!(un["secret"], secret().as_str());
    assert_eq!(un["tokens"]["l"], LISTEN);
    assert!(un.get("tunnel").is_none(), "unregister carries no tunnel");
    std::thread::sleep(Duration::from_millis(400));
    assert_eq!(
        site.count("POST /api/unregister"),
        1,
        "unregistered exactly once"
    );
}

#[test]
fn closing_never_waits_on_a_hanging_site() {
    let status = Arc::new(AtomicU16::new(200));
    let site = mock(status);
    let base = site.base();
    let dir = ShareDirectory::new();
    dir.update(&base, LISTEN, "", &secret(), TUNNEL, &[]);
    assert!(state_is(&dir, DirectoryState::Online, 3000));
    site.hang.store(true, Ordering::SeqCst); // the site takes the request and never answers
    dir.update(
        &base,
        LISTEN,
        "",
        &secret(),
        "https://other-name.trycloudflare.com",
        &[],
    );
    assert!(wait_for(2000, || site.count("POST /api/register") >= 2));
    let alive = dir.worker_alive_flag();
    let detached = hs_app::detached_workers();
    let t0 = Instant::now();
    drop(dir); // the DAW closes the song now
    let took = t0.elapsed();
    println!("shutdown with a hanging request took {took:?}");
    assert!(
        took < Duration::from_secs(1),
        "closing never waits on the network (< 1 s)"
    );
    assert!(
        hs_app::detached_workers() > detached,
        "the thread outlived the drop, so the module was pinned first"
    );
    site.hang.store(false, Ordering::SeqCst);
    assert!(
        wait_for(15_000, || !alive.load(Ordering::SeqCst)),
        "the thread ends once its call returns, and starts no other"
    );
    let n = site.count("POST");
    assert!(holds_for(300, || site.count("POST") == n));
}

#[test]
fn friends_and_changes_are_registered() {
    let status = Arc::new(AtomicU16::new(200));
    let site = mock(status);
    let base = site.base();
    let dir = ShareDirectory::new();
    let friends = vec![
        "friendaaaaaaaaaaaaaaaaaaa".to_owned(),
        "friendbbbbbbbbbbbbbbbbbbb".to_owned(),
    ];
    dir.update(&format!("{base}/"), "", "", &secret(), TUNNEL, &friends);
    assert!(
        state_is(&dir, DirectoryState::Online, 3000),
        "friends alone are enough to register"
    );
    let j = site.requests("POST /api/register")[0].json();
    assert_eq!(j["tokens"]["f"], json!(friends));
    assert!(j["tokens"].get("l").is_none() && j["tokens"].get("s").is_none());
    // base given with a trailing slash is stored without it
    assert!(site.requests("POST /api/register")[0].path == "/api/register");
    let n = site.count("POST /api/register");
    dir.update(
        &base,
        "",
        "",
        &secret(),
        "https://new-name.trycloudflare.com",
        &friends,
    );
    assert!(
        wait_for(3000, || site.count("POST /api/register") > n),
        "a new tunnel is registered at once, not at the next heartbeat"
    );
    assert_eq!(
        site.requests("POST /api/register").pop().unwrap().json()["tunnel"],
        "https://new-name.trycloudflare.com"
    );
}

#[test]
fn revoked_links_are_handed_to_the_hub_once() {
    let site = Fake::start(|_| {
        Resp::json(
            200,
            json!({"ok": true, "ttl": 90, "tokens": [], "revoked": [LISTEN, 5, "x"]}),
        )
    });
    let dir = ShareDirectory::new();
    dir.set_timings(100, 100);
    dir.update(&site.base(), LISTEN, SEND, &secret(), TUNNEL, &[]);
    assert!(
        wait_for(3000, || site.count("POST /api/register") >= 3),
        "heartbeats keep coming"
    );
    assert_eq!(
        dir.take_revoked(),
        vec![LISTEN.to_owned(), "x".to_owned()],
        "each revoked token once, in order, non-strings dropped"
    );
    assert!(dir.take_revoked().is_empty());
}

#[test]
fn bearer_goes_to_the_site_that_is_asked_for() {
    let status = Arc::new(AtomicU16::new(200));
    let site = mock(status);
    let base = site.base();
    let dir = ShareDirectory::new();
    let account = base.clone();
    dir.set_bearer(move |b| {
        if b == account {
            "tok-123".to_owned()
        } else {
            String::new()
        }
    });
    dir.update(&base, LISTEN, "", &secret(), TUNNEL, &[]);
    assert!(state_is(&dir, DirectoryState::Online, 3000));
    assert_eq!(
        site.requests("POST /api/register")[0].header("authorization"),
        Some("Bearer tok-123")
    );
    let other = ShareDirectory::new();
    other.set_bearer(|_| String::new());
    other.update(&base, LISTEN, "", &secret(), TUNNEL, &[]);
    assert!(state_is(&other, DirectoryState::Online, 3000));
    assert!(
        site.requests("POST /api/register")
            .iter()
            .any(|r| r.header("authorization").is_none()),
        "no token for a base that has none"
    );
}

#[test]
fn bug_secret_never_goes_in_clear_text_to_another_computer() {
    // C++ bug: the secret (and the bearer) were sent to any http:// base. Now only https, or this computer.
    let dir = ShareDirectory::new();
    dir.update("http://192.0.2.1:9", LISTEN, SEND, &secret(), TUNNEL, &[]);
    assert!(
        state_is(&dir, DirectoryState::Unreachable, 1000),
        "refused at once, no 5 s connect attempt"
    );
}

#[test]
fn bug_rate_limited_site_is_not_hammered() {
    // C++ bug: 429 ("slow down") was retried after 2 s, doubling, like any outage. Now it waits like 400 / 403.
    let status = Arc::new(AtomicU16::new(429));
    let site = mock(status);
    let dir = ShareDirectory::new();
    dir.set_timings(300, 50);
    dir.update(&site.base(), LISTEN, "", &secret(), TUNNEL, &[]);
    assert!(state_is(&dir, DirectoryState::Unreachable, 3000));
    std::thread::sleep(Duration::from_millis(600));
    assert_eq!(
        site.count("POST /api/register"),
        1,
        "no retry within the slow window"
    );
    // but a changed setup is tried at once
    dir.update(
        &site.base(),
        LISTEN,
        "",
        &secret(),
        "https://again-name.trycloudflare.com",
        &[],
    );
    assert!(wait_for(2000, || site.count("POST /api/register") == 2));
}

#[test]
fn resolve_reads_the_directory_answer() {
    let site = Fake::start(|r| match r.path.as_str() {
        "/api/resolve/onlinetoken000000000000" => Resp::json(
            200,
            json!({"online": true, "ws": "wss://able-dog-cat.trycloudflare.com/ws/l/onlinetoken000000000000"}),
        ),
        "/api/resolve/insecuretunnel00000000" => Resp::json(
            200,
            json!({"online": true, "ws": "ws://192.168.1.5:47810/ws/l/insecuretunnel00000000"}),
        ),
        "/api/resolve/offlinetoken00000000000" => Resp::json(200, json!({"online": false})),
        "/api/resolve/slowdown" => Resp::json(429, json!({"online": false, "retry": true})),
        "/api/resolve/nodirectory" => Resp::json(404, json!({"hello": "not a directory"})),
        _ => Resp {
            status: 200,
            body: "<html>".into(),
            location: None,
        },
    });
    let base = site.base();
    assert_eq!(
        ShareDirectory::resolve(&base, "onlinetoken000000000000"),
        Resolved::Online(
            "wss://able-dog-cat.trycloudflare.com/ws/l/onlinetoken000000000000".into()
        )
    );
    assert_eq!(
        ShareDirectory::resolve(&format!("{base}/"), "offlinetoken00000000000"),
        Resolved::Offline
    );
    assert_eq!(
        ShareDirectory::resolve(&base, "insecuretunnel00000000"),
        Resolved::Offline,
        "only secure tunnels are handed out"
    );
    assert_eq!(
        ShareDirectory::resolve(&base, "slowdown"),
        Resolved::Offline
    );
    assert_eq!(
        ShareDirectory::resolve(&base, "nodirectory"),
        Resolved::NoDirectory
    );
    assert_eq!(
        ShareDirectory::resolve(&base, "whatever"),
        Resolved::NoDirectory,
        "an answer without 'online' is not a directory"
    );
    let n = site.count("GET");
    // C++ bug: the token was appended to the path as given
    assert_eq!(
        ShareDirectory::resolve(&base, "../../etc"),
        Resolved::Offline
    );
    assert_eq!(ShareDirectory::resolve(&base, "a/b?x=1"), Resolved::Offline);
    assert_eq!(ShareDirectory::resolve(&base, ""), Resolved::Offline);
    assert_eq!(site.count("GET"), n, "refused without a request");
    // not https and not this computer: not a directory
    assert_eq!(
        ShareDirectory::resolve("http://192.0.2.1:9", "onlinetoken000000000000"),
        Resolved::NoDirectory
    );
    assert_eq!(
        ShareDirectory::resolve("http://127.0.0.1:9", "onlinetoken000000000000"),
        Resolved::NoDirectory,
        "nothing listening"
    );
}

// ---- against the real server logic ------------------------------------------------------------------------------

struct Node(std::process::Child);

impl Drop for Node {
    fn drop(&mut self) {
        let _ = self.0.kill();
        let _ = self.0.wait();
    }
}

fn dechunk(mut b: &str) -> String {
    let mut out = String::new();
    while let Some((size, rest)) = b.split_once("\r\n") {
        let n = usize::from_str_radix(size.trim(), 16).unwrap_or(0);
        if n == 0 || rest.len() < n {
            break;
        }
        out.push_str(&rest[..n]);
        b = rest[n..].trim_start_matches("\r\n");
    }
    out
}

fn http_get(port: u16, path: &str) -> (u16, Value) {
    let mut s = TcpStream::connect(("127.0.0.1", port)).unwrap();
    s.set_read_timeout(Some(Duration::from_secs(5))).unwrap();
    write!(
        s,
        "GET {path} HTTP/1.1\r\nHost: 127.0.0.1\r\nConnection: close\r\n\r\n"
    )
    .unwrap();
    let mut out = String::new();
    let _ = s.read_to_string(&mut out);
    let status = out
        .split(' ')
        .nth(1)
        .and_then(|c| c.parse().ok())
        .unwrap_or(0);
    let (head, body) = out.split_once("\r\n\r\n").unwrap_or((&out, ""));
    let body = if head
        .to_ascii_lowercase()
        .contains("transfer-encoding: chunked")
    {
        dechunk(body)
    } else {
        body.to_owned()
    };
    (status, serde_json::from_str(&body).unwrap_or(Value::Null))
}

#[test]
fn client_against_the_share_vercel_server() {
    // `web/share-vercel/scripts/dev-server.mjs` serves the same handlers (lib/handlers.js) as the Vercel functions that
    // `share_api` tests: register, resolve, unregister, the 403 for someone else's secret.
    let root = std::path::Path::new(env!("CARGO_MANIFEST_DIR")).join("../../../web/share-vercel");
    let script = root.join("scripts/dev-server.mjs");
    if !script.exists() {
        println!("SKIPPED: {} not found", script.display());
        return;
    }
    let port = std::net::TcpListener::bind("127.0.0.1:0")
        .unwrap()
        .local_addr()
        .unwrap()
        .port();
    let child = std::process::Command::new("node")
        .arg(&script)
        .arg(port.to_string())
        .current_dir(&root)
        .stdout(std::process::Stdio::null())
        .stderr(std::process::Stdio::null())
        .spawn();
    let node = match child {
        Ok(c) => Node(c),
        Err(e) => {
            println!("SKIPPED: node is not on PATH ({e})");
            return;
        }
    };
    if !wait_for(10_000, || TcpStream::connect(("127.0.0.1", port)).is_ok()) {
        panic!("the dev server did not start");
    }
    let base = format!("http://127.0.0.1:{port}");
    let (l, s, f) = (
        "listen0123456789abcdefgh",
        "send0123456789abcdefghij",
        "friendaaaaaaaaaaaaaaaaaaa",
    );
    let tunnel = "https://able-dog-cat.trycloudflare.com";

    let dir = ShareDirectory::new();
    dir.update(&base, l, s, &secret(), tunnel, &[f.to_owned()]);
    assert!(
        state_is(&dir, DirectoryState::Online, 5000),
        "the real server accepts the register body"
    );
    let (code, body) = http_get(port, &format!("/api/resolve/{l}"));
    assert_eq!(
        (code, &body),
        (
            200,
            &json!({"online": true, "ws": format!("wss://able-dog-cat.trycloudflare.com/ws/l/{l}")})
        )
    );
    assert_eq!(
        http_get(port, &format!("/api/resolve/{s}")).1["ws"],
        format!("wss://able-dog-cat.trycloudflare.com/ws/s/{s}")
    );
    assert_eq!(
        http_get(port, &format!("/api/resolve/{f}")).1["ws"],
        format!("wss://able-dog-cat.trycloudflare.com/ws/s/{f}"),
        "friend links resolve as send-in"
    );

    assert_eq!(
        ShareDirectory::resolve(&base, l),
        Resolved::Online(format!("wss://able-dog-cat.trycloudflare.com/ws/l/{l}")),
        "the Rust resolve client reads the real answer"
    );
    assert_eq!(
        ShareDirectory::resolve(&base, "nobodyhasthistoken000000"),
        Resolved::Offline
    );

    // a new tunnel replaces the old one under the same link
    dir.update(
        &base,
        l,
        s,
        &secret(),
        "https://new-name-here.trycloudflare.com",
        &[f.to_owned()],
    );
    assert!(wait_for(5000, || http_get(
        port,
        &format!("/api/resolve/{l}")
    )
    .1["ws"]
        == format!("wss://new-name-here.trycloudflare.com/ws/l/{l}")));

    // someone else's secret cannot take the link over: 403 -> unreachable, and the link stays with its owner
    let thief = ShareDirectory::new();
    thief.update(
        &base,
        l,
        "",
        &"b".repeat(64),
        "https://evil-site.trycloudflare.com",
        &[],
    );
    assert!(
        state_is(&thief, DirectoryState::Unreachable, 5000),
        "403 for a link that is not theirs"
    );
    assert_eq!(
        http_get(port, &format!("/api/resolve/{l}")).1["ws"],
        format!("wss://new-name-here.trycloudflare.com/ws/l/{l}")
    );
    drop(thief);

    // sharing off: the link goes offline, the owner keeps the token
    dir.stop();
    assert!(
        wait_for(5000, || http_get(port, &format!("/api/resolve/{l}")).1
            == json!({"online": false})),
        "unregister takes the link offline"
    );
    assert!(state_is(&dir, DirectoryState::Off, 2000));
    drop(dir);
    drop(node);
}
