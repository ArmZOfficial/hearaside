//! A fake web server on 127.0.0.1 for the tests: plain HTTP, one thread per connection, every request recorded.
#![allow(dead_code)]
use std::collections::HashMap;
use std::io::{Read, Write};
use std::net::{Shutdown, TcpListener, TcpStream};
use std::path::PathBuf;
use std::sync::atomic::{AtomicBool, AtomicU64, Ordering};
use std::sync::{Arc, Mutex};
use std::time::{Duration, Instant};

#[derive(Clone, Debug, Default)]
pub struct Req {
    pub method: String,
    pub path: String,
    pub headers: HashMap<String, String>,
    pub body: Vec<u8>,
}

impl Req {
    pub fn json(&self) -> serde_json::Value {
        serde_json::from_slice(&self.body).unwrap_or(serde_json::Value::Null)
    }
    pub fn header(&self, name: &str) -> Option<&str> {
        self.headers
            .get(&name.to_ascii_lowercase())
            .map(String::as_str)
    }
}

#[derive(Clone, Debug)]
pub struct Resp {
    pub status: u16,
    pub body: String,
    pub location: Option<String>,
}

impl Resp {
    pub fn json(status: u16, v: serde_json::Value) -> Self {
        Self {
            status,
            body: v.to_string(),
            location: None,
        }
    }
}

type Handler = Box<dyn Fn(&Req) -> Resp + Send + Sync>;

pub struct Fake {
    pub port: u16,
    pub log: Arc<Mutex<Vec<Req>>>,
    /// while set, a request is taken and never answered
    pub hang: Arc<AtomicBool>,
    /// every request waits this long (ms) before it is answered
    pub delay_ms: Arc<AtomicU64>,
    stop: Arc<AtomicBool>,
    thread: Option<std::thread::JoinHandle<()>>,
}

impl Fake {
    pub fn start(handler: impl Fn(&Req) -> Resp + Send + Sync + 'static) -> Fake {
        let listener = TcpListener::bind("127.0.0.1:0").expect("bind");
        listener.set_nonblocking(true).unwrap();
        let port = listener.local_addr().unwrap().port();
        let log = Arc::new(Mutex::new(Vec::new()));
        let hang = Arc::new(AtomicBool::new(false));
        let delay_ms = Arc::new(AtomicU64::new(0));
        let stop = Arc::new(AtomicBool::new(false));
        let handler: Arc<Handler> = Arc::new(Box::new(handler));
        let (l2, h2, s2, d2) = (
            Arc::clone(&log),
            Arc::clone(&hang),
            Arc::clone(&stop),
            Arc::clone(&delay_ms),
        );
        let thread = std::thread::spawn(move || {
            while !s2.load(Ordering::SeqCst) {
                match listener.accept() {
                    Ok((conn, _)) => {
                        let (handler, log, hang, stop, delay) = (
                            Arc::clone(&handler),
                            Arc::clone(&l2),
                            Arc::clone(&h2),
                            Arc::clone(&s2),
                            Arc::clone(&d2),
                        );
                        std::thread::spawn(move || {
                            serve(conn, &handler, &log, &hang, &stop, &delay)
                        });
                    }
                    Err(_) => std::thread::sleep(Duration::from_millis(5)),
                }
            }
        });
        Fake {
            port,
            log,
            hang,
            delay_ms,
            stop,
            thread: Some(thread),
        }
    }

    pub fn base(&self) -> String {
        format!("http://127.0.0.1:{}", self.port)
    }

    /// Requests whose "METHOD path" starts with `prefix` (e.g. `"POST /api/register"`).
    pub fn count(&self, prefix: &str) -> usize {
        self.log
            .lock()
            .unwrap()
            .iter()
            .filter(|r| format!("{} {}", r.method, r.path).starts_with(prefix))
            .count()
    }

    pub fn requests(&self, prefix: &str) -> Vec<Req> {
        self.log
            .lock()
            .unwrap()
            .iter()
            .filter(|r| format!("{} {}", r.method, r.path).starts_with(prefix))
            .cloned()
            .collect()
    }
}

impl Drop for Fake {
    fn drop(&mut self) {
        self.stop.store(true, Ordering::SeqCst);
        self.hang.store(false, Ordering::SeqCst);
        if let Some(t) = self.thread.take() {
            let _ = t.join();
        }
    }
}

