//! The plug-in side of a HEARASIDE account (`plugins/common/AccountManager.{h,cpp}`): sign in with a code from the browser
//! (device authorization flow, RFC 8628 style), keep the tokens, show the profile, edit it, sign out.
//!
//! * One worker thread does all the network and disk work; no method here blocks on either.
//! * Tokens live in `HEARASIDE/account.dat` in the user's app-data folder, shared by every plug-in and every DAW of the
//!   user (C++ and Rust alike): DPAPI-protected JSON on Windows, owner-only JSON elsewhere (`vault.rs`). A refresh token
//!   works once, so a refresh takes the inter-process lock `HEARASIDE_account_refresh` and re-reads the file first.
//! * Callbacks: the C++ posts them to the message thread. Here the worker only queues `AccountEvent`s; the caller drains
//!   them on the thread it chooses with `dispatch_events` (calls the listeners) or `take_events`.
//! * The access token is for this account's own server (`server_base_url`) only: never send it anywhere else, and it is
//!   never sent over plain HTTP to another computer.
use crate::http::{Client, Reply, secure_enough};
use crate::paths::{Os, PathEnv, account_file_for};
use crate::properties::write_atomic;
use crate::settings::{Appearance, ThemeMode, env_or};
use crate::strings::Language;
use crate::sync::Relock;
use crate::vault;
use serde_json::{Value, json};
use std::collections::VecDeque;
use std::path::PathBuf;
use std::sync::atomic::{AtomicBool, Ordering};
use std::sync::{Arc, Condvar, Mutex};
use std::time::{Duration, Instant, SystemTime, UNIX_EPOCH};

const DEFAULT_SERVER: &str = "https://hearaside.netlify.app";
const LOCK_NAME: &str = "HEARASIDE_account_refresh";
const MAX_EVENTS: usize = 256;
const MAX_AVATAR_BYTES: u64 = 5 * 1024 * 1024;

#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub enum State {
    SignedOut,
    WaitingForCode,
    SignedIn,
    /// signed in, but the server cannot be reached right now
    Offline,
}

#[derive(Clone, Debug, PartialEq)]
pub struct Profile {
    pub id: String,
    pub email: String,
    pub handle: String,
    pub display_name: String,
    pub about: String,
    pub avatar_url: String,
    pub locale: String,
    pub hide_email: bool,
    pub sync_appearance: bool,
    pub device_name: String,
    pub plan: String,
}

impl Default for Profile {
    fn default() -> Self {
        Self {
            id: String::new(),
            email: String::new(),
            handle: String::new(),
            display_name: String::new(),
            about: String::new(),
            avatar_url: String::new(),
            locale: "en".into(),
            hide_email: true,
            sync_appearance: false,
            device_name: String::new(),
            plan: "free".into(),
        }
    }
}

/// The sign-in code the user types on the website. The secret half (`device_code`, which the plug-in polls with) is
/// kept inside the manager and never shown here, in events or in a Debug print.
#[derive(Clone, Debug, PartialEq)]
pub struct DeviceCode {
    pub user_code: String,
    pub verification_uri: String,
    pub verification_uri_complete: String,
    pub expires_in_sec: i32,
    pub interval_sec: i32,
    pub expired: bool,
}

impl Default for DeviceCode {
    fn default() -> Self {
        Self {
            user_code: String::new(),
            verification_uri: String::new(),
            verification_uri_complete: String::new(),
            expires_in_sec: 600,
            interval_sec: 5,
            expired: false,
        }
    }
}

/// Fields to change in the profile; `None` = leave as it is. Only `about` can be cleared (`Some("")`): the website wants a
/// display name of 1 to 40 characters and a username of `a-z 0-9 _`, 3 to 20 (`web/site/shared/schemas.ts`); `about` may be
/// empty, up to 160. Lengths count UTF-16 units, like the website. `AccountManager::update_profile` checks this first.
#[derive(Clone, Debug, Default, PartialEq, Eq)]
pub struct ProfilePatch {
    pub display_name: Option<String>,
    pub handle: Option<String>,
    pub about: Option<String>,
}

/// Which field of a `ProfilePatch` the website would refuse.
#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub enum ProfileError {
    /// empty after trimming, or over 40
    DisplayName,
    /// not `a-z 0-9 _`, 3 to 20 (the website lower-cases it)
    Handle,
    /// over 160
    About,
}

impl std::fmt::Display for ProfileError {
    fn fmt(&self, f: &mut std::fmt::Formatter<'_>) -> std::fmt::Result {
        f.write_str(match self {
            ProfileError::DisplayName => "display name must be 1 to 40 characters",
            ProfileError::Handle => "username must be a-z, 0-9 and _, 3 to 20 characters",
            ProfileError::About => "about must be at most 160 characters",
        })
    }
}

