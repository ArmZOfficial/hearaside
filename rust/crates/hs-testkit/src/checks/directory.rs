//! `--test-directory`: `ShareDirectory` (permanent links) against a mock share web site: register, heartbeat, retry after
//! errors, unregister when sharing stops, and a quick shutdown while the site hangs (the DAW must never wait on the network).
use super::wait_for;
use crate::report::Report;
use hs_app::{DirectoryState, ShareDirectory};
use serde_json::Value;
use std::io::{Read, Write};
use std::net::{TcpListener, TcpStream};
use std::sync::atomic::{AtomicBool, AtomicU32, Ordering::SeqCst};
use std::sync::{Arc, Mutex};
use std::time::{Duration, Instant};

/// The mock site: answers `status` (200 = `{"ok":true}`) to every request, counts registers and the rest, keeps the
/// last body, or never answers (`hang`).
struct Mock {
    port: u16,
    run: Arc<AtomicBool>,
    hang: Arc<AtomicBool>,
    status: Arc<AtomicU32>,
    registers: Arc<AtomicU32>,
    unregisters: Arc<AtomicU32>,
    last_body: Arc<Mutex<String>>,
    thread: Option<std::thread::JoinHandle<()>>,
}

fn read_request(s: &mut TcpStream) -> Option<(String, String)> {
    let _ = s.set_read_timeout(Some(Duration::from_secs(2)));
    let mut req = Vec::new();
    let mut buf = [0u8; 4096];
    let head_end = loop {
        if let Some(i) = req.windows(4).position(|w| w == b"\r\n\r\n") {
            break i;
        }
        match s.read(&mut buf) {
            Ok(n) if n > 0 => req.extend_from_slice(&buf[..n]),
            _ => return None,
        }
    };
    let head = String::from_utf8_lossy(&req[..head_end]).into_owned();
    let len = head
        .to_ascii_lowercase()
        .split("content-length:")
        .nth(1)
        .and_then(|r| {
            r.trim_start()
                .split(['\r', '\n'])
                .next()?
                .trim()
                .parse::<usize>()
                .ok()
        })
        .unwrap_or(0);
    while req.len() < head_end + 4 + len {
        match s.read(&mut buf) {
            Ok(n) if n > 0 => req.extend_from_slice(&buf[..n]),
            _ => break,
        }
    }
    let body = String::from_utf8_lossy(&req[(head_end + 4).min(req.len())..]).into_owned();
    Some((head, body))
}

impl Mock {
    fn start() -> Mock {
        let listener = TcpListener::bind(("127.0.0.1", 0)).expect("a loopback port");
        let port = listener.local_addr().map_or(0, |a| a.port());
        listener.set_nonblocking(true).expect("non-blocking");
        let mut m = Mock {
            port,
            run: Arc::new(AtomicBool::new(true)),
            hang: Arc::new(AtomicBool::new(false)),
            status: Arc::new(AtomicU32::new(200)),
            registers: Arc::new(AtomicU32::new(0)),
            unregisters: Arc::new(AtomicU32::new(0)),
            last_body: Arc::new(Mutex::new(String::new())),
            thread: None,
        };
        let (run, hang, status) = (m.run.clone(), m.hang.clone(), m.status.clone());
        let (regs, unregs, last) = (
            m.registers.clone(),
            m.unregisters.clone(),
            m.last_body.clone(),
        );
        m.thread = Some(std::thread::spawn(move || {
            while run.load(SeqCst) {
                let Ok((mut s, _)) = listener.accept() else {
                    std::thread::sleep(Duration::from_millis(10));
                    continue;
                };
                let _ = s.set_nonblocking(false);
                let Some((head, body)) = read_request(&mut s) else {
                    continue;
                };
                if hang.load(SeqCst) {
                    // takes the request and never answers
                    while hang.load(SeqCst) && run.load(SeqCst) {
                        std::thread::sleep(Duration::from_millis(20));
                    }
                    continue;
                }
                if head.starts_with("POST /api/register") {
                    regs.fetch_add(1, SeqCst);
                } else {
                    unregs.fetch_add(1, SeqCst);
                }
                *last.lock().unwrap_or_else(|e| e.into_inner()) = body;
                let st = status.load(SeqCst);
                let reply = if st == 200 {
                    "{\"ok\":true}"
                } else {
                    "{\"error\":\"x\"}"
                };
                let _ = write!(
                    s,
                    "HTTP/1.1 {st} X\r\nContent-Type: application/json\r\nContent-Length: {}\r\nConnection: close\r\n\r\n{reply}",
                    reply.len()
                );
            }
        }));
        m
    }
}

