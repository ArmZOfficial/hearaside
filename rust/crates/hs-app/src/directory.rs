//! Permanent share links, the Hub's side (`plugins/hub/ShareDirectory.{h,cpp}`): tells the share web site
//! (`web/share-vercel`) where this Hub's cloudflared tunnel is, so `https://<site>/l/<token>` keeps working when the tunnel
//! address changes. Only the address is sent; the audio goes straight from the Hub to the listener.
//!
//! `POST /api/register {secret, tokens: {l, s, f: [..]}, tunnel}` is also the heartbeat (the site forgets an address after
//! 90 s, the Hub registers every 30 s); `POST /api/unregister` takes the links offline when sharing stops. The `secret`
//! (64 hex, `ShareServer::new_secret`) proves who owns the tokens. All network calls run on one thread of their own;
//! nothing here waits on the caller's thread.
use crate::http::{Client, Reply, origin_of, secure_enough};
use crate::sync::Relock;
use serde_json::{Value, json};
use std::sync::atomic::{AtomicBool, AtomicU8, AtomicU32, Ordering};
use std::sync::{Arc, Condvar, Mutex};
use std::time::{Duration, Instant};

#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub enum DirectoryState {
    Off,
    Registering,
    Online,
    Unreachable,
}

impl DirectoryState {
    fn from_u8(v: u8) -> Self {
        match v {
            1 => Self::Registering,
            2 => Self::Online,
            3 => Self::Unreachable,
            _ => Self::Off,
        }
    }
}

/// What the site should know.
#[derive(Clone, Debug, Default, PartialEq, Eq)]
struct Config {
    base: String,
    listen: String,
    send: String,
    secret: String,
    tunnel: String,
    friends: Vec<String>,
}

/// What the share site says about a permanent link (`resolveLink` of App Audio's `LinkReceiver`).
#[derive(Clone, Debug, PartialEq, Eq)]
pub enum Resolved {
    /// the Hub is up: the WebSocket address (`wss://<tunnel>/ws/l/<token>`)
    Online(String),
    /// the directory says "not shared right now"
    Offline,
    /// no directory answered there (a Hub's own link, or the site is down)
    NoDirectory,
}

type Bearer = Arc<dyn Fn(&str) -> String + Send + Sync>;

#[derive(Default)]
struct Guarded {
    wanted: Config,
    registered: Config,
    registered_valid: bool,
    unregister: bool,
    wake: bool,
    bearer: Option<Bearer>,
    revoked: Vec<String>,
}

struct Shared {
    g: Mutex<Guarded>,
    cv: Condvar,
    state: AtomicU8,
    stop: AtomicBool,
    heartbeat_ms: AtomicU32,
    retry_ms: AtomicU32,
    client: Client,
}

/// Registers the Hub's links on the share site from a background thread. Dropping it returns within about a quarter of a
/// second even when the site hangs; it does not unregister (call `stop` first for that).
///
/// The thread checks for the stop before every call; a call already in flight has no cancel and ends within its 8 s
/// deadline. If the thread outlasts the drop, this module is pinned in memory first (see `AccountManager`), so the host
/// unloading the plug-in cannot pull the code from under it. Do not drop it under the operating system's loader lock
/// (`DllMain`, a static destructor of the module).
pub struct ShareDirectory {
    shared: Arc<Shared>,
    thread: Mutex<Option<std::thread::JoinHandle<()>>>,
    alive: Arc<std::sync::atomic::AtomicBool>,
}

impl Default for ShareDirectory {
    fn default() -> Self {
        Self::new()
    }
}

impl ShareDirectory {
    pub fn new() -> Self {
        Self {
            shared: Arc::new(Shared {
                g: Mutex::new(Guarded::default()),
                cv: Condvar::new(),
                state: AtomicU8::new(0),
                stop: AtomicBool::new(false),
                heartbeat_ms: AtomicU32::new(30_000),
                retry_ms: AtomicU32::new(2_000),
                client: Client::new(Duration::from_secs(5), Duration::from_secs(8), 64 * 1024),
            }),
            thread: Mutex::new(None),
            alive: Arc::new(AtomicBool::new(false)),
        }
    }

    /// `base`: `"https://hearaside.vercel.app"` (empty = no permanent links). `tunnel`: the Hub's https tunnel (empty =
    /// none: the links go offline). `friends`: every friend's send-in token, registered like the send token.
    pub fn update(
        &self,
        base: &str,
        listen_token: &str,
        send_token: &str,
        secret: &str,
        tunnel: &str,
        friends: &[String],
    ) {
        let c = Config {
            base: base.trim_end_matches('/').to_owned(),
            listen: listen_token.to_owned(),
            send: send_token.to_owned(),
            secret: secret.to_owned(),
            tunnel: tunnel.to_owned(),
            friends: friends.to_vec(),
        };
        {
            let mut g = self.shared.g.relock();
            if c == g.wanted && !g.unregister {
                return;
            }
            g.wanted = c;
            g.unregister = false;
            g.wake = true;
        }
        let mut t = self.thread.relock();
        if t.is_none() {
            let shared = Arc::clone(&self.shared);
            let alive = Arc::clone(&self.alive);
            alive.store(true, Ordering::SeqCst);
            *t = std::thread::Builder::new()
                .name("HEARASIDE share directory".into())
                .spawn(move || {
                    crate::sync::keep_running(
                        || shared.stop.load(Ordering::SeqCst),
                        || run(&shared),
                    );
                    alive.store(false, Ordering::SeqCst);
                })
                .ok();
        }
        self.shared.cv.notify_all();
    }