impl std::error::Error for ProfileError {}

impl ProfilePatch {
    /// The patch as the website takes it (display name trimmed, username lower-cased), or the field it would answer 400 for.
    fn checked(&self) -> Result<ProfilePatch, ProfileError> {
        let display_name = match &self.display_name {
            Some(n) => {
                let n = n.trim();
                if n.is_empty() || len16(n) > 40 {
                    return Err(ProfileError::DisplayName);
                }
                Some(n.to_owned())
            }
            None => None,
        };
        let handle = match &self.handle {
            Some(h) => {
                let h = h.trim().to_lowercase();
                let ok = (3..=20).contains(&h.len())
                    && h.bytes()
                        .all(|b| b.is_ascii_lowercase() || b.is_ascii_digit() || b == b'_');
                if !ok {
                    return Err(ProfileError::Handle);
                }
                Some(h)
            }
            None => None,
        };
        if self.about.as_deref().is_some_and(|a| len16(a) > 160) {
            return Err(ProfileError::About);
        }
        Ok(ProfilePatch {
            display_name,
            handle,
            about: self.about.clone(),
        })
    }
}

/// What the worker tells the caller. Queued in order; `AccountManager::dispatch_events` hands them to the listeners.
#[derive(Clone, Debug, PartialEq)]
pub enum AccountEvent {
    StateChanged(State),
    ProfileChanged(Profile),
    DeviceCodeUpdated(DeviceCode),
    /// "Sync appearance" is on and the account has a newer look: apply it with `Settings::apply_appearance`
    AppearanceFromAccount(Appearance),
    /// a request the user asked for was refused or could not be made (`error`: the server's code, e.g. `handle_taken`)
    RequestFailed {
        what: &'static str,
        status: u16,
        error: String,
    },
}

/// Callbacks for `AccountManager::dispatch_events`, called on the thread that calls it.
pub trait AccountListener: Send + Sync {
    fn account_state_changed(&self, _state: State) {}
    fn account_profile_changed(&self, _profile: &Profile) {}
    fn account_device_code_updated(&self, _code: &DeviceCode) {}
    fn account_appearance_from_account(&self, _appearance: &Appearance) {}
    fn account_request_failed(&self, _what: &str, _status: u16, _error: &str) {}
}

/// What the plug-in tells the server about itself when it asks for a code (never tracks, names or audio).
#[derive(Clone, Debug)]
pub struct ClientMeta {
    pub version: String,
    pub os: String,
    pub daw: String,
    pub computer: String,
}

impl Default for ClientMeta {
    fn default() -> Self {
        let computer = std::env::var("COMPUTERNAME")
            .or_else(|_| std::env::var("HOSTNAME"))
            .unwrap_or_default();
        Self {
            version: env!("CARGO_PKG_VERSION").into(),
            os: std::env::consts::OS.into(),
            daw: String::new(),
            computer,
        }
    }
}

/// How often the worker does its periodic work (tests shorten these).
#[derive(Clone, Copy, Debug)]
pub struct Timings {
    /// `None`: the server's poll interval (5 s). Tests use a short one.
    pub poll_interval: Option<Duration>,
    /// token refresh + profile + appearance while signed in
    pub periodic: Duration,
    /// file watch and the countdown tick
    pub tick: Duration,
    /// a "hide my email" choice the server did not take is tried again after this long
    pub retry: Duration,
    /// a session loaded at start-up waits this long before its first token refresh and profile fetch, so a plug-in scanner
    /// that creates and drops the manager within moments makes no network traffic
    pub first_periodic: Duration,
}

impl Default for Timings {
    fn default() -> Self {
        Self {
            poll_interval: None,
            periodic: Duration::from_secs(60),
            tick: Duration::from_secs(1),
            retry: Duration::from_secs(10),
            first_periodic: Duration::from_secs(3),
        }
    }
}

#[derive(Clone, Debug, Default)]
pub struct AccountConfig {
    /// `None`: `$HEARASIDE_API_URL`, else `https://hearaside.netlify.app`
    pub server_base: Option<String>,
    /// `None`: `<app data>/HEARASIDE/<$HEARASIDE_ACCOUNT_NAME or "account">.dat`. Tests always give their own file.
    pub storage_file: Option<PathBuf>,
    /// `None`: `HEARASIDE_account_refresh` (shared with the C++ plug-ins). Tests give their own.
    pub lock_name: Option<String>,
    pub client: ClientMeta,
    pub timings: Timings,
}

/// `<user application data>/HEARASIDE/<name>.dat` where the C++ `AccountManager` keeps it (`paths.rs`: `~/Library/HEARASIDE` on
/// macOS, `$XDG_CONFIG_HOME` or `~/.config` on Linux, `%APPDATA%` on Windows). `name` is `account` unless
/// `$HEARASIDE_ACCOUNT_NAME` is set (tests use their own file).
pub fn default_storage_file() -> Option<PathBuf> {
    account_file_for(
        Os::current(),
        &PathEnv::from_process(),
        &env_or("HEARASIDE_ACCOUNT_NAME", "account"),
    )
}

