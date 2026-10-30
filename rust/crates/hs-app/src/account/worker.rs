//! The worker thread's side of `AccountManager`: HTTP calls, the token file, the sign-in poll. Nothing in here is called
//! from the caller's thread, and nothing slow (a network call, DPAPI, a file write) runs with the `core` mutex held.
//!
//! Stopping: `stopped()` is checked before every network call, before every write or delete of the token file and while
//! waiting for the refresh lock, so once the owner has dropped the manager this thread starts nothing new and writes nothing.
use super::*;
use crate::vault::Lock;
use std::io::Read as _;

/// A request's answer is what the user asked for or an error code to show.
fn error_of(r: &Reply) -> String {
    r.json()
        .get("error")
        .and_then(Value::as_str)
        .unwrap_or("")
        .to_owned()
}

fn sniff_image(b: &[u8]) -> Option<(&'static str, &'static str)> {
    if b.starts_with(&[0xff, 0xd8, 0xff]) {
        Some(("image/jpeg", "avatar.jpg"))
    } else if b.starts_with(&[0x89, b'P', b'N', b'G']) {
        Some(("image/png", "avatar.png"))
    } else if b.len() > 12 && &b[..4] == b"RIFF" && &b[8..12] == b"WEBP" {
        Some(("image/webp", "avatar.webp"))
    } else {
        None
    }
}

/// What the token file holds right now, read without the `core` mutex.
pub(super) enum Disk {
    /// no storage location at all
    Nowhere,
    Missing,
    /// there, but busy or unreadable at this moment: look again later
    Unreadable,
    /// there, but not a session of this user (damaged, plain text, another user's)
    Invalid,
    Session {
        value: Value,
        mtime: Option<SystemTime>,
    },
}

impl Inner {
    /// One call to the account's own server (`status` 0 when it cannot be reached, when the server address is one a
    /// token must not be sent to, or when the manager is being dropped).
    fn call(&self, method: &str, endpoint: &str, body: Option<&Value>, bearer: &str) -> Reply {
        if self.stopped() {
            return Reply::default();
        }
        let base = self.core.relock().server_base.clone();
        if !secure_enough(&base) {
            return Reply::default();
        }
        // only a GET (no secret in a body) may be sent on to another address
        let follow = if method == "GET" { 2 } else { 0 };
        self.client
            .json(method, &format!("{base}{endpoint}"), body, bearer, follow)
    }

    /// A call with the access token; refused with 401 = the token ran out (or was replaced by another plug-in): refresh
    /// once and try again.
    fn authed(&self, f: impl Fn(&str) -> Reply) -> Reply {
        let token = self.core.relock().access_token.clone();
        if token.is_empty() {
            return Reply::default();
        }
        let r = f(&token);
        if r.status != 401 {
            return r;
        }
        self.do_refresh_token(&token);
        let again = self.core.relock().access_token.clone();
        if again.is_empty() { r } else { f(&again) }
    }

    pub(super) fn do_cmd(&self, cmd: Cmd) {
        if self.stopped() {
            return;
        }
        match cmd {
            Cmd::StartDeviceFlow { plugin, flow } => self.do_start_flow(&plugin, flow),
            Cmd::FetchProfile => self.do_fetch_profile(),
            Cmd::UpdateProfile(p) => self.do_update_profile(&p),
            Cmd::UploadAvatar(f) => self.do_upload_avatar(&f),
            Cmd::RemoveAvatar => self.do_profile_request("avatar", "DELETE", "/api/v1/me/avatar"),
            Cmd::UpdateDeviceName(n) => self.do_update_device_name(&n),
            Cmd::SignOut => self.do_sign_out(),
        }
    }

    // ---- the token file -------------------------------------------------------------------------------------------

    fn file_mtime(&self) -> Option<SystemTime> {
        std::fs::metadata(&self.storage)
            .and_then(|m| m.modified())
            .ok()
    }

