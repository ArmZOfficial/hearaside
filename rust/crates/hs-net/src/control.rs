//! `ControlServer`: the HTTP side of the Hub's REST API (docs/rest-api.md). Port of `plugins/hub/ControlServer.{h,cpp}`.
//!
//! Listens on `127.0.0.1` only (47800..=47809, the first free port), checks the `Host` header of every request, rejects
//! any request with an `Origin` (a web page) and every CORS preflight, and wants `Authorization: Bearer <key>`.
//! Requests are parsed on the connection threads, queued, and answered on the host's message thread:
//! [`ControlServer::service`] (called from a timer there) runs the handler for each queued request. A request that is
//! not answered within 2 s gets a 503 and is dropped from the queue, so it is never run late.
//!
//! Plain std threads and sockets like `share` (R3.3); none of them real time. The routes are in [`crate::rest`].
//! Differences from the C++ (bugs fixed) are marked `C++ bug` and listed in the R3.4 ledger.
use crate::json::Json;
use crate::share::sock;
use crate::share::util::{ct_eq, secure_random};
use serde_json::Value;
use std::collections::VecDeque;
use std::net::{Ipv4Addr, TcpListener, TcpStream};
use std::sync::atomic::{AtomicBool, AtomicU16, Ordering};
use std::sync::{Arc, Condvar, Mutex, MutexGuard, PoisonError};
use std::thread::JoinHandle;
use std::time::{Duration, Instant};

/// The first port tried; the next 9 follow.
pub const FIRST_PORT: u16 = 47800;
pub const PORT_COUNT: u16 = 10;
/// connections served at once; more are closed
const MAX_CONNS: usize = 8;
const MAX_HEAD: usize = 8192;
const MAX_BODY: usize = 16384;
/// requests waiting for the message thread (at most one per connection, this is a second fence)
const MAX_QUEUE: usize = 16;
/// time for the whole request head, and again for the whole body: one byte now and then does not keep a place
const IO_TIMEOUT: Duration = Duration::from_secs(5);
/// the message thread has this long for a request
const ANSWER_TIMEOUT: Duration = Duration::from_secs(2);
/// a request the message thread already started gets this long on top to finish
const RUNNING_GRACE: Duration = Duration::from_secs(2);

/// One request: `method` is upper case, `path` has no query. `body` is `Value::Null` when there is none.
#[derive(Clone, Debug)]
pub struct Request {
    pub method: String,
    pub path: String,
    pub body: Value,
}

#[derive(Clone, Debug, PartialEq)]
pub struct Response {
    pub status: u16,
    pub body: Json,
}

impl Response {
    pub fn new(status: u16, body: Json) -> Self {
        Response { status, body }
    }

    pub fn error(status: u16, message: &str) -> Self {
        Response::new(status, Json::error(message))
    }
}

fn lock<T>(m: &Mutex<T>) -> MutexGuard<'_, T> {
    m.lock().unwrap_or_else(PoisonError::into_inner)
}

// ---- queued requests ---------------------------------------------------------------------------------------------

enum JobState {
    Queued,
    /// the message thread is running the handler
    Running,
    /// nobody waits for the answer any more: never run
    Abandoned,
    Done(Response),
}

struct Job {
    request: Request,
    state: Mutex<JobState>,
    cv: Condvar,
}

/// If the handler panics the connection must not wait for an answer that never comes.
struct Finish<'a> {
    job: &'a Job,
    done: bool,
}

impl Finish<'_> {
    fn set(&mut self, r: Response) {
        self.done = true;
        *lock(&self.job.state) = JobState::Done(r);
        self.job.cv.notify_all();
    }
}

impl Drop for Finish<'_> {
    fn drop(&mut self) {
        if !self.done {
            self.set(Response::error(503, "the Hub failed to answer"));
        }
    }
}

/// What one `start()` shares with its threads.
struct Ctx {
    key: String,
    port: u16,
    alive: AtomicBool,
    queue: Mutex<VecDeque<Arc<Job>>>,
    answer_timeout: Duration,
    io_timeout: Duration,
}