fn serve(
    mut conn: TcpStream,
    handler: &Handler,
    log: &Mutex<Vec<Req>>,
    hang: &AtomicBool,
    stop: &AtomicBool,
    delay: &AtomicU64,
) {
    let _ = conn.set_nonblocking(false);
    let _ = conn.set_read_timeout(Some(Duration::from_secs(2)));
    let mut buf = Vec::new();
    let mut chunk = [0u8; 4096];
    let head_end = loop {
        if let Some(i) = buf.windows(4).position(|w| w == b"\r\n\r\n") {
            break i;
        }
        match conn.read(&mut chunk) {
            Ok(0) | Err(_) => return,
            Ok(n) => buf.extend_from_slice(&chunk[..n]),
        }
    };
    let head = String::from_utf8_lossy(&buf[..head_end]).into_owned();
    let mut lines = head.split("\r\n");
    let mut first = lines.next().unwrap_or("").split(' ');
    let mut req = Req {
        method: first.next().unwrap_or("").into(),
        path: first.next().unwrap_or("").into(),
        ..Req::default()
    };
    for l in lines {
        if let Some((k, v)) = l.split_once(':') {
            req.headers
                .insert(k.trim().to_ascii_lowercase(), v.trim().to_owned());
        }
    }
    let len: usize = req
        .header("content-length")
        .and_then(|v| v.parse().ok())
        .unwrap_or(0);
    let mut body = buf[head_end + 4..].to_vec();
    while body.len() < len {
        match conn.read(&mut chunk) {
            Ok(0) | Err(_) => break,
            Ok(n) => body.extend_from_slice(&chunk[..n]),
        }
    }
    req.body = body;
    log.lock().unwrap().push(req.clone());
    if hang.load(Ordering::SeqCst) {
        while hang.load(Ordering::SeqCst) && !stop.load(Ordering::SeqCst) {
            std::thread::sleep(Duration::from_millis(20));
        }
        return;
    }
    let until = Instant::now() + Duration::from_millis(delay.load(Ordering::SeqCst));
    while Instant::now() < until && !stop.load(Ordering::SeqCst) {
        std::thread::sleep(Duration::from_millis(10));
    }
    let r = handler(&req);
    let loc = r
        .location
        .as_ref()
        .map(|l| {
            format!(
                "Location: {l}
"
            )
        })
        .unwrap_or_default();
    let out = format!(
        "HTTP/1.1 {} X
Content-Type: application/json
{loc}Content-Length: {}
Connection: close

{}",
        r.status,
        r.body.len(),
        r.body
    );
    let _ = conn.write_all(out.as_bytes());
    let _ = conn.shutdown(Shutdown::Write);
}

pub fn wait_for(ms: u64, mut f: impl FnMut() -> bool) -> bool {
    let end = Instant::now() + Duration::from_millis(ms);
    loop {
        if f() {
            return true;
        }
        if Instant::now() >= end {
            return false;
        }
        std::thread::sleep(Duration::from_millis(10));
    }
}

/// `f` stays true for `ms` (polled every 10 ms; false the moment it is not). For "nothing happens" checks.
pub fn holds_for(ms: u64, mut f: impl FnMut() -> bool) -> bool {
    let end = Instant::now() + Duration::from_millis(ms);
    while Instant::now() < end {
        if !f() {
            return false;
        }
        std::thread::sleep(Duration::from_millis(10));
    }
    f()
}

/// Polls `f` until it has given the same value for `quiet_ms` (a counter that stopped moving), then returns it.
pub fn settled<T: PartialEq>(quiet_ms: u64, mut f: impl FnMut() -> T) -> T {
    let mut last = f();
    let mut since = Instant::now();
    while since.elapsed() < Duration::from_millis(quiet_ms) {
        std::thread::sleep(Duration::from_millis(10));
        let now = f();
        if now != last {
            last = now;
            since = Instant::now();
        }
    }
    last
}

/// A fresh empty folder under the target dir, removed on drop.
pub struct TempDir(pub PathBuf);

impl TempDir {
    pub fn new(tag: &str) -> TempDir {
        static N: std::sync::atomic::AtomicU32 = std::sync::atomic::AtomicU32::new(0);
        let dir = std::env::temp_dir().join(format!(
            "hs-app-test-{}-{}-{}",
            tag,
            std::process::id(),
            N.fetch_add(1, Ordering::SeqCst)
        ));
        let _ = std::fs::remove_dir_all(&dir);
        std::fs::create_dir_all(&dir).unwrap();
        TempDir(dir)
    }
    pub fn file(&self, name: &str) -> PathBuf {
        self.0.join(name)
    }
}

impl Drop for TempDir {
    fn drop(&mut self) {
        let _ = std::fs::remove_dir_all(&self.0);
    }
}