    /// Reads and decrypts the file. Takes no lock: call it, then `apply_disk` with the `core` mutex.
    pub(super) fn read_disk(&self) -> Disk {
        if self.storage.as_os_str().is_empty() {
            return Disk::Nowhere;
        }
        let mtime = self.file_mtime();
        let bytes = match std::fs::read(&self.storage) {
            Ok(b) => b,
            Err(e) if e.kind() == std::io::ErrorKind::NotFound => return Disk::Missing,
            Err(_) => return Disk::Unreadable,
        };
        let parsed = vault::unprotect(&bytes)
            .and_then(|t| serde_json::from_str::<Value>(&t).ok())
            .filter(Value::is_object);
        match parsed {
            Some(value) => Disk::Session { value, mtime },
            None => Disk::Invalid,
        }
    }

    /// `loadFromDisk`: what another plug-in (or an earlier run) stored. A missing file means "signed out there".
    pub(super) fn apply_disk(&self, c: &mut Core, d: Disk) {
        match d {
            Disk::Nowhere | Disk::Unreadable => {}
            Disk::Missing => {
                if c.last_mtime.is_some() && matches!(c.state, State::SignedIn | State::Offline) {
                    // removed by another plug-in: this computer is signed out
                    c.clear_session();
                    c.set_state(State::SignedOut);
                    c.notify_profile();
                }
                c.last_mtime = None;
            }
            Disk::Invalid => {
                // not ours, not this user's, or damaged: never trusted, never treated as plain text
                c.last_mtime = self.file_mtime();
                if matches!(c.state, State::SignedIn | State::Offline) {
                    c.clear_session();
                    c.set_state(State::SignedOut);
                    c.notify_profile();
                }
            }
            Disk::Session { value: v, mtime } => {
                c.last_mtime = mtime;
                let text = |k: &str| v.get(k).and_then(Value::as_str).unwrap_or("").to_owned();
                c.access_token = text("access_token");
                c.refresh_token = text("refresh_token");
                c.device_id = text("device_id");
                c.expires_at_ms = v
                    .get("expires_at")
                    .and_then(|e| e.as_i64().or_else(|| e.as_f64().map(|f| f as i64)))
                    .unwrap_or(0);
                if let Some(u) = v.get("user").filter(|u| u.is_object()) {
                    let keep_pending = c.pending_hide_email;
                    c.profile = Profile::default();
                    c.pending_hide_email = None;
                    c.parse_profile(u);
                    c.pending_hide_email = keep_pending;
                    if let Some(h) = keep_pending {
                        c.profile.hide_email = h;
                    }
                }
                if c.access_token.is_empty() {
                    c.set_state(State::SignedOut);
                } else {
                    c.poll = None;
                    c.code = DeviceCode::default();
                    c.code_expires_at = None;
                    c.set_state(State::SignedIn);
                }
            }
        }
    }

    /// `saveToDisk`, on the worker: the session is copied under the mutex, then protected and written without it. When the
    /// tokens cannot be protected they are not stored at all (they stay in memory). Never writes once stopped.
    pub(super) fn persist(&self) {
        self.persist_inner(false);
    }

    /// Like `persist`, but also after the manager was dropped: for tokens the server has just rotated or issued. The old
    /// refresh token no longer works, so a session that is not written is lost, and the next start would present the dead
    /// token and be signed out as "stolen". This runs at most once per call that was already in flight at the drop.
    fn persist_critical(&self) {
        self.persist_inner(true);
    }

    fn persist_inner(&self, even_if_stopped: bool) {
        if self.storage.as_os_str().is_empty() || (self.stopped() && !even_if_stopped) {
            return;
        }
        let doc = {
            let c = self.core.relock();
            if c.access_token.is_empty() && c.refresh_token.is_empty() {
                return;
            }
            json!({
                "access_token": c.access_token, "refresh_token": c.refresh_token, "device_id": c.device_id,
                "expires_at": c.expires_at_ms, "user": profile_json(&c.profile),
            })
        };
        let Some(bytes) = vault::protect(&doc.to_string()) else {
            return;
        };
        if (self.stopped() && !even_if_stopped)
            || write_atomic(&self.storage, &bytes, true, true).is_err()
        {
            return;
        }
        let mtime = self.file_mtime();
        self.core.relock().last_mtime = mtime;
    }

    /// Deletes the token file (unless stopped) and forgets its time stamp.
    fn forget_file(&self) {
        if !self.stopped() {
            let _ = std::fs::remove_file(&self.storage);
        }
        self.core.relock().last_mtime = None;
    }