fn now_ms() -> i64 {
    SystemTime::now()
        .duration_since(UNIX_EPOCH)
        .map_or(0, |d| d.as_millis() as i64)
}

/// UTF-16 length: how the website (zod `max`, a JavaScript string length) counts.
fn len16(s: &str) -> usize {
    s.encode_utf16().count()
}

/// The longest start of `s` of at most `max` UTF-16 units, never splitting a character.
fn clip(s: &str, max: usize) -> String {
    let mut n = 0;
    s.chars()
        .take_while(|c| {
            n += c.len_utf16();
            n <= max
        })
        .collect()
}

fn clamp_server_base(s: &str) -> String {
    s.trim().trim_end_matches('/').to_owned()
}

#[derive(Debug)]
enum Cmd {
    StartDeviceFlow { plugin: String, flow: u64 },
    FetchProfile,
    UpdateProfile(ProfilePatch),
    UploadAvatar(PathBuf),
    RemoveAvatar,
    UpdateDeviceName(String),
    SignOut,
}

struct Poll {
    flow: u64,
    device_code: String,
    interval: Duration,
    next_at: Instant,
}

struct Core {
    state: State,
    profile: Profile,
    code: DeviceCode,
    code_expires_at: Option<Instant>,
    server_base: String,
    access_token: String,
    refresh_token: String,
    expires_at_ms: i64,
    device_id: String,
    queue: VecDeque<Cmd>,
    poll: Option<Poll>,
    flow: u64,
    start_failed: bool,
    /// a "hide my email" choice the server has not taken yet (kept until it does; the server's older answer must not undo it)
    pending_hide_email: Option<bool>,
    hide_retry_at: Option<Instant>,
    /// when the periodic round (token refresh, profile, appearance) is next due; `None` = now
    next_periodic: Option<Instant>,
    /// something changed that the worker must write to the token file
    save_pending: bool,
    last_tick: Instant,
    last_mtime: Option<SystemTime>,
    // appearance sync
    local_appearance: Option<Appearance>,
    local_changed_at: i64,
    remote_seen_at: i64,
    local_dirty: bool,
    wake: bool,
    events: VecDeque<AccountEvent>,
}

struct Inner {
    core: Mutex<Core>,
    cv: Condvar,
    stop: AtomicBool,
    listeners: Mutex<Vec<(u64, Arc<dyn AccountListener>)>>,
    next_listener: Mutex<u64>,
    client: Client,
    storage: PathBuf,
    lock_name: String,
    meta: ClientMeta,
    timings: Timings,
}

/// One per process (or per account file in tests): create it when the first plug-in of the module loads and drop it at
/// module shutdown.
///
/// Dropping asks the worker to stop and waits up to a quarter of a second for it. The worker checks for the stop before
/// every network call, every disk write and while it waits for the refresh lock, so only a call already in flight can
/// outlast the drop (a call has no cancel; it ends within its 20 s deadline) and it writes nothing afterwards. When it does
/// outlast the drop, this module is pinned in memory (`GetModuleHandleExW` PIN on Windows, `RTLD_NODELETE` elsewhere), so
/// the host unloading the plug-in cannot pull the code away from under that thread.
///
/// Do not drop it while holding the operating system's loader lock (from `DllMain`, a static destructor of the module,
/// `DLL_PROCESS_DETACH`): joining a thread there can hang the host. Drop it from the module's shutdown on a normal thread,
/// as the C++ does with JUCE's `DeletedAtShutdown`.
pub struct AccountManager {
    inner: Arc<Inner>,
    thread: Mutex<Option<std::thread::JoinHandle<()>>>,
    alive: Arc<AtomicBool>,
}

impl Core {
    fn push(&mut self, e: AccountEvent) {
        // the countdown event comes every second: keep only the newest while nobody is listening
        if matches!(e, AccountEvent::DeviceCodeUpdated(_))
            && matches!(self.events.back(), Some(AccountEvent::DeviceCodeUpdated(_)))
        {
            self.events.pop_back();
        }
        if self.events.len() >= MAX_EVENTS {
            self.events.pop_front();
        }
        self.events.push_back(e);
    }

    fn set_state(&mut self, s: State) {
        if self.state != s {
            self.state = s;
            self.push(AccountEvent::StateChanged(s));
        }
    }

    fn notify_profile(&mut self) {
        let p = self.profile.clone();
        self.push(AccountEvent::ProfileChanged(p));
    }