impl Drop for Mock {
    fn drop(&mut self) {
        self.run.store(false, SeqCst);
        self.hang.store(false, SeqCst);
        if let Some(t) = self.thread.take() {
            let _ = t.join();
        }
    }
}

pub fn run(r: &Report) -> i32 {
    let mock = Mock::start();
    let base = format!("http://127.0.0.1:{}", mock.port);
    let secret = "ab".repeat(32);
    let (listen, send) = ("listentokenlisten000000000", "sendtokensendtoken00000000");
    {
        let dir = ShareDirectory::new();
        dir.set_timings(300, 100);
        dir.update(&base, listen, send, &secret, "", &[]);
        std::thread::sleep(Duration::from_millis(300));
        r.expect(
            "no tunnel yet: nothing sent",
            mock.registers.load(SeqCst) == 0 && dir.state() == DirectoryState::Off,
        );
        dir.update(
            &base,
            listen,
            send,
            &secret,
            "https://abc-def.trycloudflare.com",
            &[],
        );
        r.expect(
            "tunnel ready: registered, online",
            wait_for(3000, || dir.state() == DirectoryState::Online),
        );
        {
            let j: Value =
                serde_json::from_str(&mock.last_body.lock().unwrap_or_else(|e| e.into_inner()))
                    .unwrap_or(Value::Null);
            r.expect(
                "body: tunnel, both tokens, the secret",
                j["tunnel"] == "https://abc-def.trycloudflare.com"
                    && j["tokens"]["l"] == listen
                    && j["tokens"]["s"] == send
                    && j["secret"] == secret.as_str(),
            );
        }
        r.expect(
            "permanent link address",
            dir.listen_url() == format!("{base}/l/{listen}"),
        );
        let before = mock.registers.load(SeqCst);
        r.expect(
            "heartbeat keeps registering",
            wait_for(3000, || mock.registers.load(SeqCst) >= before + 2),
        );
        mock.status.store(503, SeqCst);
        r.expect(
            "site errors: unreachable",
            wait_for(3000, || dir.state() == DirectoryState::Unreachable),
        );
        mock.status.store(200, SeqCst);
        r.expect(
            "site back: online again (retry with backoff)",
            wait_for(5000, || dir.state() == DirectoryState::Online),
        );
        dir.stop();
        r.expect(
            "sharing off: unregistered",
            wait_for(3000, || mock.unregisters.load(SeqCst) == 1),
        );
        r.expect(
            "then off",
            wait_for(2000, || dir.state() == DirectoryState::Off),
        );
    }
    {
        let dir = ShareDirectory::new();
        dir.update(
            &base,
            listen,
            "",
            &secret,
            "https://abc-def.trycloudflare.com",
            &[],
        );
        std::thread::sleep(Duration::from_millis(100));
        mock.hang.store(true, SeqCst); // the site takes the request and never answers
        dir.update(
            &base,
            listen,
            "",
            &secret,
            "https://other-name.trycloudflare.com",
            &[],
        );
        std::thread::sleep(Duration::from_millis(300));
        let t0 = Instant::now();
        drop(dir); // the DAW closes the song now
        let ms = t0.elapsed().as_millis();
        r.line(format!("     shutdown with a hanging request took {ms} ms"));
        r.expect("closing never waits on the network (< 1 s)", ms < 1000);
        mock.hang.store(false, SeqCst);
    }
    r.finish("share directory")
}