    fn reload_if_file_changed(&self) {
        let known = self.core.relock().last_mtime;
        if self.file_mtime() != known {
            let d = self.read_disk();
            self.apply_disk(&mut self.core.relock(), d);
        }
    }

    // ---- sign-in ----------------------------------------------------------------------------------------------------

    fn do_start_flow(&self, plugin_name: &str, flow: u64) {
        {
            let c = self.core.relock();
            if c.flow != flow || c.state != State::WaitingForCode {
                return;
            }
        }
        // the server takes exactly hub / track / app: anything else is a 400 and no code
        let plugin = if plugin_name.contains("hub") {
            "hub"
        } else if plugin_name.contains("app") {
            "app"
        } else {
            "track"
        };
        let body = json!({"client_info": {
            "plugin": plugin, "version": clip(&self.meta.version, 20), "os": clip(&self.meta.os, 40),
            "daw": clip(&self.meta.daw, 40), "computer": clip(&self.meta.computer, 64),
        }});
        let r = self.call("POST", "/api/v1/device/start", Some(&body), "");
        let mut c = self.core.relock();
        if c.flow != flow || c.state != State::WaitingForCode {
            return; // cancelled while asking
        }
        let v = r.json();
        let text = |k: &str| v.get(k).and_then(Value::as_str).unwrap_or("").to_owned();
        let user_code = text("user_code");
        let device_code = text("device_code");
        if !r.ok() || user_code.is_empty() || device_code.is_empty() {
            // not signed in, whatever the reason: "Offline" would mean "signed in, can't reach the server"
            c.start_failed = true;
            c.set_state(State::SignedOut);
            c.push(AccountEvent::StateChanged(State::SignedOut)); // the panel shows why, even when the state was already SignedOut
            return;
        }
        let expires_in = v
            .get("expires_in")
            .and_then(Value::as_i64)
            .unwrap_or(600)
            .clamp(1, 3600);
        let interval = v
            .get("interval")
            .and_then(Value::as_i64)
            .unwrap_or(5)
            .clamp(2, 60);
        c.start_failed = false;
        c.code = DeviceCode {
            user_code,
            verification_uri: text("verification_uri"),
            verification_uri_complete: text("verification_uri_complete"),
            expires_in_sec: expires_in as i32,
            interval_sec: interval as i32,
            expired: false,
        };
        // monotonic: a clock change (NTP, a laptop waking up) must not stretch or cut the countdown
        c.code_expires_at = Some(Instant::now() + Duration::from_secs(expires_in as u64));
        let every = self
            .timings
            .poll_interval
            .unwrap_or(Duration::from_secs(interval as u64));
        c.poll = Some(Poll {
            flow,
            device_code,
            interval: every,
            next_at: Instant::now() + every,
        });
        c.notify_code();
    }