    fn notify_code(&mut self) {
        let c = self.code.clone();
        self.push(AccountEvent::DeviceCodeUpdated(c));
    }

    fn code_remaining(&self) -> i32 {
        if self.code.expired {
            return 0;
        }
        if self.code.user_code.is_empty() {
            return self.code.expires_in_sec; // still asking the server
        }
        let rem = self.code_expires_at.map_or(Duration::ZERO, |t| {
            t.saturating_duration_since(Instant::now())
        });
        i32::try_from(rem.as_secs()).unwrap_or(i32::MAX)
    }

    fn token_valid_for(&self, margin_ms: i64) -> bool {
        !self.access_token.is_empty() && self.expires_at_ms > now_ms() + margin_ms
    }

    fn clear_session(&mut self) {
        self.access_token.clear();
        self.refresh_token.clear();
        self.device_id.clear();
        self.expires_at_ms = 0;
        self.profile = Profile::default();
        self.code = DeviceCode::default();
        self.code_expires_at = None;
        self.poll = None;
        self.pending_hide_email = None;
    }

    /// `parseProfileJson`: only what the answer has; a partial answer never blanks the rest.
    fn parse_profile(&mut self, v: &Value) {
        let Some(o) = v.as_object() else { return };
        let s = |k: &str| o.get(k).and_then(Value::as_str);
        let p = &mut self.profile;
        for (key, field) in [
            ("id", &mut p.id),
            ("email", &mut p.email),
            ("handle", &mut p.handle),
            ("displayName", &mut p.display_name),
            ("about", &mut p.about),
            ("deviceName", &mut p.device_name),
        ] {
            if let Some(x) = s(key) {
                *field = x.to_owned();
            }
        }
        if o.contains_key("avatarUrl") {
            p.avatar_url = s("avatarUrl").unwrap_or("").to_owned(); // null = no photo
        }
        for (key, field) in [("locale", &mut p.locale), ("plan", &mut p.plan)] {
            if let Some(x) = s(key).filter(|x| !x.is_empty()) {
                *field = x.to_owned();
            }
        }
        if let Some(b) = o.get("hideEmail").and_then(Value::as_bool)
            && self.pending_hide_email.is_none()
        {
            p.hide_email = b;
        }
        if let Some(b) = o.get("syncAppearance").and_then(Value::as_bool) {
            p.sync_appearance = b;
        }
    }
}

fn profile_json(p: &Profile) -> Value {
    json!({
        "id": p.id, "email": p.email, "handle": p.handle, "displayName": p.display_name, "about": p.about,
        "avatarUrl": p.avatar_url, "locale": p.locale, "hideEmail": p.hide_email, "plan": p.plan,
        "syncAppearance": p.sync_appearance, "deviceName": p.device_name,
    })
}

/// `appearance` as the website's `syncedSettings.appearance`.
fn appearance_json(a: &Appearance) -> Value {
    let mut o = serde_json::Map::new();
    if let Some(l) = a.language {
        o.insert(
            "language".into(),
            json!(if l == Language::Thai { "th" } else { "en" }),
        );
    }
    if let Some(t) = a.theme {
        o.insert(
            "theme".into(),
            json!(match t {
                ThemeMode::Light => "light",
                ThemeMode::Dark => "dark",
                ThemeMode::Auto => "system",
            }),
        );
    }
    if let Some(v) = a.ui_scale {
        o.insert("uiScale".into(), json!(v));
    }
    if let Some(v) = a.glass_alpha {
        o.insert("glassAlpha".into(), json!(v));
    }
    if let Some(v) = a.host_colours {
        o.insert("hostColours".into(), json!(v));
    }
    if let Some(v) = a.reduce_motion {
        o.insert("reduceMotion".into(), json!(v));
    }
    Value::Object(o)
}

fn appearance_from_json(v: &Value) -> Appearance {
    Appearance {
        language: v.get("language").and_then(Value::as_str).map(|s| {
            if s == "th" {
                Language::Thai
            } else {
                Language::English
            }
        }),
        theme: v.get("theme").and_then(Value::as_str).map(|t| match t {
            "light" => ThemeMode::Light,
            "dark" => ThemeMode::Dark,
            _ => ThemeMode::Auto,
        }),
        ui_scale: v.get("uiScale").and_then(Value::as_f64),
        glass_alpha: v.get("glassAlpha").and_then(Value::as_f64),
        host_colours: v.get("hostColours").and_then(Value::as_bool),
        reduce_motion: v.get("reduceMotion").and_then(Value::as_bool),
    }
}

/// `juce::Time::fromISO8601(..).toMilliseconds()` for the server's `2026-10-30T12:34:56.789Z` (and `+hh:mm` offsets);
/// 0 for anything else.
pub(crate) fn iso_ms(v: &Value) -> i64 {
    v.as_str().and_then(parse_iso8601).unwrap_or(0)
}