impl Ctx {
    fn alive(&self) -> bool {
        self.alive.load(Ordering::SeqCst)
    }

    /// Queues the request and waits for the message thread (or the time limit).
    fn submit(&self, request: Request) -> Response {
        let job = Arc::new(Job {
            request,
            state: Mutex::new(JobState::Queued),
            cv: Condvar::new(),
        });
        {
            let mut q = lock(&self.queue);
            if !self.alive() {
                return Response::error(503, "the Hub is closing");
            }
            if q.len() >= MAX_QUEUE {
                // C++ bug: the queue had no limit (a stalled message thread let it grow with every retry)
                return Response::error(503, "the DAW did not answer (busy?)");
            }
            q.push_back(job.clone());
        }
        let deadline = Instant::now() + self.answer_timeout;
        let mut st = lock(&job.state);
        loop {
            match &*st {
                JobState::Done(r) => return r.clone(),
                JobState::Abandoned => {
                    return Response::error(503, "the DAW did not answer (busy?)");
                }
                JobState::Queued | JobState::Running => {}
            }
            let queued = matches!(*st, JobState::Queued);
            let limit = if queued {
                deadline
            } else {
                deadline + RUNNING_GRACE
            };
            let now = Instant::now();
            if now >= limit || (queued && !self.alive()) {
                if queued {
                    // C++ bug: the job stayed in the queue and ran when the message thread came back, after the
                    // client had its 503 (a retried toggle then toggled twice)
                    *st = JobState::Abandoned;
                    drop(st);
                    lock(&self.queue).retain(|j| !Arc::ptr_eq(j, &job));
                    return if self.alive() {
                        Response::error(503, "the DAW did not answer (busy?)")
                    } else {
                        Response::error(503, "the Hub is closing")
                    };
                }
                return Response::error(503, "the DAW did not answer (busy?)");
            }
            let wait = (limit - now).min(Duration::from_millis(50));
            st = job
                .cv
                .wait_timeout(st, wait)
                .unwrap_or_else(PoisonError::into_inner)
                .0;
        }
    }
}

// ---- the server --------------------------------------------------------------------------------------------------

struct Conn {
    done: Arc<AtomicBool>,
    /// shut down by `stop()` to wake the thread; the descriptor is released only after the thread was joined
    wake: TcpStream,
    thread: Option<JoinHandle<()>>,
}

struct Run {
    ctx: Arc<Ctx>,
    accept: Option<JoinHandle<()>>,
    conns: Arc<Mutex<Vec<Conn>>>,
}

pub struct ControlServer {
    ports: Vec<u16>,
    answer_timeout: Duration,
    io_timeout: Duration,
    run: Mutex<Option<Run>>,
    /// what `service` needs, without waiting for a `stop()` that joins threads
    current: Mutex<Option<Arc<Ctx>>>,
    running: AtomicBool,
    port: AtomicU16,
}

impl Default for ControlServer {
    fn default() -> Self {
        Self::new()
    }
}

impl Drop for ControlServer {
    fn drop(&mut self) {
        self.stop();
    }
}

impl ControlServer {
    pub fn new() -> Self {
        ControlServer {
            ports: (FIRST_PORT..FIRST_PORT + PORT_COUNT).collect(),
            answer_timeout: ANSWER_TIMEOUT,
            io_timeout: IO_TIMEOUT,
            run: Mutex::new(None),
            current: Mutex::new(None),
            running: AtomicBool::new(false),
            port: AtomicU16::new(0),
        }
    }

    /// Tests: other ports to try in order (0 = any free port), so tests running side by side do not meet.
    #[doc(hidden)]
    pub fn with_ports(ports: &[u16]) -> Self {
        let mut s = Self::new();
        s.ports = ports.to_vec();
        s
    }