    pub(super) fn do_poll(&self) {
        let (flow, device_code) = {
            let mut c = self.core.relock();
            let Some(p) = &c.poll else { return };
            let (flow, dc) = (p.flow, p.device_code.clone());
            if c.flow != flow || c.state != State::WaitingForCode || dc.is_empty() {
                c.poll = None;
                return;
            }
            if c.code_expires_at.is_some_and(|t| Instant::now() >= t) {
                // the code ran out here: no point asking any more (the C++ kept asking, forever, when the server was down)
                c.poll = None;
                if !c.code.expired {
                    c.code.expired = true;
                    c.notify_code();
                }
                return;
            }
            (flow, dc)
        };
        let r = self.call(
            "POST",
            "/api/v1/device/token",
            Some(&json!({"device_code": device_code})),
            "",
        );
        let mut c = self.core.relock();
        if c.flow != flow || c.state != State::WaitingForCode {
            return;
        }
        let v = r.json();
        if r.status == 200 && v.is_object() {
            let text = |k: &str| v.get(k).and_then(Value::as_str).unwrap_or("").to_owned();
            if !text("access_token").is_empty() && !text("refresh_token").is_empty() {
                c.access_token = text("access_token");
                c.refresh_token = text("refresh_token");
                c.device_id = text("device_id");
                c.expires_at_ms = now_ms()
                    + v.get("expires_in")
                        .and_then(Value::as_i64)
                        .unwrap_or(900)
                        .clamp(30, 86_400)
                        * 1000;
                c.profile = Profile::default();
                c.pending_hide_email = None;
                if let Some(u) = v.get("user") {
                    c.parse_profile(u);
                }
                c.poll = None;
                c.code = DeviceCode::default();
                c.code_expires_at = None;
                c.start_failed = false;
                drop(c);
                // stored before it is announced: "signed in" means the next plug-in finds the session in the file
                self.persist_critical();
                let mut c = self.core.relock();
                c.set_state(State::SignedIn);
                c.notify_profile();
                c.next_periodic = Some(Instant::now() + self.timings.periodic);
                return;
            }
        } else {
            // 429 (the site asks for fewer requests) is the same as the server's own slow_down
            let error = if r.status == 429 {
                "slow_down"
            } else if r.status == 400 && v.is_object() {
                v.get("error").and_then(Value::as_str).unwrap_or("")
            } else {
                ""
            };
            match error {
                "slow_down" => {
                    if let Some(p) = c.poll.as_mut() {
                        p.interval =
                            (p.interval + Duration::from_secs(5)).min(Duration::from_secs(60));
                    }
                }
                "access_denied" => {
                    c.poll = None;
                    c.set_state(State::SignedOut);
                    return;
                }
                "invalid_grant" => {
                    // the code was already used (or is unknown): waiting longer can't help, say it failed
                    c.poll = None;
                    c.start_failed = true;
                    c.set_state(State::SignedOut);
                    c.push(AccountEvent::StateChanged(State::SignedOut));
                    return;
                }
                "expired_token" => {
                    c.poll = None;
                    c.code.expired = true;
                    c.notify_code();
                    return;
                }
                _ => {} // authorization_pending and anything unknown: ask again
            }
        }
        // 0 (server unreachable) and every other answer: ask again at the next interval
        if let Some(p) = c.poll.as_mut() {
            p.next_at = Instant::now() + p.interval;
        }
    }

    // ---- tokens -----------------------------------------------------------------------------------------------------

    /// A new access token before the old one runs out. `failed`: the token a request was just refused with.
    /// Every DAW (process) with a HEARASIDE plug-in shares the token file, and a refresh token works once: one process
    /// at a time, and only when the token in the file still needs it (another one may have just refreshed), or the
    /// server takes the reuse for a stolen token and signs this computer out.
    pub(super) fn do_refresh_token(&self, failed: &str) {
        let _held = match vault::lock(&self.lock_name, Duration::from_secs(25), &|| self.stopped())
        {
            Lock::Held(g) => Some(g),
            Lock::Unavailable => None, // no lock can be made here: go on without, like the C++
            Lock::TimedOut => return,  // the next round tries again
        };
        if self.stopped() {
            return;
        }
        self.reload_if_file_changed();
        let refresh = {
            let c = self.core.relock();
            if c.token_valid_for(2 * 60_000) && c.access_token != failed {
                return;
            }
            c.refresh_token.clone()
        };
        if refresh.is_empty() {
            return;
        }
        let r = self.call(
            "POST",
            "/api/v1/token/refresh",
            Some(&json!({"refresh_token": refresh})),
            "",
        );
        if r.ok() {
            let v = r.json();
            let (at, rt) = (
                v.get("access_token").and_then(Value::as_str),
                v.get("refresh_token").and_then(Value::as_str),
            );
            if let (Some(at), Some(rt)) =
                (at.filter(|s| !s.is_empty()), rt.filter(|s| !s.is_empty()))
            {
                {
                    let mut c = self.core.relock();
                    c.access_token = at.to_owned();
                    c.refresh_token = rt.to_owned();
                    c.expires_at_ms = now_ms()
                        + v.get("expires_in")
                            .and_then(Value::as_i64)
                            .unwrap_or(900)
                            .clamp(30, 86_400)
                            * 1000;
                }
                self.persist_critical(); // still holding the refresh lock: the next process reads the rotated token
            }
        } else if r.status == 401 {
            // revoked (on the website, or because an old token came back): this computer is signed out; the server
            // already knows, so nothing to revoke
            self.sign_out_locally();
        }
    }