    /// The account's access token for a base (empty = none): the site then lists the links in Account > Links and
    /// friends. Only ever given for the account's own server; and never sent in clear text to another computer.
    pub fn set_bearer(&self, f: impl Fn(&str) -> String + Send + Sync + 'static) {
        self.shared.g.relock().bearer = Some(Arc::new(f));
    }

    /// Tokens the site said were revoked on the account website (the caller makes new ones).
    pub fn take_revoked(&self) -> Vec<String> {
        std::mem::take(&mut self.shared.g.relock().revoked)
    }

    /// Sharing switched off: unregister (best effort, on the thread).
    pub fn stop(&self) {
        {
            let mut g = self.shared.g.relock();
            if g.wanted.tunnel.is_empty() && !g.registered_valid {
                return;
            }
            g.wanted.tunnel.clear();
            g.unregister = true;
            g.wake = true;
        }
        self.shared.cv.notify_all();
    }

    pub fn state(&self) -> DirectoryState {
        DirectoryState::from_u8(self.shared.state.load(Ordering::SeqCst))
    }

    /// `base/l/<token>`; empty unless a base and a token are set.
    pub fn listen_url(&self) -> String {
        let g = self.shared.g.relock();
        if g.wanted.base.is_empty() || g.wanted.listen.is_empty() {
            String::new()
        } else {
            format!("{}/l/{}", g.wanted.base, g.wanted.listen)
        }
    }

    pub fn send_url(&self) -> String {
        let g = self.shared.g.relock();
        if g.wanted.base.is_empty() || g.wanted.send.is_empty() {
            String::new()
        } else {
            format!("{}/s/{}", g.wanted.base, g.wanted.send)
        }
    }

    /// `scheme://host` of the base, for the WebSocket Origin check.
    pub fn origin(&self) -> String {
        origin_of(&self.shared.g.relock().wanted.base)
    }

    /// `GET <base>/api/resolve/<token>`: where a permanent link points right now. Blocking, at most a few seconds, for the
    /// caller's own thread. Only an https site (or this computer) counts as a directory, and a token that is not
    /// `[A-Za-z0-9_-]` is never put in the path (the C++ appended whatever it was given).
    pub fn resolve(base: &str, token: &str) -> Resolved {
        let base = base.trim_end_matches('/');
        if !secure_enough(base) {
            return Resolved::NoDirectory;
        }
        if token.is_empty()
            || !token
                .bytes()
                .all(|b| b.is_ascii_alphanumeric() || b == b'-' || b == b'_')
        {
            return Resolved::Offline;
        }
        let client = Client::new(Duration::from_secs(3), Duration::from_secs(4), 8192);
        let v = client
            .json("GET", &format!("{base}/api/resolve/{token}"), None, "", 0)
            .json();
        match v.get("online") {
            None => Resolved::NoDirectory,
            Some(Value::Bool(true)) => match v.get("ws").and_then(Value::as_str) {
                Some(ws) if ws.starts_with("wss://") => Resolved::Online(ws.to_owned()), // only secure tunnels are handed out
                _ => Resolved::Offline,
            },
            Some(_) => Resolved::Offline,
        }
    }

    pub fn origin_of(url: &str) -> String {
        origin_of(url)
    }

    /// Tests: turns false when the worker thread has ended.
    #[doc(hidden)]
    pub fn worker_alive_flag(&self) -> Arc<AtomicBool> {
        Arc::clone(&self.alive)
    }

    /// Tests: heartbeat interval and first retry delay (ms).
    pub fn set_timings(&self, heartbeat_ms: u32, retry_ms: u32) {
        self.shared
            .heartbeat_ms
            .store(heartbeat_ms, Ordering::SeqCst);
        self.shared.retry_ms.store(retry_ms, Ordering::SeqCst);
    }
}

impl Drop for ShareDirectory {
    fn drop(&mut self) {
        self.shared.stop.store(true, Ordering::SeqCst);
        self.shared.cv.notify_all();
        let handle = self.thread.relock().take();
        if let Some(h) = handle {
            // A request in flight cannot be cancelled: wait a little, then let the thread end on its own (it holds only
            // `Shared`, and its call has a deadline). The DAW never waits on the network.
            let until = Instant::now() + Duration::from_millis(250);
            while !h.is_finished() && Instant::now() < until {
                std::thread::sleep(Duration::from_millis(5));
            }
            if h.is_finished() {
                let _ = h.join();
            } else {
                crate::pin::pin_module_for_detached_thread();
            }
        }
    }
}