    /// Tests: shorter limits for "not answered" and for a request that is sent too slowly.
    #[doc(hidden)]
    pub fn with_timeouts(mut self, answer: Duration, io: Duration) -> Self {
        self.answer_timeout = answer;
        self.io_timeout = io;
        self
    }

    /// 32 hex characters (128 bits) from the OS's secure random source; "" if it fails.
    pub fn new_key() -> String {
        let mut bytes = [0u8; 16];
        if !secure_random(&mut bytes) {
            return String::new();
        }
        bytes.iter().map(|b| format!("{b:02x}")).collect()
    }

    /// Message thread. Binds the first free port and starts serving with this API key (at least 16 characters).
    /// `false` = key too short or no free port. Calling it again restarts with the new key.
    pub fn start(&self, key: &str) -> bool {
        let mut run = lock(&self.run);
        self.stop_locked(&mut run);
        if key.chars().count() < 16 {
            return false;
        }
        sock::ignore_sigpipe();
        let Some((listener, port)) = self.ports.iter().find_map(|&p| {
            let l = TcpListener::bind((Ipv4Addr::LOCALHOST, p)).ok()?; // this computer only
            let port = l.local_addr().ok()?.port();
            Some((l, port))
        }) else {
            return false;
        };
        if listener.set_nonblocking(true).is_err() {
            return false;
        }
        // a process the DAW starts must not keep the listener open after we close it
        sock::no_inherit_listener(&listener);
        let ctx = Arc::new(Ctx {
            key: key.to_owned(),
            port,
            alive: AtomicBool::new(true),
            queue: Mutex::new(VecDeque::new()),
            answer_timeout: self.answer_timeout,
            io_timeout: self.io_timeout,
        });
        let conns = Arc::new(Mutex::new(Vec::new()));
        let (c2, k2) = (ctx.clone(), conns.clone());
        let Ok(accept) = std::thread::Builder::new()
            .name("control-accept".into())
            .spawn(move || accept_loop(&c2, &listener, &k2))
        else {
            return false;
        };
        *lock(&self.current) = Some(ctx.clone());
        self.port.store(port, Ordering::SeqCst);
        self.running.store(true, Ordering::SeqCst);
        *run = Some(Run {
            ctx,
            accept: Some(accept),
            conns,
        });
        true
    }

    /// Ends every thread and closes every socket; queued requests get a 503. Safe to call twice.
    pub fn stop(&self) {
        let mut run = lock(&self.run);
        self.stop_locked(&mut run);
    }

    fn stop_locked(&self, run: &mut Option<Run>) {
        let Some(mut r) = run.take() else { return };
        self.running.store(false, Ordering::SeqCst);
        *lock(&self.current) = None;
        r.ctx.alive.store(false, Ordering::SeqCst);
        if let Some(h) = r.accept.take() {
            let _ = h.join();
        }
        // nobody will answer these any more
        let queued: Vec<Arc<Job>> = lock(&r.ctx.queue).drain(..).collect();
        for job in queued {
            let mut st = lock(&job.state);
            if matches!(*st, JobState::Queued) {
                *st = JobState::Done(Response::error(503, "the Hub is closing"));
                job.cv.notify_all();
            }
        }
        // The connection threads leave by themselves (they look at `alive` every 100 ms): one that is answering a
        // queued request must finish writing its 503 before its socket is shut down, or the client gets nothing
        // (the flaky test). A thread that does not leave in time (a client that does not read) is shut down.
        let patience = Instant::now() + Duration::from_millis(500);
        while Instant::now() < patience
            && lock(&r.conns)
                .iter()
                .any(|c| !c.done.load(Ordering::SeqCst))
        {
            sock::sleep_ms(5);
        }
        // C++ bug: the sockets were closed while their threads still used them (the descriptor could be reused by
        // the next connection): shut down to wake them, join, and only then release
        let entries: Vec<Conn> = std::mem::take(&mut *lock(&r.conns));
        for e in &entries {
            sock::shutdown(&e.wake);
        }
        for mut e in entries {
            if let Some(h) = e.thread.take() {
                let _ = h.join();
            }
        }
        self.port.store(0, Ordering::SeqCst);
    }

