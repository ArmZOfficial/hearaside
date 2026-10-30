//! `ui-snapshot --test-account` and `--test-account-flow` for the Rust AccountManager, against a fake account server on
//! 127.0.0.1 that speaks the `web/site` API (`server/routes/device.ts`, `me.ts`), plus the bug regressions.
mod common;
use common::{Fake, Req, Resp, TempDir, holds_for, settled, wait_for};
use hs_app::{
    AccountConfig, AccountEvent, AccountListener, AccountManager, Appearance, ClientMeta,
    DeviceCode, Language, Profile, ProfileError, ProfilePatch, State, ThemeMode, Timings,
};
use serde_json::{Value, json};
use std::sync::atomic::{AtomicU32, Ordering};
use std::sync::{Arc, Mutex};
use std::time::Duration;

struct S {
    seq: u32,
    access: String,
    refresh: String,
    polls: u32,
    approve_after: u32,
    deny: bool,
    token_error: Option<&'static str>,
    token_http_status: Option<u16>,
    start_status: u16,
    code_ttl: i64,
    access_ttl: i64,
    me: Value,
    patch_me_fail: u32,
    settings: Value,
    refreshes: u32,
}

impl S {
    fn new() -> Self {
        S {
            seq: 0,
            access: String::new(),
            refresh: String::new(),
            polls: 0,
            approve_after: 0,
            deny: false,
            token_error: None,
            token_http_status: None,
            start_status: 200,
            code_ttl: 600,
            access_ttl: 900,
            me: json!({"id": "u1", "email": "mint@example.com", "handle": "flowtest", "displayName": "Mint", "about": "hi",
                       "avatarUrl": null, "locale": "th", "hideEmail": true, "plan": "free"}),
            patch_me_fail: 0,
            settings: json!({"appearance": {}, "stemNames": [], "updatedAt": null}),
            refreshes: 0,
        }
    }
}

fn server() -> (Arc<Mutex<S>>, Fake) {
    let s = Arc::new(Mutex::new(S::new()));
    let s2 = Arc::clone(&s);
    let fake = Fake::start(move |r| handle(&mut s2.lock().unwrap(), r));
    (s, fake)
}

fn handle(s: &mut S, r: &Req) -> Resp {
    let authed =
        r.header("authorization") == Some(&format!("Bearer {}", s.access)) && !s.access.is_empty();
    let unauth = || Resp::json(401, json!({"error": "unauthorized"}));
    match (r.method.as_str(), r.path.as_str()) {
        ("POST", "/api/v1/device/start") if s.start_status != 200 => {
            Resp::json(s.start_status, json!({"error": "x"}))
        }
        ("POST", "/api/v1/device/start") => Resp::json(
            200,
            json!({"device_code": "D".repeat(43), "user_code": "KQ7M-2TXP", "verification_uri": "https://hearaside.example/link",
                   "verification_uri_complete": "https://hearaside.example/link?code=KQ7M-2TXP", "expires_in": s.code_ttl, "interval": 2}),
        ),
        ("POST", "/api/v1/device/token") => {
            s.polls += 1;
            if let Some(st) = s.token_http_status {
                return Resp::json(st, json!({"error": "slow_down"}));
            }
            if let Some(e) = s.token_error {
                return Resp::json(400, json!({"error": e}));
            }
            if s.deny {
                return Resp::json(400, json!({"error": "access_denied"}));
            }
            if s.polls <= s.approve_after {
                return Resp::json(400, json!({"error": "authorization_pending"}));
            }
            s.seq += 1;
            s.access = format!("access-{}", s.seq);
            s.refresh = format!("refresh-{}", s.seq);
            Resp::json(
                200,
                json!({"token_type": "Bearer", "access_token": s.access, "expires_in": s.access_ttl, "refresh_token": s.refresh,
                                   "device_id": "dev-1", "user": s.me}),
            )
        }
        ("POST", "/api/v1/token/refresh") => {
            if r.json()["refresh_token"] != s.refresh.as_str() || s.refresh.is_empty() {
                return Resp::json(401, json!({"error": "invalid_grant"}));
            }
            s.refreshes += 1;
            s.seq += 1;
            s.access = format!("access-{}", s.seq);
            s.refresh = format!("refresh-{}", s.seq);
            Resp::json(
                200,
                json!({"token_type": "Bearer", "access_token": s.access, "expires_in": 900, "refresh_token": s.refresh}),
            )
        }
        _ if !authed => unauth(),
        ("GET", "/api/v1/me") => Resp::json(200, s.me.clone()),
        ("PATCH", "/api/v1/me") => {
            if s.patch_me_fail > 0 {
                s.patch_me_fail -= 1;
                return Resp::json(500, json!({"error": "boom"}));
            }
            let body = r.json();
            if body["handle"] == "taken" {
                return Resp::json(409, json!({"error": "handle_taken"}));
            }
            for (k, v) in body.as_object().unwrap() {
                s.me[k] = v.clone();
            }
            Resp::json(200, s.me.clone())
        }
        ("POST", "/api/v1/device/revoke") => {
            s.access.clear();
            Resp::json(200, json!({"ok": true}))
        }
        ("PATCH", "/api/v1/device") => Resp::json(200, json!({"ok": true})),
        ("POST", "/api/v1/me/avatar") => {
            s.me["avatarUrl"] = json!("https://hearaside.example/a/u1.webp");
            Resp::json(200, s.me.clone())
        }
        ("DELETE", "/api/v1/me/avatar") => {
            s.me["avatarUrl"] = Value::Null;
            Resp::json(200, s.me.clone())
        }
        ("GET", "/api/v1/me/settings") => Resp::json(200, s.settings.clone()),
        ("PUT", "/api/v1/me/settings") => {
            let b = r.json();
            s.settings = json!({"appearance": b["appearance"], "stemNames": b["stemNames"], "updatedAt": "2026-10-30T12:00:00.000Z"});
            Resp::json(200, s.settings.clone())
        }
        _ => Resp::json(404, json!({"error": "not_found"})),
    }
}

static N: AtomicU32 = AtomicU32::new(0);

fn config(dir: &TempDir, base: &str) -> AccountConfig {
    AccountConfig {
        server_base: Some(base.to_owned()),
        storage_file: Some(dir.file("account.dat")),
        lock_name: Some(format!(
            "HEARASIDE_test_account_{}_{}",
            std::process::id(),
            N.fetch_add(1, Ordering::SeqCst)
        )),
        client: ClientMeta {
            version: "1.2.3".into(),
            os: "TestOS".into(),
            daw: "TestDAW".into(),
            computer: "TestPC".into(),
        },
        timings: Timings {
            poll_interval: Some(Duration::from_millis(40)),
            periodic: Duration::from_secs(60),
            tick: Duration::from_millis(50),
            retry: Duration::from_millis(100),
            first_periodic: Duration::ZERO,
        },
    }
}

fn state_is(m: &AccountManager, s: State, ms: u64) -> bool {
    wait_for(ms, || m.state() == s)
}