fn parse_iso8601(s: &str) -> Option<i64> {
    let b = s.trim().as_bytes();
    let num = |from: usize, len: usize| -> Option<i64> {
        let part = b.get(from..from + len)?;
        part.iter()
            .all(u8::is_ascii_digit)
            .then(|| part.iter().fold(0i64, |a, d| a * 10 + i64::from(d - b'0')))
    };
    let (y, mo, d) = (num(0, 4)?, num(5, 2)?, num(8, 2)?);
    if *b.get(4)? != b'-' || *b.get(7)? != b'-' || !(1..=12).contains(&mo) || !(1..=31).contains(&d)
    {
        return None;
    }
    let (mut h, mut mi, mut sec, mut ms, mut i) = (0, 0, 0, 0, 10);
    if matches!(b.get(10), Some(b'T' | b't' | b' ')) {
        h = num(11, 2)?;
        mi = num(14, 2)?;
        i = 16;
        if b.get(i) == Some(&b':') {
            sec = num(i + 1, 2)?;
            i += 3;
            if b.get(i) == Some(&b'.') {
                i += 1;
                let mut scale = 100;
                while i < b.len() && b[i].is_ascii_digit() {
                    ms += i64::from(b[i] - b'0') * scale;
                    scale /= 10;
                    i += 1;
                }
            }
        }
    }
    let mut offset_min = 0;
    match b.get(i) {
        Some(b'Z' | b'z') | None => {}
        Some(&sign @ (b'+' | b'-')) => {
            let oh = num(i + 1, 2)?;
            let om = if b.get(i + 3) == Some(&b':') {
                num(i + 4, 2)?
            } else {
                num(i + 3, 2).unwrap_or(0)
            };
            offset_min = (oh * 60 + om) * if sign == b'+' { 1 } else { -1 };
        }
        _ => return None,
    }
    // days from civil (Howard Hinnant)
    let y2 = if mo <= 2 { y - 1 } else { y };
    let era = y2.div_euclid(400);
    let yoe = y2 - era * 400;
    let doy = (153 * (mo + if mo > 2 { -3 } else { 9 }) + 2) / 5 + d - 1;
    let doe = yoe * 365 + yoe / 4 - yoe / 100 + doy;
    let days = era * 146_097 + doe - 719_468;
    Some(((days * 24 + h) * 60 + mi - offset_min) * 60_000 + sec * 1000 + ms)
}

impl AccountManager {
    /// Loads the stored session and starts the worker.
    pub fn new(config: AccountConfig) -> Self {
        let server = clamp_server_base(
            &config
                .server_base
                .clone()
                .unwrap_or_else(|| env_or("HEARASIDE_API_URL", DEFAULT_SERVER)),
        );
        let storage = config
            .storage_file
            .clone()
            .or_else(default_storage_file)
            .unwrap_or_default();
        let now = Instant::now();
        let core = Core {
            state: State::SignedOut,
            profile: Profile::default(),
            code: DeviceCode::default(),
            code_expires_at: None,
            server_base: server,
            access_token: String::new(),
            refresh_token: String::new(),
            expires_at_ms: 0,
            device_id: String::new(),
            queue: VecDeque::new(),
            poll: None,
            flow: 0,
            start_failed: false,
            pending_hide_email: None,
            hide_retry_at: None,
            next_periodic: None,
            save_pending: false,
            last_tick: now,
            last_mtime: None,
            local_appearance: None,
            local_changed_at: 0,
            remote_seen_at: 0,
            local_dirty: false,
            wake: false,
            events: VecDeque::new(),
        };
        let inner = Arc::new(Inner {
            core: Mutex::new(core),
            cv: Condvar::new(),
            stop: AtomicBool::new(false),
            listeners: Mutex::new(Vec::new()),
            next_listener: Mutex::new(1),
            client: Client::new(
                Duration::from_secs(10),
                Duration::from_secs(20),
                1024 * 1024,
            ),
            storage,
            lock_name: config.lock_name.unwrap_or_else(|| LOCK_NAME.to_owned()),
            meta: config.client,
            timings: config.timings,
        });
        {
            let disk = inner.read_disk();
            let mut c = inner.core.relock();
            inner.apply_disk(&mut c, disk);
            c.events.clear(); // the state at start-up is not news
            if matches!(c.state, State::SignedIn | State::Offline) {
                c.next_periodic = Some(Instant::now() + inner.timings.first_periodic);
            }
        }
        let worker = Arc::clone(&inner);
        let alive = Arc::new(AtomicBool::new(true));
        let flag = Arc::clone(&alive);
        let thread = std::thread::Builder::new()
            .name("AccountWorker".into())
            .spawn(move || {
                crate::sync::keep_running(|| worker.stop.load(Ordering::SeqCst), || worker.run());
                flag.store(false, Ordering::SeqCst);
            })
            .ok();
        Self {
            inner,
            thread: Mutex::new(thread),
            alive,
        }
    }