    pub fn running(&self) -> bool {
        self.running.load(Ordering::SeqCst)
    }

    /// 0 when not running.
    pub fn port(&self) -> u16 {
        self.port.load(Ordering::SeqCst)
    }

    pub fn base_url(&self) -> String {
        format!("http://127.0.0.1:{}", self.port())
    }

    /// Requests waiting for `service` (tests).
    #[doc(hidden)]
    pub fn pending(&self) -> usize {
        lock(&self.current)
            .as_ref()
            .map_or(0, |c| lock(&c.queue).len())
    }

    /// Message thread: answers the queued requests with `handler`. Never waits for a connection.
    pub fn service(&self, mut handler: impl FnMut(&Request) -> Response) {
        let Some(ctx) = lock(&self.current).clone() else {
            return;
        };
        let jobs: VecDeque<Arc<Job>> = std::mem::take(&mut *lock(&ctx.queue));
        for job in jobs {
            {
                let mut st = lock(&job.state);
                if !matches!(*st, JobState::Queued) {
                    continue; // its client gave up
                }
                *st = JobState::Running;
            }
            let mut finish = Finish {
                job: &job,
                done: false,
            };
            let answer = handler(&job.request);
            finish.set(answer);
        }
    }
}

// ---- connections -------------------------------------------------------------------------------------------------

fn reap(conns: &Mutex<Vec<Conn>>) -> usize {
    let finished: Vec<Conn> = {
        let mut list = lock(conns);
        let (done, keep): (Vec<Conn>, Vec<Conn>) = std::mem::take(&mut *list)
            .into_iter()
            .partition(|c| c.done.load(Ordering::SeqCst));
        *list = keep;
        done
    };
    for mut c in finished {
        if let Some(h) = c.thread.take() {
            let _ = h.join();
        }
    }
    lock(conns).len()
}

struct DoneGuard(Arc<AtomicBool>);

impl Drop for DoneGuard {
    fn drop(&mut self) {
        self.0.store(true, Ordering::SeqCst);
    }
}