fn sign_in(m: &AccountManager) {
    m.start_device_flow("hub");
    assert!(
        state_is(m, State::SignedIn, 5000),
        "signed in once the browser allows it, state {:?}",
        m.state()
    );
}

#[derive(Default)]
struct Recorder(Mutex<Vec<String>>, Mutex<Option<std::thread::ThreadId>>);

impl AccountListener for Recorder {
    fn account_state_changed(&self, s: State) {
        self.0.lock().unwrap().push(format!("state {s:?}"));
        *self.1.lock().unwrap() = Some(std::thread::current().id());
    }
    fn account_profile_changed(&self, p: &Profile) {
        self.0.lock().unwrap().push(format!("profile {}", p.handle));
    }
    fn account_device_code_updated(&self, c: &DeviceCode) {
        self.0.lock().unwrap().push(format!("code {}", c.user_code));
    }
}

// ---- testAccount ---------------------------------------------------------------------------------------------------

/// `isValidCode` of `testAccount`: XXXX-XXXX, base32 letters and digits without 0 O 1 I.
fn is_valid_code(code: &str) -> bool {
    let c: Vec<char> = code.chars().collect();
    c.len() == 9
        && c[4] == '-'
        && c.iter().enumerate().all(|(i, &ch)| {
            i == 4 || matches!(ch, '2'..='9') || (ch.is_ascii_uppercase() && ch != 'O' && ch != 'I')
        })
}

#[test]
fn starts_signed_out_with_a_server_url() {
    let d = TempDir::new("acc-init");
    let (_s, fake) = server();
    let m = AccountManager::new(config(&d, &format!("{}/", fake.base())));
    assert!(
        matches!(m.state(), State::SignedOut | State::Offline),
        "account starts signed-out or offline"
    );
    assert!(
        m.server_base_url().starts_with("http"),
        "serverBaseUrl defaults to valid URL"
    );
    assert_eq!(
        m.server_base_url(),
        fake.base(),
        "a trailing slash is dropped"
    );
    assert!(!m.is_signed_in() && m.access_token().is_empty() && m.masked_email().is_empty());
    // no explicit server: the default (or $HEARASIDE_API_URL), still http(s)
    let mut c = config(&d, "");
    c.server_base = None;
    assert!(AccountManager::new(c).server_base_url().starts_with("http"));
    assert!(
        is_valid_code("ABCD-EFGH") && !is_valid_code("AB0D-EFGH") && !is_valid_code("ABID-EFGH"),
        "device code format regex check"
    );
}

// ---- testAccountFlow -----------------------------------------------------------------------------------------------

#[test]
fn device_flow_signs_in_and_out() {
    let d = TempDir::new("acc-flow");
    let (s, fake) = server();
    s.lock().unwrap().approve_after = 2;
    let m = AccountManager::new(config(&d, &fake.base()));
    let rec = Arc::new(Recorder::default());
    m.add_listener(rec.clone());
    assert_eq!(m.state(), State::SignedOut, "starts signed out");
    m.start_device_flow("HEARASIDE Hub");
    assert_eq!(m.state(), State::WaitingForCode);
    assert!(
        m.device_code().user_code.is_empty() && m.code_remaining_seconds() == 600,
        "no code yet: nothing counts down"
    );
    assert!(
        wait_for(3000, || !m.device_code().user_code.is_empty()),
        "the code arrives"
    );
    let code = m.device_code();
    assert!(
        is_valid_code(&code.user_code)
            && code.verification_uri_complete.ends_with("?code=KQ7M-2TXP")
    );
    assert!(
        m.code_remaining_seconds() > 595 && m.code_remaining_seconds() <= 600,
        "the code is still counting down"
    );
    assert!(
        state_is(&m, State::SignedIn, 5000),
        "signed in once the browser allows it"
    );
    assert_eq!(
        m.profile().handle,
        "flowtest",
        "profile from the token reply"
    );
    assert_eq!(m.profile().display_name, "Mint");
    assert!(
        m.device_code().user_code.is_empty(),
        "the code is dropped once used"
    );
    assert_eq!(
        fake.count("POST /api/v1/device/token"),
        3,
        "two pending answers, then the tokens"
    );

    // what the plug-in told the server about itself
    let start = fake
        .requests("POST /api/v1/device/start")
        .pop()
        .unwrap()
        .json();
    assert_eq!(
        start["client_info"],
        json!({"plugin": "hub", "version": "1.2.3", "os": "TestOS", "daw": "TestDAW", "computer": "TestPC"})
    );
    assert_eq!(
        fake.requests("POST /api/v1/device/token")[0].json(),
        json!({"device_code": "D".repeat(43)})
    );
    assert_eq!(
        fake.requests("POST /api/v1/device/token")[0].header("authorization"),
        None
    );

    // the listeners hear it, in order, on the thread that dispatches
    m.dispatch_events();
    let heard = rec.0.lock().unwrap().clone();
    let pos = |what: &str| {
        heard
            .iter()
            .position(|h| h == what)
            .unwrap_or_else(|| panic!("{what} missing in {heard:?}"))
    };
    assert!(
        pos("state WaitingForCode") < pos("code KQ7M-2TXP")
            && pos("code KQ7M-2TXP") < pos("state SignedIn")
            && pos("state SignedIn") < pos("profile flowtest")
    );
    assert_eq!(
        *rec.1.lock().unwrap(),
        Some(std::thread::current().id()),
        "callbacks run on the dispatching thread"
    );
    m.dispatch_events(); // nothing new queued of those kinds
    assert!(m.is_signed_in());

    // the session is stored, protected, and a new manager (another plug-in) picks it up without any network
    let raw = std::fs::read(d.file("account.dat")).unwrap();
    assert!(
        !String::from_utf8_lossy(&raw).contains("access-1"),
        "tokens are not in the file as plain text"
    );
    let other = AccountManager::new(config(&d, "http://127.0.0.1:9"));
    assert_eq!(other.state(), State::SignedIn);
    assert_eq!(other.profile().handle, "flowtest");
    drop(other);

    // the signed-in page keeps its rows when the profile refreshes
    let before = fake.count("GET /api/v1/me");
    m.refresh_profile();
    assert!(wait_for(3000, || fake.count("GET /api/v1/me") > before));
    assert_eq!(
        fake.requests("GET /api/v1/me")
            .pop()
            .unwrap()
            .header("authorization"),
        Some("Bearer access-1")
    );
    assert!(
        wait_for(2000, || m.profile().id == "u1")
            && m.profile().handle == "flowtest"
            && m.state() == State::SignedIn
    );
    assert_eq!(m.masked_email(), "m\u{2022}\u{2022}\u{2022}@example.com");
    assert!(!m.access_token().is_empty(), "good for more than 30 s");

    m.sign_out();
    assert!(state_is(&m, State::SignedOut, 3000), "signed out again");
    assert_eq!(fake.count("POST /api/v1/device/revoke"), 1);
    assert_eq!(
        fake.requests("POST /api/v1/device/revoke")[0].header("authorization"),
        Some("Bearer access-1")
    );
    assert!(
        !d.file("account.dat").exists(),
        "the stored session is gone"
    );
    assert!(m.access_token().is_empty() && m.profile() == Profile::default());
    assert_eq!(
        AccountManager::new(config(&d, &fake.base())).state(),
        State::SignedOut
    );
}