    /// Tests: turns false when the worker thread has ended.
    #[doc(hidden)]
    pub fn worker_alive_flag(&self) -> Arc<AtomicBool> {
        Arc::clone(&self.alive)
    }

    // ---- listeners and events ---------------------------------------------------------------------------------------

    /// Adds a listener; returns an id for `remove_listener`.
    pub fn add_listener(&self, l: Arc<dyn AccountListener>) -> u64 {
        let mut n = self.inner.next_listener.relock();
        let id = *n;
        *n += 1;
        self.inner.listeners.relock().push((id, l));
        id
    }

    pub fn remove_listener(&self, id: u64) {
        self.inner.listeners.relock().retain(|(i, _)| *i != id);
    }

    /// The queued events, oldest first (for a caller with its own dispatch).
    pub fn take_events(&self) -> Vec<AccountEvent> {
        self.inner.core.relock().events.drain(..).collect()
    }

    /// Hands the queued events to the listeners, on the calling thread. Call it from the thread the callbacks must run on
    /// (the message thread), from a timer of about 10 to 30 Hz; it returns at once when nothing is queued.
    pub fn dispatch_events(&self) {
        let events = self.take_events();
        if events.is_empty() {
            return;
        }
        let listeners: Vec<Arc<dyn AccountListener>> = self
            .inner
            .listeners
            .relock()
            .iter()
            .map(|(_, l)| Arc::clone(l))
            .collect();
        for e in &events {
            for l in &listeners {
                match e {
                    AccountEvent::StateChanged(s) => l.account_state_changed(*s),
                    AccountEvent::ProfileChanged(p) => l.account_profile_changed(p),
                    AccountEvent::DeviceCodeUpdated(c) => l.account_device_code_updated(c),
                    AccountEvent::AppearanceFromAccount(a) => l.account_appearance_from_account(a),
                    AccountEvent::RequestFailed {
                        what,
                        status,
                        error,
                    } => l.account_request_failed(what, *status, error),
                }
            }
        }
    }

    // ---- reading ----------------------------------------------------------------------------------------------------

    pub fn state(&self) -> State {
        self.inner.core.relock().state
    }

    pub fn is_signed_in(&self) -> bool {
        matches!(self.state(), State::SignedIn | State::Offline)
    }

    /// A copy: the worker keeps changing the real one.
    pub fn profile(&self) -> Profile {
        self.inner.core.relock().profile.clone()
    }

    pub fn device_code(&self) -> DeviceCode {
        self.inner.core.relock().code.clone()
    }

    pub fn server_base_url(&self) -> String {
        self.inner.core.relock().server_base.clone()
    }

    pub fn set_server_base_url(&self, url: &str) {
        self.inner.core.relock().server_base = clamp_server_base(url);
    }

    /// The last "Sign in with your browser" could not get a code (no internet, server down) or the code was refused.
    pub fn start_failed(&self) -> bool {
        self.inner.core.relock().start_failed
    }

    /// The plug-in's access token while it is good for at least 30 s more, else `""` (any thread). Only for this
    /// account's own server (`server_base_url`): never send it anywhere else.
    pub fn access_token(&self) -> String {
        let c = self.inner.core.relock();
        if c.token_valid_for(30_000) {
            c.access_token.clone()
        } else {
            String::new()
        }
    }

    /// Seconds the sign-in code is still good for (the full time while the server is still being asked, 0 when expired).
    pub fn code_remaining_seconds(&self) -> i32 {
        self.inner.core.relock().code_remaining()
    }

    /// The email as the screen may show it: with "hide my email" on, `a•••@example.com` (and `•••` when there is no name
    /// before an `@`), so a stream never shows it; the C++ showed addresses such as `a@x.com` in the clear.
    pub fn masked_email(&self) -> String {
        let c = self.inner.core.relock();
        mask_email(&c.profile.email, c.profile.hide_email)
    }

    // ---- actions (queued for the worker) ----------------------------------------------------------------------------

    /// "Sign in with your browser": asks the server for a code, then polls until the user allows it, denies it or the code
    /// expires. `plugin_name` is "hub", "track" or "app" (anything else counts as "track"; "HEARASIDE Hub" is fine).
    /// Does nothing when already signed in.
    pub fn start_device_flow(&self, plugin_name: &str) {
        let mut c = self.inner.core.relock();
        if matches!(c.state, State::SignedIn | State::Offline) {
            return;
        }
        c.flow += 1;
        let flow = c.flow;
        c.poll = None;
        c.queue
            .retain(|q| !matches!(q, Cmd::StartDeviceFlow { .. }));
        c.queue.push_back(Cmd::StartDeviceFlow {
            plugin: plugin_name.to_lowercase(),
            flow,
        });
        c.code = DeviceCode::default(); // no code yet: the panel says "Getting a code..." and nothing counts down
        c.code_expires_at = None;
        c.start_failed = false;
        c.set_state(State::WaitingForCode);
        c.notify_code();
        self.inner.wake(&mut c);
    }