    fn sign_out_locally(&self) {
        {
            let mut c = self.core.relock();
            c.clear_session();
            c.queue.retain(|q| matches!(q, Cmd::StartDeviceFlow { .. }));
            c.set_state(State::SignedOut);
            c.notify_profile();
        }
        self.forget_file();
    }

    pub(super) fn do_fetch_profile(&self) {
        let r = self.authed(|t| self.call("GET", "/api/v1/me", None, t));
        let mut c = self.core.relock();
        if c.access_token.is_empty() {
            return; // nothing to ask with, or signed out meanwhile
        }
        if r.status == 0 {
            c.set_state(State::Offline);
            return;
        }
        let v = r.json();
        if r.status == 200 && v.is_object() {
            c.parse_profile(&v);
            c.set_state(State::SignedIn);
            c.notify_profile();
            drop(c);
            self.persist();
        }
    }

    fn fail(&self, what: &'static str, r: &Reply) {
        let error = if r.status == 0 {
            "offline".to_owned()
        } else {
            error_of(r)
        };
        self.core.relock().push(AccountEvent::RequestFailed {
            what,
            status: r.status,
            error,
        });
    }

    /// A request whose 2xx answer is the new profile (`/me`).
    fn took_profile(&self, what: &'static str, r: &Reply) {
        if !r.ok() {
            self.fail(what, r);
            return;
        }
        let v = r.json();
        if v.is_object() {
            let mut c = self.core.relock();
            c.parse_profile(&v);
            c.notify_profile();
            drop(c);
            self.persist();
        }
    }

    fn do_profile_request(&self, what: &'static str, method: &str, endpoint: &str) {
        let r = self.authed(|t| self.call(method, endpoint, None, t));
        if r.status == 0 && self.core.relock().access_token.is_empty() {
            return;
        }
        self.took_profile(what, &r);
    }

    fn do_update_profile(&self, p: &ProfilePatch) {
        let mut o = serde_json::Map::new();
        for (k, v) in [
            ("displayName", &p.display_name),
            ("handle", &p.handle),
            ("about", &p.about),
        ] {
            if let Some(v) = v {
                o.insert(k.into(), json!(v));
            }
        }
        if o.is_empty() {
            return;
        }
        let body = Value::Object(o);
        let r = self.authed(|t| self.call("PATCH", "/api/v1/me", Some(&body), t));
        if r.status == 0 && self.core.relock().access_token.is_empty() {
            return;
        }
        self.took_profile("profile", &r);
    }

    fn do_upload_avatar(&self, file: &std::path::Path) {
        let mut bytes = Vec::new();
        let read = std::fs::File::open(file)
            .and_then(|f| f.take(MAX_AVATAR_BYTES + 1).read_to_end(&mut bytes));
        let local_error = |code: &str| {
            self.core.relock().push(AccountEvent::RequestFailed {
                what: "avatar",
                status: 0,
                error: code.to_owned(),
            });
        };
        if read.is_err() || bytes.is_empty() {
            return local_error("bad_image");
        }
        if bytes.len() as u64 > MAX_AVATAR_BYTES {
            return local_error("too_big");
        }
        let Some((mime, name)) = sniff_image(&bytes) else {
            return local_error("bad_image");
        };
        let mut boundary = format!("----hearaside{:x}{:x}", now_ms(), std::process::id());
        while bytes
            .windows(boundary.len())
            .any(|w| w == boundary.as_bytes())
        {
            boundary.push('x');
        }
        let mut body = format!("--{boundary}\r\nContent-Disposition: form-data; name=\"photo\"; filename=\"{name}\"\r\nContent-Type: {mime}\r\n\r\n").into_bytes();
        body.extend_from_slice(&bytes);
        body.extend_from_slice(format!("\r\n--{boundary}--\r\n").as_bytes());
        let r = self.authed(|t| {
            if self.stopped() {
                return Reply::default();
            }
            let base = self.core.relock().server_base.clone();
            if !secure_enough(&base) {
                return Reply::default();
            }
            self.client.send(
                "POST",
                &format!("{base}/api/v1/me/avatar"),
                vec![
                    ("Authorization", format!("Bearer {t}")),
                    (
                        "Content-Type",
                        format!("multipart/form-data; boundary={boundary}"),
                    ),
                ],
                body.clone(),
                0,
            )
        });
        if r.status == 0 && self.core.relock().access_token.is_empty() {
            return;
        }
        self.took_profile("avatar", &r);
    }