#[test]
fn plugin_names_map_to_what_the_server_takes() {
    let d = TempDir::new("acc-names");
    let (_s, fake) = server();
    let m = AccountManager::new(config(&d, &fake.base()));
    for (given, sent) in [
        ("hub", "hub"),
        ("HEARASIDE Track", "track"),
        ("App Audio", "app"),
        ("something else", "track"),
        ("", "track"),
        ("HEARASIDE HUB", "hub"),
    ] {
        let before = fake.count("POST /api/v1/device/start");
        m.start_device_flow(given);
        assert!(
            wait_for(2000, || fake.count("POST /api/v1/device/start") > before),
            "{given}"
        );
        assert_eq!(
            fake.requests("POST /api/v1/device/start")
                .pop()
                .unwrap()
                .json()["client_info"]["plugin"],
            sent,
            "{given}"
        );
        m.cancel_device_flow();
    }
}

#[test]
fn bad_sign_in_endings() {
    let d = TempDir::new("acc-ends");
    let (s, fake) = server();
    let m = AccountManager::new(config(&d, &fake.base()));
    // the server cannot give a code
    s.lock().unwrap().start_status = 503;
    m.start_device_flow("hub");
    assert!(
        wait_for(3000, || m.start_failed()) && m.state() == State::SignedOut,
        "no code: failed, not 'offline'"
    );
    assert!(
        m.take_events()
            .contains(&AccountEvent::StateChanged(State::SignedOut))
    );
    // the user says no
    s.lock().unwrap().start_status = 200;
    s.lock().unwrap().deny = true;
    m.start_device_flow("hub");
    assert!(!m.start_failed(), "a new try clears the failure");
    assert!(state_is(&m, State::WaitingForCode, 1000));
    assert!(
        wait_for(3000, || m.state() == State::SignedOut) && !m.start_failed(),
        "denied: signed out, not a failure"
    );
    let polls = settled(150, || fake.count("POST /api/v1/device/token"));
    assert!(
        holds_for(400, || fake.count("POST /api/v1/device/token") == polls),
        "no more polling after the answer"
    );
    // the code was used already
    s.lock().unwrap().deny = false;
    s.lock().unwrap().token_error = Some("invalid_grant");
    m.start_device_flow("hub");
    assert!(wait_for(3000, || m.start_failed()) && m.state() == State::SignedOut);
    // the server says the code ran out
    s.lock().unwrap().token_error = Some("expired_token");
    m.start_device_flow("hub");
    assert!(
        wait_for(3000, || m.device_code().expired),
        "expired_token marks the code expired"
    );
    assert_eq!(
        m.state(),
        State::WaitingForCode,
        "the panel keeps showing it, with 'get a new code'"
    );
    assert_eq!(m.code_remaining_seconds(), 0);
    // get a new code
    s.lock().unwrap().token_error = None;
    m.start_device_flow("hub");
    assert!(state_is(&m, State::SignedIn, 5000));
}

#[test]
fn cancel_stops_polling_and_a_late_answer_is_ignored() {
    let d = TempDir::new("acc-cancel");
    let (s, fake) = server();
    s.lock().unwrap().approve_after = 1_000_000;
    let m = AccountManager::new(config(&d, &fake.base()));
    m.start_device_flow("hub");
    assert!(wait_for(3000, || fake.count("POST /api/v1/device/token") >= 2));
    m.cancel_device_flow();
    assert_eq!(m.state(), State::SignedOut);
    let n = settled(150, || fake.count("POST /api/v1/device/token"));
    assert!(
        holds_for(400, || fake.count("POST /api/v1/device/token") == n),
        "cancelled: no more polls"
    );
    // cancelling while a code is being asked for: the answer must not bring the flow back
    fake.hang.store(true, Ordering::SeqCst);
    m.start_device_flow("hub");
    assert!(wait_for(2000, || fake.count("POST /api/v1/device/start") >= 2));
    m.cancel_device_flow();
    fake.hang.store(false, Ordering::SeqCst);
    assert!(
        holds_for(600, || m.state() == State::SignedOut
            && m.device_code().user_code.is_empty()),
        "the late answer does not bring the flow back"
    );
}

#[test]
fn bug_polling_stops_when_the_code_runs_out() {
    // C++ bug: the poll loop only stopped on an answer from the server; with the server down (or just never approving)
    // it asked every few seconds for as long as the plug-in lived, long after the code expired on screen.
    let d = TempDir::new("acc-expiry");
    let (s, fake) = server();
    {
        let mut s = s.lock().unwrap();
        s.approve_after = 1_000_000;
        s.code_ttl = 1;
    }
    let m = AccountManager::new(config(&d, &fake.base()));
    m.start_device_flow("hub");
    assert!(
        wait_for(4000, || m.device_code().expired),
        "the countdown ends"
    );
    assert!(
        m.take_events()
            .iter()
            .any(|e| matches!(e, AccountEvent::DeviceCodeUpdated(c) if c.expired))
    );
    let n = settled(150, || fake.count("POST /api/v1/device/token"));
    assert!(n >= 1);
    assert!(holds_for(400, || fake.count("POST /api/v1/device/token") == n));
    assert_eq!(
        fake.count("POST /api/v1/device/token"),
        n,
        "an expired code is not polled any more"
    );
    // a new code starts it again
    s.lock().unwrap().code_ttl = 600;
    m.start_device_flow("hub");
    assert!(wait_for(3000, || fake.count("POST /api/v1/device/token") > n));
}

#[test]
fn slow_down_backs_off() {
    let d = TempDir::new("acc-slow");
    let (s, fake) = server();
    s.lock().unwrap().token_error = Some("slow_down");
    let m = AccountManager::new(config(&d, &fake.base()));
    m.start_device_flow("hub");
    assert!(wait_for(3000, || fake.count("POST /api/v1/device/token") == 1));
    assert!(
        holds_for(500, || fake.count("POST /api/v1/device/token") == 1),
        "five seconds more between polls after slow_down"
    );
}

#[test]
fn bug_no_tokens_over_plain_http_to_another_computer() {
    // C++ bug: an http:// server address (a typo, a setting, an environment variable) got the refresh and access
    // tokens in clear text. Now only https, or this computer.
    let d = TempDir::new("acc-http");
    let (_s, fake) = server();
    let m = AccountManager::new(config(&d, &fake.base()));
    m.set_server_base_url("http://192.0.2.7:8080/");
    assert_eq!(m.server_base_url(), "http://192.0.2.7:8080");
    m.start_device_flow("hub");
    assert!(
        wait_for(1000, || m.start_failed()),
        "refused at once, no connect attempt"
    );
    assert_eq!(fake.count("POST"), 0);
}