    /// Stops waiting for the browser. Only changes anything while a code is being waited for.
    pub fn cancel_device_flow(&self) {
        let mut c = self.inner.core.relock();
        if c.state != State::WaitingForCode {
            return;
        }
        c.flow += 1;
        c.poll = None;
        c.queue
            .retain(|q| !matches!(q, Cmd::StartDeviceFlow { .. }));
        c.set_state(State::SignedOut);
        self.inner.wake(&mut c);
    }

    pub fn sign_out(&self) {
        self.inner.push_cmd(Cmd::SignOut);
    }

    pub fn refresh_profile(&self) {
        self.inner.push_cmd(Cmd::FetchProfile);
    }

    /// Changes the profile on the account. A field the website would refuse is an `Err` here and nothing is sent.
    pub fn update_profile(&self, patch: ProfilePatch) -> Result<(), ProfileError> {
        self.inner.push_cmd(Cmd::UpdateProfile(patch.checked()?));
        Ok(())
    }

    /// JPG, PNG or WebP up to 5 MB (the website turns it into a 512 px WebP).
    pub fn upload_avatar(&self, file: impl Into<PathBuf>) {
        self.inner.push_cmd(Cmd::UploadAvatar(file.into()));
    }

    pub fn remove_avatar(&self) {
        self.inner.push_cmd(Cmd::RemoveAvatar);
    }

    /// Takes effect at once on this computer and is sent to the account as soon as it can be (retried until the server
    /// has it; the account's older answer does not undo it meanwhile).
    pub fn set_hide_email(&self, hide: bool) {
        let mut c = self.inner.core.relock();
        c.profile.hide_email = hide;
        c.pending_hide_email = Some(hide);
        c.hide_retry_at = None;
        c.save_pending = true; // the worker writes the token file
        c.notify_profile();
        self.inner.wake(&mut c);
    }

    /// "Sync appearance" on or off. Turning it on needs the look of this computer (`Settings::appearance()`): it goes up
    /// only when the account has none yet, otherwise the account's comes down.
    pub fn set_sync_appearance(&self, sync: bool, current: Option<Appearance>) {
        let mut c = self.inner.core.relock();
        c.profile.sync_appearance = sync;
        if sync {
            c.local_appearance = current;
            c.local_changed_at = 0;
            c.local_dirty = true;
            c.remote_seen_at = 0;
        }
        c.save_pending = true;
        c.notify_profile();
        c.next_periodic = None; // sync now
        self.inner.wake(&mut c);
    }

    /// Wire `Settings::set_appearance_hook` to this: the look of this computer changed.
    pub fn appearance_changed(&self, a: Appearance) {
        let mut c = self.inner.core.relock();
        if !c.profile.sync_appearance {
            return;
        }
        c.local_appearance = Some(a);
        c.local_changed_at = now_ms();
        c.local_dirty = true;
        c.next_periodic = None;
        self.inner.wake(&mut c);
    }

    /// The name of this computer in Account > Devices on the website.
    pub fn set_device_name(&self, name: &str) {
        self.inner.push_cmd(Cmd::UpdateDeviceName(name.to_owned()));
    }
}

pub(crate) fn mask_email(email: &str, hide: bool) -> String {
    if email.is_empty() {
        return String::new();
    }
    if !hide {
        return email.to_owned();
    }
    match email.find('@') {
        Some(at) if at >= 1 => {
            let first = email.chars().next().unwrap_or_default();
            format!("{first}\u{2022}\u{2022}\u{2022}{}", &email[at..])
        }
        _ => "\u{2022}\u{2022}\u{2022}".to_owned(),
    }
}

impl Drop for AccountManager {
    fn drop(&mut self) {
        self.inner.stop.store(true, Ordering::SeqCst);
        {
            let mut c = self.inner.core.relock();
            self.inner.wake(&mut c);
        }
        if let Some(h) = self.thread.relock().take() {
            let until = Instant::now() + Duration::from_millis(250);
            while !h.is_finished() && Instant::now() < until {
                std::thread::sleep(Duration::from_millis(5));
            }
            if h.is_finished() {
                let _ = h.join();
            } else {
                // a call is in flight: the thread ends by itself when it returns (it will not write anything), but
                // not before the host may unload this module
                crate::pin::pin_module_for_detached_thread();
            }
        }
    }
}

// ---- the worker ----------------------------------------------------------------------------------------------------

