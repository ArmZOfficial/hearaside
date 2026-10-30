//! `--test-rest`: the REST API end to end (docs/rest-api.md) over a raw socket, against a Hub with its seven demo Tracks.
//! The requests block until the message thread answers, so they run on a client thread beside the message loop.
use crate::hub::{HubOptions, TestHub};
use crate::report::Report;
use serde_json::Value;
use std::io::{Read, Write};
use std::net::TcpStream;
use std::sync::atomic::{AtomicBool, Ordering::SeqCst};
use std::time::Duration;

const KEY: &str = "0123456789abcdef0123456789abcdef";

/// One request on a raw socket: `(status, body)`; status 0 = no connection.
fn call(port: u16, method: &str, path: &str, body: &str, extra: &str) -> (i32, String) {
    let Ok(mut s) =
        TcpStream::connect_timeout(&([127, 0, 0, 1], port).into(), Duration::from_secs(2))
    else {
        return (0, String::new());
    };
    let req = format!(
        "{method} {path} HTTP/1.1\r\n{extra}Content-Length: {}\r\nConnection: close\r\n\r\n{body}",
        body.len()
    );
    if s.write_all(req.as_bytes()).is_err() {
        return (0, String::new());
    }
    let _ = s.set_read_timeout(Some(Duration::from_secs(3)));
    let mut all = Vec::new();
    let mut buf = [0u8; 4096];
    while let Ok(n) = s.read(&mut buf) {
        if n == 0 {
            break;
        }
        all.extend_from_slice(&buf[..n]);
    }
    let text = String::from_utf8_lossy(&all).into_owned();
    let status = text
        .split(' ')
        .nth(1)
        .and_then(|s| s.trim().parse().ok())
        .unwrap_or(0);
    let body = text
        .split_once("\r\n\r\n")
        .map(|(_, b)| b.to_owned())
        .unwrap_or_default();
    (status, body)
}

pub fn run(r: &Report, opts: &HubOptions) -> i32 {
    let mut hub = TestHub::new(opts);
    hub.settings().set_rest_api(true);
    hub.settings().set_rest_api_key(KEY);
    let started = hub.pump_until(2000, |h| h.rest_running());
    if !started {
        r.line("FAIL: the REST API did not start");
        return 1;
    }
    let port = hub.rest_port();
    let host = format!("Host: 127.0.0.1:{port}\r\n");
    let key = format!("Authorization: Bearer {KEY}\r\n");
    let tracks = hub.tracks_on_bus();
    let track_count = tracks.len();
    let finished = AtomicBool::new(false);
    // the requests wait for the message thread: the client runs beside the loop that answers them
    std::thread::scope(|scope| {
        scope.spawn(|| {
            let get = |extra: &str| call(port, "GET", "/api/v1/state", "", extra);
            let (status, _) = get(&host);
            r.expect("no key -> 401", status == 401);
            let (status, _) = get(&format!(
                "{host}Authorization: Bearer wrongwrongwrongwrongwrongwrong00\r\n"
            ));
            r.expect("wrong key -> 401", status == 401);
            let (status, _) = get(&format!("{host}{key}Origin: https://evil.example\r\n"));
            r.expect("a web page (Origin) -> 403", status == 403);
            let (status, _) = get(&format!("Host: evil.example:{port}\r\n{key}"));
            r.expect("another host name (DNS rebinding) -> 403", status == 403);
            let (status, body) = get(&format!("{host}{key}"));
            let state: Value = serde_json::from_str(&body).unwrap_or(Value::Null);
            r.expect(
                "GET /api/v1/state -> 200 with every track",
                status == 200 && state["tracks"].as_array().map(Vec::len) == Some(track_count),
            );
            let (status, body) = call(
                port,
                "POST",
                "/api/v1/mute",
                "{\"on\": true}",
                &format!("{host}{key}"),
            );
            let v: Value = serde_json::from_str(&body).unwrap_or(Value::Null);
            r.expect(
                "POST /api/v1/mute {on: true} -> 200, stream_muted",
                status == 200 && v["stream_muted"] == true,
            );
            let (status, _) = call(
                port,
                "POST",
                "/api/v1/tracks/Vocal",
                "{\"you_hear\": true, \"viewers_db\": -6}",
                &format!("{host}{key}"),
            );
            r.expect("POST /api/v1/tracks/<name> -> 200", status == 200);
            let (status, _) = call(
                port,
                "POST",
                "/api/v1/tracks/nope",
                "{}",
                &format!("{host}{key}"),
            );
            r.expect("unknown track -> 404", status == 404);
            let (status, _) = call(
                port,
                "POST",
                "/api/v1/stream",
                "not json",
                &format!("{host}{key}"),
            );
            r.expect("bad JSON -> 400", status == 400);
            finished.store(true, SeqCst);
        });
        for _ in 0..400 {
            if finished.load(SeqCst) {
                break;
            }
            hub.pump(25);
        }
    });
    hub.pump(300); // the Track takes the command within a block, through its own parameters
    r.expect(
        "the Hub parameter changed (host sees it)",
        hub.params().panic(),
    );
    let vocal = tracks.get(1).map(|t| t.params());
    r.expect(
        "the Track's \"you hear\" changed",
        vocal.is_some_and(|p| p.mon),
    );
    r.expect(
        "the Track's viewers level changed",
        vocal.is_some_and(|p| (p.str_gain_db + 6.0).abs() < 0.01),
    );
    r.finish("REST API")
}