#[test]
fn bug_a_non_windows_plain_file_is_never_trusted_on_windows() {
    // DPAPI: a file that is not a blob of this user (plain JSON, another user's, damaged) means signed out.
    if !cfg!(windows) {
        println!("SKIPPED: plain JSON is the stored format outside Windows");
        return;
    }
    let d = TempDir::new("acc-plain");
    std::fs::write(d.file("account.dat"), r#"{"access_token":"stolen","refresh_token":"r","device_id":"d","expires_at":99999999999999,"user":{"handle":"x"}}"#).unwrap();
    let m = AccountManager::new(config(&d, "http://127.0.0.1:9"));
    assert_eq!(m.state(), State::SignedOut);
    assert!(m.access_token().is_empty());
}

#[cfg(unix)]
#[test]
fn bug_token_file_is_private_from_the_first_byte() {
    // C++ bug: the file was written with the default permissions and chmod-ed afterwards.
    use std::os::unix::fs::PermissionsExt;
    let d = TempDir::new("acc-mode");
    let (_s, fake) = server();
    let m = AccountManager::new(config(&d, &fake.base()));
    sign_in(&m);
    let mode = std::fs::metadata(d.file("account.dat"))
        .unwrap()
        .permissions()
        .mode()
        & 0o777;
    assert_eq!(mode, 0o600);
}

// ---- tokens ---------------------------------------------------------------------------------------------------------

#[test]
fn an_old_token_is_refreshed_and_the_rotated_one_is_stored() {
    let d = TempDir::new("acc-refresh");
    let (s, fake) = server();
    s.lock().unwrap().access_ttl = 30; // under the 2 minutes a token must still have
    {
        let m = AccountManager::new(config(&d, &fake.base()));
        sign_in(&m);
        assert!(
            m.access_token().is_empty(),
            "less than 30 s left: not handed out"
        );
    }
    let m = AccountManager::new(config(&d, &fake.base())); // the next run: its first periodic round refreshes
    assert!(
        wait_for(5000, || !m.access_token().is_empty()),
        "a fresh access token"
    );
    assert_eq!(s.lock().unwrap().refreshes, 1);
    assert_eq!(m.access_token(), s.lock().unwrap().access);
    // the rotated pair is in the file: a third run needs no refresh at all
    assert!(
        wait_for(3000, || AccountManager::new(config(
            &d,
            "http://127.0.0.1:9"
        ))
        .access_token()
            == m.access_token()),
        "the next run reads the rotated token from the file"
    );
    assert!(holds_for(300, || s.lock().unwrap().refreshes == 1));
}

#[test]
fn two_plugins_refresh_one_after_the_other() {
    // a refresh token works once: the second plug-in must see that the first has refreshed, not use the old one
    if !cfg!(windows) {
        println!(
            "SKIPPED: the refresh lock is a per-process fcntl lock outside Windows, two managers in one process do not exclude each other"
        );
        return;
    }
    let d = TempDir::new("acc-race");
    let (s, fake) = server();
    s.lock().unwrap().access_ttl = 30;
    {
        let m = AccountManager::new(config(&d, &fake.base()));
        sign_in(&m);
    }
    let mut ca = config(&d, &fake.base());
    let mut cb = config(&d, &fake.base());
    ca.lock_name = Some("HEARASIDE_test_account_shared_lock".into());
    cb.lock_name = ca.lock_name.clone();
    let (a, b) = (AccountManager::new(ca), AccountManager::new(cb));
    assert!(
        wait_for(8000, || !a.access_token().is_empty()
            && !b.access_token().is_empty()),
        "both end up with a good token"
    );
    assert_eq!(s.lock().unwrap().refreshes, 1, "one refresh for both");
    assert!(
        a.is_signed_in() && b.is_signed_in(),
        "nobody was signed out for 'reusing' a token"
    );
    assert_eq!(a.access_token(), b.access_token());
}

#[test]
fn a_revoked_refresh_token_signs_this_computer_out() {
    let d = TempDir::new("acc-revoked");
    let (s, fake) = server();
    s.lock().unwrap().access_ttl = 30;
    {
        let m = AccountManager::new(config(&d, &fake.base()));
        sign_in(&m);
    }
    s.lock().unwrap().refresh = "someone-else".into(); // the website revoked this device, or an old token came back
    let m = AccountManager::new(config(&d, &fake.base()));
    assert!(state_is(&m, State::SignedOut, 5000));
    assert!(!d.file("account.dat").exists());
    assert_eq!(
        fake.count("POST /api/v1/device/revoke"),
        0,
        "nothing to revoke with a token the server already refused"
    );
    assert!(m.access_token().is_empty() && m.profile() == Profile::default());
}

#[test]
fn a_401_refreshes_and_asks_again() {
    let d = TempDir::new("acc-401");
    let (s, fake) = server();
    let m = AccountManager::new(config(&d, &fake.base()));
    sign_in(&m);
    // another plug-in refreshed meanwhile: our access token is no longer the server's
    {
        let mut s = s.lock().unwrap();
        s.seq += 1;
        s.access = format!("access-{}", s.seq);
    }
    // the server accepts our refresh token still (it did not change), so this refresh yields a new pair
    m.refresh_profile();
    assert!(
        wait_for(5000, || s.lock().unwrap().refreshes == 1),
        "refreshed after the 401"
    );
    assert!(
        wait_for(3000, || fake
            .requests("GET /api/v1/me")
            .last()
            .is_some_and(|r| r.header("authorization")
                == Some(&format!("Bearer {}", s.lock().unwrap().access)))),
        "the request was repeated with the new token"
    );
    assert!(m.is_signed_in());
}

#[test]
fn other_plugins_signing_out_signs_this_one_out() {
    // C++ bug: when another plug-in removed the file, this one noticed but kept the tokens in memory (and kept using them).
    let d = TempDir::new("acc-removed");
    let (_s, fake) = server();
    let m = AccountManager::new(config(&d, &fake.base()));
    sign_in(&m);
    assert!(!m.access_token().is_empty());
    std::fs::remove_file(d.file("account.dat")).unwrap();
    assert!(state_is(&m, State::SignedOut, 3000));
    assert!(m.access_token().is_empty(), "no token left to use");
    assert_eq!(m.profile(), Profile::default());
}

#[test]
fn another_plugin_signing_in_ends_the_wait() {
    let d = TempDir::new("acc-shared");
    let (s, fake) = server();
    s.lock().unwrap().approve_after = 1_000_000;
    let waiting = AccountManager::new(config(&d, &fake.base()));
    waiting.start_device_flow("track");
    assert!(state_is(&waiting, State::WaitingForCode, 1000));
    assert!(wait_for(2000, || !waiting
        .device_code()
        .user_code
        .is_empty()));
    // a second plug-in (another process in real life) signs in and writes the file
    s.lock().unwrap().approve_after = 0;
    let other = AccountManager::new(config(&d, &fake.base()));
    sign_in(&other);
    assert!(
        state_is(&waiting, State::SignedIn, 3000),
        "the waiting plug-in is signed in too, state {:?}",
        waiting.state()
    );
    assert_eq!(waiting.profile().handle, "flowtest");
    assert!(waiting.device_code().user_code.is_empty());
}

// ---- profile --------------------------------------------------------------------------------------------------------

#[test]
fn profile_edits() {
    let d = TempDir::new("acc-edit");
    let (s, fake) = server();
    let m = AccountManager::new(config(&d, &fake.base()));
    sign_in(&m);
    m.take_events();

    m.update_profile(ProfilePatch {
        display_name: Some("Mint 2".into()),
        ..ProfilePatch::default()
    })
    .unwrap();
    assert!(wait_for(3000, || m.profile().display_name == "Mint 2"));
    let body = fake.requests("PATCH /api/v1/me").pop().unwrap().json();
    assert_eq!(
        body,
        json!({"displayName": "Mint 2"}),
        "only what changed is sent"
    );
    assert_eq!(
        m.profile().email,
        "mint@example.com",
        "an answer with fewer fields never blanks the rest"
    );

    // C++ bug: an empty text meant "leave it", so 'About you' could never be cleared
    m.update_profile(ProfilePatch {
        about: Some(String::new()),
        ..ProfilePatch::default()
    })
    .unwrap();
    assert!(wait_for(3000, || m.profile().about.is_empty()
        && s.lock().unwrap().me["about"] == ""));
    assert_eq!(
        fake.requests("PATCH /api/v1/me").pop().unwrap().json(),
        json!({"about": ""})
    );

    // a refused change is reported, the profile stays
    m.take_events();
    m.update_profile(ProfilePatch {
        handle: Some("taken".into()),
        ..ProfilePatch::default()
    })
    .unwrap();
    assert!(wait_for(3000, || {
        m.take_events().iter().any(|e| matches!(e, AccountEvent::RequestFailed { what: "profile", status: 409, error } if error == "handle_taken"))
    }));
    assert_eq!(m.profile().handle, "flowtest");

    // nothing to change: nothing sent
    let n = fake.count("PATCH /api/v1/me");
    m.update_profile(ProfilePatch::default()).unwrap();
    m.set_device_name("  Studio PC  ");
    assert!(wait_for(3000, || m.profile().device_name == "Studio PC"));
    assert_eq!(fake.count("PATCH /api/v1/me"), n);
    assert_eq!(
        fake.requests("PATCH /api/v1/device")[0].json(),
        json!({"name": "Studio PC"})
    );
    // the device name is kept in the file for the next run
    assert!(wait_for(3000, || AccountManager::new(config(
        &d,
        "http://127.0.0.1:9"
    ))
    .profile()
    .device_name
        == "Studio PC"));
}

#[test]
fn bug_hide_email_choice_is_not_lost_while_the_server_does_not_take_it() {
    // C++ bug: the choice was sent once; if that failed (offline, token about to expire, a 5xx) the next profile refresh
    // brought the server's old value back and the email showed on a stream again.
    let d = TempDir::new("acc-hide");
    let (s, fake) = server();
    let m = AccountManager::new(config(&d, &fake.base()));
    sign_in(&m);
    assert!(m.profile().hide_email);
    s.lock().unwrap().patch_me_fail = 2;
    m.set_hide_email(false);
    assert!(
        !m.profile().hide_email && m.masked_email() == "mint@example.com",
        "takes effect at once"
    );
    assert!(wait_for(3000, || fake.count("PATCH /api/v1/me") >= 1));
    m.refresh_profile(); // the server still says hideEmail: true
    assert!(wait_for(3000, || fake.count("GET /api/v1/me") >= 1));
    assert!(
        holds_for(300, || !m.profile().hide_email),
        "the server's older answer does not undo it"
    );
    assert!(
        wait_for(5000, || s.lock().unwrap().me["hideEmail"] == false),
        "retried until the server has it"
    );
    assert_eq!(fake.count("PATCH /api/v1/me"), 3);
    // saved to the file as well
    assert!(
        wait_for(3000, || !AccountManager::new(config(
            &d,
            "http://127.0.0.1:9"
        ))
        .profile()
        .hide_email),
        "saved to the file by the worker"
    );
    // and now the server's value rules again
    s.lock().unwrap().me["hideEmail"] = json!(true);
    m.refresh_profile();
    assert!(wait_for(3000, || m.profile().hide_email));
}

#[test]
fn avatar_upload_and_remove() {
    let d = TempDir::new("acc-avatar");
    let (_s, fake) = server();
    let m = AccountManager::new(config(&d, &fake.base()));
    sign_in(&m);
    // a real-looking PNG (only the signature matters: the website checks the content)
    let mut png = vec![0x89, b'P', b'N', b'G', 0x0d, 0x0a, 0x1a, 0x0a];
    png.extend((0..300u32).map(|i| (i * 7) as u8));
    std::fs::write(d.file("me.png"), &png).unwrap();
    m.upload_avatar(d.file("me.png"));
    assert!(wait_for(3000, || m
        .profile()
        .avatar_url
        .ends_with("u1.webp")));
    let up = fake.requests("POST /api/v1/me/avatar").pop().unwrap();
    assert_eq!(up.header("authorization"), Some("Bearer access-1"));
    let ct = up.header("content-type").unwrap();
    let boundary = ct
        .strip_prefix("multipart/form-data; boundary=")
        .expect("multipart");
    let text = String::from_utf8_lossy(&up.body).into_owned();
    assert!(
        text.contains("name=\"photo\"")
            && text.contains("Content-Type: image/png")
            && text.ends_with(&format!("--{boundary}--\r\n"))
    );
    assert!(
        up.body.windows(png.len()).any(|w| w == png.as_slice()),
        "the file bytes go as they are"
    );

    m.remove_avatar();
    assert!(wait_for(3000, || m.profile().avatar_url.is_empty()));
    assert_eq!(fake.count("DELETE /api/v1/me/avatar"), 1);

    // C++ bug: any answer counted as success (and a refusal's JSON was read as the profile); a file that is not a picture
    // or is too big is now refused here, without a request
    m.take_events();
    std::fs::write(d.file("note.txt"), b"hello").unwrap();
    std::fs::write(d.file("big.jpg"), vec![0xff; 5 * 1024 * 1024 + 1]).unwrap();
    for (f, code) in [
        ("note.txt", "bad_image"),
        ("big.jpg", "too_big"),
        ("missing.png", "bad_image"),
    ] {
        m.upload_avatar(d.file(f));
        assert!(
            wait_for(3000, || {
                m.take_events().iter().any(|e| matches!(e, AccountEvent::RequestFailed { what: "avatar", error, .. } if error == code))
            }),
            "{f}"
        );
    }
    assert_eq!(fake.count("POST /api/v1/me/avatar"), 1);
    assert_eq!(m.profile().handle, "flowtest");
}

#[test]
fn appearance_sync_up_and_down() {
    let d = TempDir::new("acc-sync");
    let (s, fake) = server();
    let m = AccountManager::new(config(&d, &fake.base()));
    sign_in(&m);
    let mine = Appearance {
        language: Some(Language::Thai),
        theme: Some(ThemeMode::Dark),
        ui_scale: Some(1.25),
        glass_alpha: Some(0.5),
        host_colours: Some(true),
        reduce_motion: Some(false),
    };
    // the account has none yet: this computer's look goes up
    m.set_sync_appearance(true, Some(mine.clone()));
    assert!(m.profile().sync_appearance);
    assert!(wait_for(3000, || fake.count("PUT /api/v1/me/settings") == 1));
    let put = fake.requests("PUT /api/v1/me/settings")[0].json();
    assert_eq!(
        put["appearance"],
        json!({"language": "th", "theme": "dark", "uiScale": 1.25, "glassAlpha": 0.5, "hostColours": true, "reduceMotion": false})
    );
    assert_eq!(put["stemNames"], json!([]));
    // a local change goes up (newer than the account's copy, from 2026-10-30T12:00:00Z: now is later)
    m.appearance_changed(Appearance {
        theme: Some(ThemeMode::Light),
        ..mine.clone()
    });
    assert!(wait_for(3000, || fake.count("PUT /api/v1/me/settings") == 2));
    assert_eq!(
        fake.requests("PUT /api/v1/me/settings")[1].json()["appearance"]["theme"],
        "light"
    );
    // the account gets a newer look from the website: it comes down as an event
    m.take_events();
    s.lock().unwrap().settings = json!({"appearance": {"language": "en", "theme": "system", "uiScale": 1.5}, "stemNames": ["Vox"], "updatedAt": "2099-01-01T00:00:00.000Z"});
    m.refresh_profile();
    {
        // the periodic round does the sync; drive it with a manager whose period is short
        let d2 = TempDir::new("acc-sync2");
        let mut c = config(&d2, &fake.base());
        c.timings.periodic = Duration::from_millis(200);
        let m2 = AccountManager::new(c);
        s.lock().unwrap().approve_after = 0;
        sign_in(&m2);
        m2.set_sync_appearance(true, Some(mine.clone()));
        let got = wait_for(5000, || {
            m2.take_events().iter().any(|e| {
                *e == AccountEvent::AppearanceFromAccount(Appearance {
                    language: Some(Language::English),
                    theme: Some(ThemeMode::Auto),
                    ui_scale: Some(1.5),
                    ..Appearance::default()
                })
            })
        });
        assert!(
            got,
            "the newer look of the account comes down, only the fields it has"
        );
        let puts = fake.count("PUT /api/v1/me/settings");
        assert!(
            holds_for(800, || fake.count("PUT /api/v1/me/settings") == puts),
            "and is not sent back up"
        );
    }
    // off: no more local changes recorded
    m.set_sync_appearance(false, None);
    let n = fake.count("PUT /api/v1/me/settings");
    m.appearance_changed(mine);
    assert!(holds_for(300, || fake.count("PUT /api/v1/me/settings") == n));
}

#[test]
fn server_down_means_offline_and_back() {
    let d = TempDir::new("acc-offline");
    let (_s, fake) = server();
    let m = AccountManager::new(config(&d, &fake.base()));
    sign_in(&m);
    // the server goes away (a port nothing listens on)
    m.set_server_base_url("http://127.0.0.1:9");
    m.refresh_profile();
    assert!(
        state_is(&m, State::Offline, 5000),
        "signed in, can't reach the server"
    );
    assert!(m.is_signed_in());
    m.set_server_base_url(&fake.base());
    m.refresh_profile();
    assert!(state_is(&m, State::SignedIn, 5000));
}

#[test]
fn closing_never_waits_on_a_hanging_server() {
    let d = TempDir::new("acc-hang");
    let (_s, fake) = server();
    let m = AccountManager::new(config(&d, &fake.base()));
    sign_in(&m);
    fake.hang.store(true, Ordering::SeqCst);
    m.refresh_profile();
    assert!(wait_for(2000, || fake.count("GET /api/v1/me") >= 1));
    let t0 = std::time::Instant::now();
    drop(m);
    assert!(t0.elapsed() < Duration::from_secs(1), "{:?}", t0.elapsed());
    fake.hang.store(false, Ordering::SeqCst);
}

/// `HEARASIDE_TEST_LIVE=1`: ask the real server (https, the OS certificate store) for a sign-in code as hub / track / app,
/// like item 6 of the C++ `testAccount`. Signed out only; nothing is saved. Skipped without the variable (no internet).
#[test]
fn live_server_gives_a_sign_in_code() {
    if std::env::var_os("HEARASIDE_TEST_LIVE").is_none() {
        println!("SKIPPED: set HEARASIDE_TEST_LIVE=1 to ask the real server");
        return;
    }
    let d = TempDir::new("acc-live");
    let mut c = config(&d, "");
    c.server_base = None; // the default server, or $HEARASIDE_API_URL
    c.timings.poll_interval = None;
    let m = AccountManager::new(c);
    for plugin in ["hub", "track", "app"] {
        m.start_device_flow(plugin);
        assert!(
            wait_for(15_000, || !m.device_code().user_code.is_empty()
                || m.start_failed()),
            "{plugin}"
        );
        let code = m.device_code();
        println!("{plugin} code: {} {}", code.user_code, m.server_base_url());
        assert!(
            is_valid_code(&code.user_code) && m.state() == State::WaitingForCode,
            "server gives a sign-in code to {plugin}"
        );
        assert!(
            m.code_remaining_seconds() > 500,
            "the code is still counting down"
        );
        m.cancel_device_flow();
    }
}

#[test]
fn bug_requests_queue_up_instead_of_replacing_each_other() {
    // C++ bug: one "pending action" slot (and one set of pending texts): a second request made before the worker took the
    // first replaced it, so an avatar removal asked right after a profile edit was silently lost.
    let d = TempDir::new("acc-queue");
    let (_s, fake) = server();
    let m = AccountManager::new(config(&d, &fake.base()));
    sign_in(&m);
    m.update_profile(ProfilePatch {
        display_name: Some("A".into()),
        ..ProfilePatch::default()
    })
    .unwrap();
    m.update_profile(ProfilePatch {
        about: Some("B".into()),
        ..ProfilePatch::default()
    })
    .unwrap();
    m.remove_avatar();
    m.set_device_name("PC");
    m.refresh_profile();
    assert!(
        wait_for(5000, || fake.count("PATCH /api/v1/me") == 2
            && fake.count("DELETE /api/v1/me/avatar") == 1
            && fake.count("PATCH /api/v1/device") == 1),
        "every request reached the server"
    );
    assert!(wait_for(2000, || m.profile().display_name == "A"
        && m.profile().about == "B"
        && m.profile().device_name == "PC"));
}

#[test]
fn bug_start_and_cancel_do_nothing_while_signed_in() {
    // C++ bug: "Sign in" while signed in flipped the state to WaitingForCode, and cancelling it ended in SignedOut with the
    // tokens still in memory (and the file).
    let d = TempDir::new("acc-signedin");
    let (_s, fake) = server();
    let m = AccountManager::new(config(&d, &fake.base()));
    sign_in(&m);
    let starts = fake.count("POST /api/v1/device/start");
    m.start_device_flow("hub");
    assert!(holds_for(300, || m.state() == State::SignedIn
        && fake.count("POST /api/v1/device/start") == starts));
    m.cancel_device_flow();
    assert_eq!(m.state(), State::SignedIn);
    assert!(!m.access_token().is_empty() && d.file("account.dat").exists());
}

#[test]
fn bug_redirect_does_not_carry_the_token_to_another_host() {
    // C++ bug: JUCE re-sent the extra headers (the Bearer token) to wherever a redirect pointed.
    let d = TempDir::new("acc-redirect");
    let (_s, fake) = server();
    let m = AccountManager::new(config(&d, &fake.base()));
    sign_in(&m);
    let elsewhere = Fake::start(|_| Resp::json(200, json!({"id": "evil", "handle": "evil"})));
    let target = elsewhere.base();
    let front = Fake::start(move |r| Resp {
        status: 307,
        body: String::new(),
        location: Some(format!("{target}{}", r.path)),
    });
    m.set_server_base_url(&front.base());
    m.refresh_profile();
    assert!(
        wait_for(3000, || elsewhere.count("GET /api/v1/me") == 1),
        "the redirect was followed"
    );
    assert_eq!(
        front.requests("GET /api/v1/me")[0].header("authorization"),
        Some("Bearer access-1"),
        "the account's own server gets the token"
    );
    assert_eq!(
        elsewhere.requests("GET /api/v1/me")[0].header("authorization"),
        None,
        "another host never does"
    );
}

#[test]
fn bug_an_enormous_answer_is_not_read_into_memory() {
    // C++ bug: readEntireStreamAsString() on whatever the server (or whoever answered) sent.
    let d = TempDir::new("acc-huge");
    let (s, fake) = server();
    let m = AccountManager::new(config(&d, &fake.base()));
    sign_in(&m);
    s.lock().unwrap().me["about"] = json!("x".repeat(3 * 1024 * 1024));
    m.refresh_profile();
    assert!(wait_for(5000, || fake.count("GET /api/v1/me") >= 1));
    assert!(
        holds_for(500, || m.profile().about == "hi"
            && m.state() == State::SignedIn),
        "the oversized answer was dropped, not applied"
    );
}

// ---- review round: stopping, delays, checks ------------------------------------------------------------------------

#[test]
fn bug_drop_during_a_stalled_call_is_fast_writes_nothing_and_the_thread_ends() {
    // Review bug: a manager dropped while its worker was inside a network call (a plug-in scanner, a DAW closing) left the
    // thread running after the host could unload the module, and that thread could still write account.dat minutes later.
    let d = TempDir::new("acc-stall");
    let (s, fake) = server();
    {
        let a = AccountManager::new(config(&d, &fake.base()));
        sign_in(&a);
    }
    // the account now says something else: the stalled answer would be saved if the worker wrote after the drop
    s.lock().unwrap().me["displayName"] = json!("Changed after the drop");
    let before = std::fs::read(d.file("account.dat")).unwrap();
    fake.delay_ms.store(1500, Ordering::SeqCst);
    let detached = hs_app::detached_workers();
    let m = AccountManager::new(config(&d, &fake.base())); // first_periodic is 0 here: asks for the profile at once
    let alive = m.worker_alive_flag();
    assert!(wait_for(5000, || fake.count("GET /api/v1/me") >= 1));
    let t0 = std::time::Instant::now();
    drop(m);
    assert!(
        t0.elapsed() < Duration::from_secs(1),
        "drop took {:?}",
        t0.elapsed()
    );
    assert!(
        hs_app::detached_workers() > detached,
        "the thread outlived the drop, so the module was pinned first"
    );
    assert!(
        wait_for(15_000, || !alive.load(Ordering::SeqCst)),
        "the thread ends once its call returns"
    );
    assert_eq!(
        std::fs::read(d.file("account.dat")).unwrap(),
        before,
        "nothing is written after the drop"
    );
}

#[test]
fn a_session_found_at_start_makes_no_network_traffic_at_first() {
    // Review bug: the first periodic round ran at once, so a plug-in scanner that creates and drops the manager within
    // moments still started network traffic (and a thread that outlived it).
    let d = TempDir::new("acc-first");
    let (_s, fake) = server();
    {
        let a = AccountManager::new(config(&d, &fake.base()));
        sign_in(&a);
    }
    let n = fake.count("");
    let mut c = config(&d, &fake.base());
    c.timings.first_periodic = Duration::from_secs(3);
    let m = AccountManager::new(c);
    assert_eq!(m.state(), State::SignedIn);
    assert!(
        holds_for(500, || fake.count("") == n),
        "nothing is sent in the first moments"
    );
}

#[test]
fn http_429_on_the_token_poll_slows_down_like_slow_down() {
    let d = TempDir::new("acc-429");
    let (s, fake) = server();
    s.lock().unwrap().token_http_status = Some(429);
    let m = AccountManager::new(config(&d, &fake.base()));
    m.start_device_flow("hub");
    assert!(wait_for(3000, || fake.count("POST /api/v1/device/token") == 1));
    assert!(
        holds_for(500, || fake.count("POST /api/v1/device/token") == 1
            && m.state() == State::WaitingForCode),
        "five seconds more between polls, still waiting"
    );
}

#[test]
fn profile_patch_is_checked_before_it_is_sent() {
    // Review: the website answers 400 for these; only `about` can be empty. Lengths count UTF-16 units like zod.
    let d = TempDir::new("acc-check");
    let (_s, fake) = server();
    let m = AccountManager::new(config(&d, &fake.base()));
    sign_in(&m);
    let n = fake.count("PATCH /api/v1/me");
    let name = |v: &str| ProfilePatch {
        display_name: Some(v.to_owned()),
        ..ProfilePatch::default()
    };
    let handle = |v: &str| ProfilePatch {
        handle: Some(v.to_owned()),
        ..ProfilePatch::default()
    };
    let about = |v: &str| ProfilePatch {
        about: Some(v.to_owned()),
        ..ProfilePatch::default()
    };
    for bad in ["", "   ", &"x".repeat(41), &"\u{1f600}".repeat(21)] {
        assert_eq!(
            m.update_profile(name(bad)),
            Err(ProfileError::DisplayName),
            "{bad:?}"
        );
    }
    for bad in [
        "ab",
        "has space",
        "UPPER-dash",
        &"a".repeat(21),
        "\u{e19}\u{e32}\u{e21}",
        "",
    ] {
        assert_eq!(
            m.update_profile(handle(bad)),
            Err(ProfileError::Handle),
            "{bad:?}"
        );
    }
    for bad in [&"a".repeat(161), &"\u{1f600}".repeat(81)] {
        assert_eq!(m.update_profile(about(bad)), Err(ProfileError::About));
    }
    assert!(
        holds_for(300, || fake.count("PATCH /api/v1/me") == n),
        "a refused patch sends nothing"
    );
    // fine ones go out, normalised the way the website does it
    assert_eq!(
        m.update_profile(name(&format!(" {} ", "\u{1f600}".repeat(20)))),
        Ok(())
    );
    assert_eq!(m.update_profile(handle("  UPPER_ok9 ")), Ok(()));
    assert_eq!(m.update_profile(about(&"a".repeat(160))), Ok(()));
    assert!(wait_for(5000, || fake.count("PATCH /api/v1/me") == n + 3));
    let sent = fake.requests("PATCH /api/v1/me");
    assert_eq!(
        sent[n].json(),
        json!({"displayName": "\u{1f600}".repeat(20)})
    );
    assert_eq!(sent[n + 1].json(), json!({"handle": "upper_ok9"}));
}

#[test]
fn bug_names_are_clipped_by_utf16_units_like_the_website_counts() {
    // Review: zod max() counts JavaScript string length (UTF-16 units); counting scalar values let an emoji name past it.
    let d = TempDir::new("acc-utf16");
    let (_s, fake) = server();
    let mut c = config(&d, &fake.base());
    c.client = ClientMeta {
        version: "\u{e9}".repeat(30),
        os: "x".repeat(50),
        daw: "\u{1f600}".repeat(21),      // 42 units: 20 emoji fit in 40
        computer: "\u{1f600}".repeat(40), // 80 units: 32 emoji fit in 64
    };
    let m = AccountManager::new(c);
    m.start_device_flow("hub");
    assert!(wait_for(3000, || fake.count("POST /api/v1/device/start") == 1));
    let info = fake.requests("POST /api/v1/device/start")[0].json()["client_info"].clone();
    let units = |k: &str| info[k].as_str().unwrap().encode_utf16().count();
    assert_eq!(
        (
            units("version"),
            units("os"),
            units("daw"),
            units("computer")
        ),
        (20, 40, 40, 64)
    );
    assert_eq!(
        info["computer"].as_str().unwrap().chars().count(),
        32,
        "no emoji is cut in half"
    );
    drop(m);
    // the same for the name of this computer
    let d2 = TempDir::new("acc-utf16b");
    let m2 = AccountManager::new(config(&d2, &fake.base()));
    sign_in(&m2);
    m2.set_device_name(&"\u{1f600}".repeat(40));
    assert!(wait_for(3000, || fake.count("PATCH /api/v1/device") == 1));
    let name = fake.requests("PATCH /api/v1/device")[0].json()["name"]
        .as_str()
        .unwrap()
        .to_owned();
    assert_eq!(name.encode_utf16().count(), 64);
}

#[test]
fn the_device_secret_is_never_shown() {
    // Review: `device_code` is the secret the plug-in polls with; it must not be in the public DeviceCode, in events or in
    // a Debug print that ends up in a log.
    let d = TempDir::new("acc-secret");
    let (s, fake) = server();
    s.lock().unwrap().approve_after = 1_000_000;
    let m = AccountManager::new(config(&d, &fake.base()));
    m.start_device_flow("hub");
    assert!(wait_for(3000, || !m.device_code().user_code.is_empty()
        && fake.count("POST /api/v1/device/token") >= 1));
    let secret = "D".repeat(43);
    assert!(!format!("{:?}", m.device_code()).contains(&secret));
    assert!(!format!("{:?}", m.take_events()).contains(&secret));
    assert!(!format!("{:?}", m.profile()).contains(&secret));
}

#[test]
fn bug_disk_writes_happen_on_the_worker_not_the_callers_thread() {
    // Review: set_hide_email / set_sync_appearance encrypted and wrote account.dat on the caller thread, under the mutex.
    let d = TempDir::new("acc-io");
    let (_s, fake) = server();
    let m = AccountManager::new(config(&d, &fake.base()));
    sign_in(&m);
    let file = d.file("account.dat");
    let before = std::fs::read(&file).unwrap();
    fake.hang.store(true, Ordering::SeqCst); // the worker is stuck in a call
    m.refresh_profile();
    assert!(wait_for(3000, || fake.count("GET /api/v1/me") >= 1));
    let t0 = std::time::Instant::now();
    m.set_hide_email(false);
    m.set_sync_appearance(true, None);
    assert!(t0.elapsed() < Duration::from_millis(500));
    assert!(
        !m.profile().hide_email && m.profile().sync_appearance,
        "visible at once"
    );
    assert_eq!(
        std::fs::read(&file).unwrap(),
        before,
        "the caller thread wrote nothing"
    );
    fake.hang.store(false, Ordering::SeqCst);
    assert!(
        wait_for(8000, || std::fs::read(&file).is_ok_and(|b| b != before)),
        "the worker saved it"
    );
    assert!(wait_for(3000, || !AccountManager::new(config(
        &d,
        "http://127.0.0.1:9"
    ))
    .profile()
    .hide_email));
}

/// The other server saw no body and no Authorization header (a GET for the profile may be followed, without the token).
fn nothing_secret(f: &Fake) -> bool {
    f.requests("")
        .iter()
        .all(|r| r.body.is_empty() && r.header("authorization").is_none() && r.method == "GET")
}

#[test]
fn bug_requests_with_a_secret_in_the_body_are_never_redirected() {
    // Review: a redirect of /device/token or /token/refresh would send the device code or refresh token on to wherever the
    // Location points. Only a GET is followed.
    let d = TempDir::new("acc-nosecret-redirect");
    let (s, fake) = server();
    s.lock().unwrap().access_ttl = 30;
    {
        let a = AccountManager::new(config(&d, &fake.base()));
        sign_in(&a);
    }
    let elsewhere = Fake::start(|_| {
        Resp::json(
            200,
            json!({"access_token": "evil", "refresh_token": "evil"}),
        )
    });
    let target = elsewhere.base();
    let front = Fake::start(move |r| Resp {
        status: 307,
        body: String::new(),
        location: Some(format!("{target}{}", r.path)),
    });
    // the token refresh (body: the refresh token)
    let m = AccountManager::new(config(&d, &front.base()));
    assert!(wait_for(5000, || front.count("POST /api/v1/token/refresh") >= 1));
    assert!(
        holds_for(400, || elsewhere.count("POST") == 0
            && nothing_secret(&elsewhere)),
        "the refresh token was not sent on"
    );
    drop(m);
    // the sign-in poll (body: the device code)
    let d2 = TempDir::new("acc-nosecret-redirect2");
    let m2 = AccountManager::new(config(&d2, &front.base()));
    m2.start_device_flow("hub");
    assert!(
        wait_for(3000, || m2.start_failed()),
        "a redirect is not a code"
    );
    assert!(holds_for(300, || elsewhere.count("POST") == 0
        && nothing_secret(&elsewhere)));
}