mod worker;

enum Step {
    Cmd(Cmd),
    Save,
    HideEmail(bool),
    Poll,
    Periodic,
    Tick,
}

impl Inner {
    fn wake(&self, c: &mut Core) {
        c.wake = true;
        self.cv.notify_all();
    }

    fn push_cmd(&self, cmd: Cmd) {
        let mut c = self.core.relock();
        c.queue.push_back(cmd);
        self.wake(&mut c);
    }

    fn stopped(&self) -> bool {
        self.stop.load(Ordering::SeqCst)
    }

    fn run(&self) {
        while !self.stopped() {
            let step = {
                let mut c = self.core.relock();
                match self.next_step(&mut c) {
                    Ok(s) => s,
                    Err(wait) => {
                        if !c.wake {
                            c = self
                                .cv
                                .wait_timeout(c, wait)
                                .unwrap_or_else(std::sync::PoisonError::into_inner)
                                .0;
                        }
                        c.wake = false;
                        continue;
                    }
                }
            };
            match step {
                Step::Cmd(cmd) => self.do_cmd(cmd),
                Step::Save => self.persist(),
                Step::HideEmail(h) => self.do_patch_hide_email(h),
                Step::Poll => self.do_poll(),
                Step::Periodic => {
                    self.do_refresh_token("");
                    if !self.stopped() {
                        self.do_fetch_profile();
                    }
                    if !self.stopped() {
                        self.do_sync_appearance();
                    }
                }
                Step::Tick => self.do_tick(),
            }
        }
    }

    /// The next thing to do, or how long until there is one.
    fn next_step(&self, c: &mut Core) -> Result<Step, Duration> {
        if let Some(cmd) = c.queue.pop_front() {
            return Ok(Step::Cmd(cmd));
        }
        if c.save_pending {
            c.save_pending = false;
            return Ok(Step::Save);
        }
        let signed_in = matches!(c.state, State::SignedIn | State::Offline);
        let now = Instant::now();
        let mut wait = Duration::from_secs(3600);
        if signed_in && let Some(h) = c.pending_hide_email {
            match c.hide_retry_at {
                Some(t) if t > now => wait = wait.min(t - now),
                _ => return Ok(Step::HideEmail(h)),
            }
        }
        if let Some(p) = &c.poll {
            if p.next_at <= now {
                return Ok(Step::Poll);
            }
            wait = wait.min(p.next_at - now);
        }
        if signed_in {
            match c.next_periodic {
                Some(t) if t > now => wait = wait.min(t - now),
                _ => {
                    c.next_periodic = Some(now + self.timings.periodic);
                    return Ok(Step::Periodic);
                }
            }
        }
        let since_tick = now.saturating_duration_since(c.last_tick);
        if since_tick >= self.timings.tick {
            c.last_tick = now;
            return Ok(Step::Tick);
        }
        Err(wait.min(self.timings.tick - since_tick))
    }
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn masked_email_edge_cases() {
        let dots = "\u{2022}\u{2022}\u{2022}";
        assert_eq!(
            mask_email("mint@example.com", true),
            format!("m{dots}@example.com")
        );
        assert_eq!(
            mask_email("a@x.com", true),
            format!("a{dots}@x.com"),
            "one-letter names were shown in full by the C++"
        );
        assert_eq!(
            mask_email("no-at-sign", true),
            dots,
            "no @: nothing is shown"
        );
        assert_eq!(mask_email("@x.com", true), dots);
        assert_eq!(
            mask_email("\u{e21}\u{e34}\u{e19}\u{e15}\u{e4c}@x.th", true),
            format!("\u{e21}{dots}@x.th"),
            "first letter, not first byte"
        );
        assert_eq!(mask_email("mint@example.com", false), "mint@example.com");
        assert_eq!(mask_email("", true), "");
    }

    #[test]
    fn iso8601_like_the_server_writes_it() {
        assert_eq!(parse_iso8601("1970-01-01T00:00:00.000Z"), Some(0));
        assert_eq!(
            parse_iso8601("2026-10-30T12:34:56.789Z"),
            Some(1_793_363_696_789)
        );
        assert_eq!(
            parse_iso8601("2026-10-30T12:34:56Z"),
            Some(1_793_363_696_000)
        );
        assert_eq!(
            parse_iso8601("2026-10-30T19:34:56+07:00"),
            Some(1_793_363_696_000)
        );
        assert_eq!(parse_iso8601("2000-02-29T00:00:00Z"), Some(951_782_400_000));
        assert_eq!(parse_iso8601("nonsense"), None);
        assert_eq!(parse_iso8601("2026-13-01T00:00:00Z"), None);
        assert_eq!(iso_ms(&Value::Null), 0);
        assert_eq!(iso_ms(&json!("")), 0);
    }
}