    fn do_update_device_name(&self, name: &str) {
        let name = clip(name.trim(), 64);
        if name.is_empty() {
            return;
        }
        let r =
            self.authed(|t| self.call("PATCH", "/api/v1/device", Some(&json!({"name": name})), t));
        if r.ok() {
            let mut c = self.core.relock();
            c.profile.device_name = name;
            c.notify_profile();
            drop(c);
            self.persist();
        } else if r.status != 0 || !self.core.relock().access_token.is_empty() {
            self.fail("device", &r);
        }
    }

    fn do_sign_out(&self) {
        let token = self.core.relock().access_token.clone();
        if !token.is_empty() {
            self.call("POST", "/api/v1/device/revoke", Some(&json!({})), &token); // best effort: signed out here either way
        }
        self.sign_out_locally();
    }

    pub(super) fn do_patch_hide_email(&self, hide: bool) {
        let r =
            self.authed(|t| self.call("PATCH", "/api/v1/me", Some(&json!({"hideEmail": hide})), t));
        let mut c = self.core.relock();
        if r.ok() {
            if c.pending_hide_email == Some(hide) {
                c.pending_hide_email = None;
                c.hide_retry_at = None;
            }
        } else if (400..500).contains(&r.status)
            && r.status != 401
            && r.status != 408
            && r.status != 429
        {
            c.pending_hide_email = None; // refused for good: asking again would not change the answer
        } else {
            c.hide_retry_at = Some(Instant::now() + self.timings.retry); // offline, expired token, busy: later
        }
    }

    // ---- appearance sync ---------------------------------------------------------------------------------------------

    /// "Sync appearance": newer wins. Never Peak ceiling, Sync safety, Line-up limit or Bus name.
    pub(super) fn do_sync_appearance(&self) {
        let (local, local_at, dirty, seen) = {
            let c = self.core.relock();
            if !c.profile.sync_appearance {
                return;
            }
            (
                c.local_appearance.clone(),
                c.local_changed_at,
                c.local_dirty,
                c.remote_seen_at,
            )
        };
        let res = self.authed(|t| self.call("GET", "/api/v1/me/settings", None, t));
        let v = res.json();
        if !res.ok() || !v.is_object() {
            return;
        }
        let remote_at = iso_ms(&v["updatedAt"]);
        let remote = &v["appearance"];
        let remote_has = remote.as_object().is_some_and(|o| !o.is_empty());
        if dirty
            && let Some(local) = local.as_ref()
            && (local_at > remote_at || !remote_has)
        {
            let stems = if v["stemNames"].is_array() {
                v["stemNames"].clone()
            } else {
                json!([])
            };
            let body = json!({"appearance": appearance_json(local), "stemNames": stems});
            let put = self.authed(|t| self.call("PUT", "/api/v1/me/settings", Some(&body), t));
            if put.ok() {
                let mut c = self.core.relock();
                if c.local_changed_at == local_at {
                    c.local_dirty = false; // no newer change meanwhile
                }
                c.remote_seen_at = iso_ms(&put.json()["updatedAt"]);
            }
            return;
        }
        if remote_has && remote_at > seen {
            let mut c = self.core.relock();
            c.remote_seen_at = remote_at;
            c.local_dirty = false;
            c.push(AccountEvent::AppearanceFromAccount(appearance_from_json(
                remote,
            )));
        }
    }

    // ---- the one-second tick ----------------------------------------------------------------------------------------

    pub(super) fn do_tick(&self) {
        // 1. file watch: did another plug-in sign in or out?
        let known = self.core.relock().last_mtime;
        let now_mtime = self.file_mtime();
        if now_mtime != known {
            let d = self.read_disk();
            let mut c = self.core.relock();
            self.apply_disk(&mut c, d);
            c.notify_profile();
        }
        // 2. countdown of the sign-in code
        let mut c = self.core.relock();
        if c.state == State::WaitingForCode && !c.code.user_code.is_empty() {
            if c.code_remaining() <= 0 {
                if !c.code.expired {
                    c.code.expired = true;
                    c.notify_code();
                }
                c.poll = None;
            } else {
                c.notify_code();
            }
        }
    }
}