fn accept_loop(ctx: &Arc<Ctx>, listener: &TcpListener, conns: &Arc<Mutex<Vec<Conn>>>) {
    while ctx.alive() {
        // polled, not blocked in accept(): closing a listener does not wake accept() on Linux (C++ bug)
        if !sock::listener_readable(listener, 50) {
            reap(conns);
            continue;
        }
        let Ok((stream, _)) = listener.accept() else {
            sock::sleep_ms(20);
            continue;
        };
        if !ctx.alive() {
            break;
        }
        let _ = stream.set_nonblocking(true);
        let _ = stream.set_nodelay(true);
        sock::no_inherit_stream(&stream);
        sock::no_sigpipe(&stream);
        if reap(conns) >= MAX_CONNS {
            continue; // refused: `stream` closes
        }
        let Ok(wake) = stream.try_clone() else {
            continue;
        };
        let done = Arc::new(AtomicBool::new(false));
        let (c2, d2) = (ctx.clone(), done.clone());
        let spawned = std::thread::Builder::new()
            .name("control-conn".into())
            .spawn(move || {
                let _done = DoneGuard(d2);
                serve(&c2, &stream);
                sock::shutdown(&stream);
            });
        if let Ok(h) = spawned {
            lock(conns).push(Conn {
                done,
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

/// The request head and the first bytes of what follows. The whole head must arrive within `timeout` (C++ bug: the
/// 3 s counted from the last byte, so one byte every 2.9 s kept a connection for hours). Over 8 KiB is `TooLarge`.
fn read_head(
    s: &TcpStream,
    alive: &dyn Fn() -> bool,
    timeout: Duration,
) -> Result<(Vec<u8>, Vec<u8>), HeadError> {
    let deadline = Instant::now() + timeout;
    let mut data: Vec<u8> = Vec::with_capacity(1024);
    let mut buf = [0u8; 2048];
    loop {
        if let Some(p) = data.windows(4).position(|w| w == b"\r\n\r\n") {
            if p + 4 > MAX_HEAD {
                return Err(HeadError::TooLarge);
            }
            let rest = data.split_off(p + 4);
            data.truncate(p);
            return Ok((data, rest));
        }
        if data.len() >= MAX_HEAD {
            return Err(HeadError::TooLarge);
        }
        if !alive() || Instant::now() >= deadline {
            return Err(HeadError::Gone);
        }
        if sock::wait_readable(s, 100) < 0 {
            return Err(HeadError::Gone);
        }
        let got = sock::try_read(s, &mut buf);
        if got < 0 {
            return Err(HeadError::Gone);
        }
        data.extend_from_slice(&buf[..got as usize]);
    }
}

/// `body` gets `want` bytes in total, all of them within `timeout`.
fn read_body(
    s: &TcpStream,
    body: &mut Vec<u8>,
    want: usize,
    alive: &dyn Fn() -> bool,
    timeout: Duration,
) -> bool {
    let deadline = Instant::now() + timeout;
    let mut buf = [0u8; 2048];
    while body.len() < want {
        if !alive() || Instant::now() >= deadline {
            return false;
        }
        if sock::wait_readable(s, 100) < 0 {
            return false;
        }
        let n = (want - body.len()).min(buf.len());
        let got = sock::try_read(s, &mut buf[..n]);
        if got < 0 {
            return false;
        }
        body.extend_from_slice(&buf[..got as usize]);
    }
    true
}

/// Throws away what the client is still sending (briefly), so closing does not reset the connection and the answer
/// we just wrote is not lost on the way.
fn drain(s: &TcpStream, alive: &dyn Fn() -> bool) {
    let mut buf = [0u8; 4096];
    for _ in 0..20 {
        if !alive() || sock::wait_readable(s, 50) != 1 || sock::try_read(s, &mut buf) <= 0 {
            break;
        }
    }
}

fn reason(status: u16) -> &'static str {
    match status {
        200 => "OK",
        400 => "Bad Request",
        401 => "Unauthorized",
        403 => "Forbidden",
        404 => "Not Found",
        405 => "Method Not Allowed",
        413 => "Payload Too Large",
        431 => "Request Header Fields Too Large",
        _ => "Service Unavailable",
    }
}

/// The whole answer for a status and a JSON body, as the C++ wrote it.
fn response_bytes(status: u16, body: &Json) -> Vec<u8> {
    let text = body.to_juce_string() + "\n";
    let mut out = format!(
        "HTTP/1.1 {status} {}\r\nContent-Type: application/json; charset=utf-8\r\nContent-Length: {}\r\nCache-Control: no-store\r\nConnection: close\r\nX-Content-Type-Options: nosniff\r\n\r\n",
        reason(status),
        text.len()
    )
    .into_bytes();
    out.extend_from_slice(text.as_bytes());
    out
}

fn respond(s: &TcpStream, status: u16, body: &Json) -> bool {
    sock::write_all(s, &response_bytes(status, body))
}

/// Answers and discards the rest of the request.
fn reject(s: &TcpStream, alive: &dyn Fn() -> bool, status: u16, message: &str) {
    respond(s, status, &Json::error(message));
    drain(s, alive);
}

/// What the request head says that matters here. Headers that may only appear once are collected as lists so a
/// repeated one can be told from a single one.
#[derive(Debug, Default)]
struct Head {
    /// as sent (the case of `OPTIONS` is not up to the client)
    method: String,
    path: String,
    hosts: Vec<String>,
    /// an `Origin` header of any value, also an empty one
    origin: bool,
    auth: Vec<String>,
    lengths: Vec<String>,
    chunked: bool,
}

/// `None` = no request line with a method and a target.
fn parse_head(text: &str) -> Option<Head> {
    let mut lines = text.lines();
    let mut tokens = lines.next()?.split(' ').filter(|t| !t.is_empty());
    let method = tokens.next()?.to_owned();
    let target = tokens.next()?;
    let mut h = Head {
        method,
        path: target.split('?').next().unwrap_or("").to_owned(),
        ..Head::default()
    };
    for l in lines {
        let Some((name, value)) = l.split_once(':') else {
            continue;
        };
        let value = value.trim().to_owned();
        match name.to_ascii_lowercase().as_str() {
            "host" => h.hosts.push(value),
            "origin" => h.origin = true,
            "authorization" => h.auth.push(value),
            "content-length" => h.lengths.push(value),
            "transfer-encoding" => {
                h.chunked |= value
                    .to_ascii_lowercase()
                    .split(',')
                    .any(|t| t.trim() == "chunked");
            }
            _ => {}
        }
    }
    Some(h)
}

/// Only `127.0.0.1:<port>` and `localhost:<port>` (any case), sent once. Another name (DNS rebinding) or none is no.
/// C++ bug: with two `Host` headers the last one counted.
fn host_ok(hosts: &[String], port: u16) -> bool {
    let [host] = hosts else { return false };
    let host = host.trim().to_ascii_lowercase();
    host == format!("127.0.0.1:{port}") || host == format!("localhost:{port}")
}

/// The key of `Bearer <key>`: the scheme must be the first word (C++ bug: "Bearer " was searched anywhere in the
/// value, so `Basic x Bearer <key>` passed).
fn bearer_token(value: &str) -> Option<&str> {
    let v = value.trim_start();
    let (scheme, rest) = v.split_at_checked(6)?;
    if !scheme.eq_ignore_ascii_case("bearer") || !rest.starts_with([' ', '\t']) {
        return None;
    }
    Some(rest.trim())
}

fn auth_ok(auth: &[String], key: &str) -> bool {
    let [value] = auth else { return false };
    bearer_token(value).is_some_and(|t| !t.is_empty() && ct_eq(t.as_bytes(), key.as_bytes()))
}

enum LengthError {
    /// not a plain number, repeated differently, or a body without a length
    Bad,
    TooBig,
}

/// The body size. C++ bug: `getLargeIntValue` made "abc" 0, "12abc" 12, wrapped on overflow, and took the last of
/// several `Content-Length` headers: a plain unsigned number is required, repeats must agree, no chunked bodies.
fn content_length(h: &Head) -> Result<usize, LengthError> {
    if h.chunked {
        return Err(LengthError::Bad);
    }
    let mut length: Option<u64> = None;
    for v in &h.lengths {
        if v.is_empty() || !v.bytes().all(|b| b.is_ascii_digit()) {
            return Err(LengthError::Bad);
        }
        let n = v.parse::<u64>().map_err(|_| LengthError::TooBig)?;
        if length.is_some_and(|l| l != n) {
            return Err(LengthError::Bad);
        }
        length = Some(n);
    }
    let n = length.unwrap_or(0);
    if n > MAX_BODY as u64 {
        return Err(LengthError::TooBig);
    }
    Ok(n as usize)
}

/// A JSON object or array like `juce::JSON::parse` (a number or a string alone is not a body). Nothing but white
/// space is "no body".
fn parse_body(bytes: &[u8]) -> Option<Value> {
    if bytes.iter().all(u8::is_ascii_whitespace) {
        return Some(Value::Null);
    }
    serde_json::from_slice::<Value>(bytes)
        .ok()
        .filter(|v| v.is_object() || v.is_array())
}

fn serve(ctx: &Ctx, s: &TcpStream) {
    let alive = || ctx.alive();
    let (head, mut body) = match read_head(s, &alive, ctx.io_timeout) {
        Ok(v) => v,
        Err(HeadError::Gone) => return,
        Err(HeadError::TooLarge) => {
            reject(s, &alive, 431, "the request header is too large");
            return;
        }
    };
    let text = String::from_utf8_lossy(&head);
    let Some(h) = parse_head(&text) else {
        reject(s, &alive, 400, "bad request");
        return;
    };
    // only programs on this computer: a web page (Origin, CORS preflight) or another host name (DNS rebinding)
    // never gets in. C++ bug: an empty `Origin:` passed, and `options` in lower case was not a preflight.
    if !host_ok(&h.hosts, ctx.port) || h.origin || h.method.eq_ignore_ascii_case("OPTIONS") {
        reject(
            s,
            &alive,
            403,
            "only programs on this computer may use the HEARASIDE API",
        );
        return;
    }
    if !auth_ok(&h.auth, &ctx.key) {
        reject(
            s,
            &alive,
            401,
            "missing or wrong API key (Hub settings > REST API)",
        );
        return;
    }
    let length = match content_length(&h) {
        Ok(n) => n,
        Err(LengthError::Bad) => {
            reject(s, &alive, 400, "bad Content-Length");
            return;
        }
        Err(LengthError::TooBig) => {
            reject(s, &alive, 413, "request too large");
            return;
        }
    };
    // exactly `length` bytes: C++ bug: whatever came after the head was the body, with or without a length
    body.truncate(length);
    if !read_body(s, &mut body, length, &alive, ctx.io_timeout) {
        return;
    }
    let Some(value) = parse_body(&body) else {
        reject(s, &alive, 400, "the body is not JSON");
        return;
    };
    let answer = ctx.submit(Request {
        method: h.method.to_uppercase(),
        path: h.path,
        body: value,
    });
    respond(s, answer.status, &answer.body);
}

#[cfg(test)]
mod tests {
    use super::*;

    fn head(h: &str) -> Head {
        parse_head(h).unwrap()
    }

    #[test]
    fn request_line_and_headers() {
        let h = head(
            "post /api/v1/mute?x=1 HTTP/1.1\r\nHost: A\r\nhost: B\r\nORIGIN:\r\nAuthorization: Bearer k\r\nContent-length: 5",
        );
        assert_eq!(h.method, "post");
        assert_eq!(h.path, "/api/v1/mute");
        assert_eq!(h.hosts, ["A", "B"]);
        assert!(h.origin);
        assert_eq!(h.auth, ["Bearer k"]);
        assert_eq!(h.lengths, ["5"]);
        for (te, chunked) in [
            ("identity", false),
            ("chunked", true),
            ("CHUNKED", true),
            ("gzip, Chunked", true),
            ("gzip", false),
            ("notchunked", false),
        ] {
            let h = head(&format!("POST /x HTTP/1.1\r\nTransfer-Encoding: {te}"));
            assert_eq!(h.chunked, chunked, "{te}");
        }
        assert!(parse_head("GET").is_none());
        assert!(parse_head("").is_none());
        // repeated spaces are fine; a header with a space before the colon is not the header
        let h = head("GET   /x   HTTP/1.1\r\nHost : evil");
        assert_eq!(h.path, "/x");
        assert!(h.hosts.is_empty());
    }

    #[test]
    fn host_rules() {
        let ok = |h: &[&str]| {
            host_ok(
                &h.iter().map(|s| (*s).to_owned()).collect::<Vec<_>>(),
                47800,
            )
        };
        assert!(ok(&["127.0.0.1:47800"]));
        assert!(ok(&["LocalHost:47800"]));
        assert!(ok(&["  127.0.0.1:47800 "]));
        assert!(!ok(&[]));
        assert!(!ok(&["127.0.0.1"]));
        assert!(!ok(&["127.0.0.1:47801"]));
        assert!(!ok(&["[::1]:47800"]));
        assert!(!ok(&["localhost.:47800"]));
        assert!(!ok(&["evil.example:47800"]));
        assert!(!ok(&["127.0.0.1:47800.evil.example"]));
        assert!(!ok(&["127.0.0.1:47800", "evil.example"]));
        assert!(!ok(&["evil.example", "127.0.0.1:47800"]));
    }

    #[test]
    fn bearer_rules() {
        assert_eq!(bearer_token("Bearer abc"), Some("abc"));
        assert_eq!(bearer_token("bearer   abc  "), Some("abc"));
        assert_eq!(bearer_token("BEARER\tabc"), Some("abc"));
        assert_eq!(bearer_token("Basic x Bearer abc"), None);
        assert_eq!(bearer_token("xBearer abc"), None);
        assert_eq!(bearer_token("Bearerabc"), None);
        assert_eq!(bearer_token("Bearer"), None);
        assert_eq!(bearer_token("ก"), None);
        let a = |v: &[&str]| {
            auth_ok(
                &v.iter().map(|s| (*s).to_owned()).collect::<Vec<_>>(),
                "key0123456789abc",
            )
        };
        assert!(a(&["Bearer key0123456789abc"]));
        assert!(!a(&["Bearer key0123456789abd"]));
        assert!(!a(&["Bearer "]));
        assert!(!a(&[]));
        assert!(!a(&["Bearer key0123456789abc", "Bearer key0123456789abc"]));
    }

    #[test]
    fn content_length_rules() {
        let len = |l: &[&str], chunked: bool| {
            content_length(&Head {
                lengths: l.iter().map(|s| (*s).to_owned()).collect(),
                chunked,
                ..Head::default()
            })
        };
        assert!(matches!(len(&[], false), Ok(0)));
        assert!(matches!(len(&["0"], false), Ok(0)));
        assert!(matches!(len(&["16384"], false), Ok(16384)));
        assert!(matches!(len(&["16385"], false), Err(LengthError::TooBig)));
        assert!(matches!(
            len(&["99999999999999999999999"], false),
            Err(LengthError::TooBig)
        ));
        for bad in ["", "-1", "+5", "12abc", "abc", "1 2", "0x10"] {
            assert!(matches!(len(&[bad], false), Err(LengthError::Bad)), "{bad}");
        }
        assert!(matches!(len(&["5", "5"], false), Ok(5)));
        assert!(matches!(len(&["5", "6"], false), Err(LengthError::Bad)));
        assert!(matches!(len(&[], true), Err(LengthError::Bad)));
    }

    #[test]
    fn bodies_are_objects_or_arrays() {
        assert_eq!(parse_body(b""), Some(Value::Null));
        assert_eq!(parse_body(b" \r\n"), Some(Value::Null));
        assert!(parse_body(b"{\"on\": true}").is_some_and(|v| v.is_object()));
        assert!(parse_body(b"[1]").is_some_and(|v| v.is_array()));
        for bad in [
            &b"null"[..],
            b"5",
            b"\"x\"",
            b"true",
            b"nonsense",
            b"{",
            b"{} x",
            b"\xff",
        ] {
            assert!(parse_body(bad).is_none(), "{bad:?}");
        }
    }

    #[test]
    fn responses_have_the_c_plus_plus_head_and_json_text() {
        let b = response_bytes(401, &Json::error("nope"));
        assert_eq!(
            String::from_utf8(b).unwrap(),
            "HTTP/1.1 401 Unauthorized\r\nContent-Type: application/json; charset=utf-8\r\nContent-Length: 18\r\nCache-Control: no-store\r\nConnection: close\r\nX-Content-Type-Options: nosniff\r\n\r\n{\"error\": \"nope\"}\n"
        );
        assert_eq!(reason(405), "Method Not Allowed");
        assert_eq!(reason(413), "Payload Too Large");
        assert_eq!(reason(431), "Request Header Fields Too Large");
        assert_eq!(reason(503), "Service Unavailable");
    }

    #[test]
    fn keys_are_32_hex_characters() {
        let (a, b) = (ControlServer::new_key(), ControlServer::new_key());
        assert_eq!(a.len(), 32);
        assert!(
            a.bytes()
                .all(|c| c.is_ascii_hexdigit() && !c.is_ascii_uppercase())
        );
        assert_ne!(a, b);
    }
}