/// The body of register / unregister (`body()` in the C++).
fn body(secret: &str, listen: &str, send: &str, tunnel: &str, friends: &[String]) -> Value {
    let mut tokens = serde_json::Map::new();
    if !listen.is_empty() {
        tokens.insert("l".into(), json!(listen));
    }
    if !send.is_empty() {
        tokens.insert("s".into(), json!(send));
    }
    if !friends.is_empty() {
        tokens.insert("f".into(), json!(friends));
    }
    let mut o = serde_json::Map::new();
    o.insert("secret".into(), json!(secret));
    o.insert("tokens".into(), Value::Object(tokens));
    if !tunnel.is_empty() {
        o.insert("tunnel".into(), json!(tunnel));
    }
    Value::Object(o)
}

fn post(shared: &Shared, base: &str, path: &str, body: &Value) -> Reply {
    if shared.stop.load(Ordering::SeqCst) || !secure_enough(base) {
        return Reply::default();
    }
    let bearer = shared.g.relock().bearer.clone();
    let token = bearer.map(|f| f(base)).unwrap_or_default();
    shared
        .client
        .json("POST", &format!("{base}{path}"), Some(body), &token, 0)
}

/// Waits up to `ms` (`None` = until woken); returns early on a wake-up or on stop.
fn wait(shared: &Shared, ms: Option<u64>) {
    let mut g = shared.g.relock();
    let deadline = ms.map(|m| Instant::now() + Duration::from_millis(m));
    while !g.wake && !shared.stop.load(Ordering::SeqCst) {
        match deadline {
            None => {
                g = shared
                    .cv
                    .wait(g)
                    .unwrap_or_else(std::sync::PoisonError::into_inner)
            }
            Some(d) => {
                let now = Instant::now();
                if now >= d {
                    break;
                }
                g = shared
                    .cv
                    .wait_timeout(g, d - now)
                    .unwrap_or_else(std::sync::PoisonError::into_inner)
                    .0;
            }
        }
    }
    g.wake = false;
}

fn set_state(shared: &Shared, s: DirectoryState) {
    shared.state.store(s as u8, Ordering::SeqCst);
}

fn run(shared: &Shared) {
    let mut backoff = u64::from(shared.retry_ms.load(Ordering::SeqCst));
    while !shared.stop.load(Ordering::SeqCst) {
        let (want, unregister, registered, registered_valid) = {
            let mut g = shared.g.relock();
            g.wake = false; // anything that changes from here on wakes the next wait
            (
                g.wanted.clone(),
                g.unregister,
                g.registered.clone(),
                g.registered_valid,
            )
        };
        let active = !want.base.is_empty()
            && !want.tunnel.is_empty()
            && !want.secret.is_empty()
            && (!want.listen.is_empty() || !want.send.is_empty() || !want.friends.is_empty());
        if !active {
            if unregister && registered_valid {
                // sharing switched off: take the links offline now (best effort; the site forgets them after 90 s anyway)
                let b = body(
                    &registered.secret,
                    &registered.listen,
                    &registered.send,
                    "",
                    &registered.friends,
                );
                post(shared, &registered.base, "/api/unregister", &b);
                let mut g = shared.g.relock();
                g.unregister = false;
            }
            {
                let mut g = shared.g.relock();
                g.registered_valid = false;
            }
            set_state(shared, DirectoryState::Off);
            wait(shared, None);
            continue;
        }
        if !registered_valid || registered != want {
            set_state(shared, DirectoryState::Registering);
        }
        // registering again is also the heartbeat
        let reply = post(
            shared,
            &want.base,
            "/api/register",
            &body(
                &want.secret,
                &want.listen,
                &want.send,
                &want.tunnel,
                &want.friends,
            ),
        );
        if shared.stop.load(Ordering::SeqCst) {
            break;
        }
        if reply.status == 200 {
            // links revoked on the account website: the Hub makes new ones (take_revoked)
            let mut revoked = Vec::new();
            if let Some(r) = reply.json().get("revoked").and_then(Value::as_array) {
                revoked.extend(r.iter().filter_map(Value::as_str).map(str::to_owned));
            }
            {
                let mut g = shared.g.relock();
                for t in revoked {
                    if !g.revoked.contains(&t) {
                        g.revoked.push(t);
                    }
                }
                g.registered = want;
                g.registered_valid = true;
            }
            set_state(shared, DirectoryState::Online);
            backoff = u64::from(shared.retry_ms.load(Ordering::SeqCst));
            wait(
                shared,
                Some(u64::from(shared.heartbeat_ms.load(Ordering::SeqCst))),
            );
        } else {
            {
                let mut g = shared.g.relock();
                g.registered_valid = g.registered_valid && g.registered == want;
            }
            set_state(shared, DirectoryState::Unreachable);
            // 400 / 403 will not fix themselves quickly, and 429 means the site asked us to slow down: retry slowly
            let slow = matches!(reply.status, 400 | 403 | 429);
            wait(
                shared,
                Some(if slow { backoff.max(60_000) } else { backoff }),
            );
            backoff = (backoff * 2).min(60_000);
        }
    }
}
