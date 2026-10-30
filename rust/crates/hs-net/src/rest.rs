//! The Hub's REST API (docs/rest-api.md): routing and JSON of `HubProcessor::handleControl` and `serviceControl`
//! (`plugins/hub/HubRestApi.cpp`), written against [`HubControl`] so the Rust Hub plug-in only has to implement that
//! trait. Everything here runs on the host's message thread (from [`ControlServer::service`]); every change goes
//! through the Hub's parameters (one change gesture, so the DAW can undo, automate and save it) or the Tracks' mailboxes,
//! exactly like a click in the Hub.
//!
//! The answers are byte for byte the C++ ones: members in the C++ order, numbers as `juce::JSON` wrote them
//! ([`crate::json`]), error texts unchanged. Differences from the C++ (bugs fixed) are marked `C++ bug` and listed in
//! the R3.4 ledger.
use crate::control::{ControlServer, Request, Response};
use crate::json::Json;
use hs_bus::protocol::{ParamId, SourceParam};
use serde_json::Value;

/// The Hub parameters the API changes (`hubparam::` in `HubProcessor.h`).
#[derive(Clone, Copy, Debug, PartialEq, Eq, Hash)]
pub enum HubParam {
    /// "Stream Level", dB
    Master,
    /// "Peak Protection"
    LimiterOn,
    /// "Hear Viewers Mix"
    Preview,
    /// "Mute Stream"
    Panic,
    /// "Headphone Master", dB
    Headphones,
}

impl HubParam {
    /// The parameter ID string (never changes: DAW projects and automation refer to it).
    pub fn id(self) -> &'static str {
        match self {
            HubParam::Master => "streamMaster",
            HubParam::LimiterOn => "limiterOn",
            HubParam::Preview => "preview",
            HubParam::Panic => "panic",
            HubParam::Headphones => "monitorMaster",
        }
    }
}

/// What the Hub is doing now (the `hub` object). Floats stay `f32` like the C++ members: JSON shows their widened value.
#[derive(Clone, Debug, Default, PartialEq)]
pub struct HubState {
    /// parameter `panic` > 0.5
    pub stream_muted: bool,
    /// parameter `preview` > 0.5
    pub hearing_viewers_mix: bool,
    /// parameter `streamMaster`
    pub stream_db: f32,
    /// parameter `monitorMaster`
    pub headphones_db: f32,
    /// parameter `limiterOn` > 0.5
    pub peak_protection: bool,
    /// the stream's loudness; -100 without a bus. Below -70 it is shown as `null`, else rounded to 0.1.
    pub loudness_lufs: f32,
    pub obs_connected: bool,
    pub viewers_silent: bool,
    /// `None` (`null`) while OBS is not connected
    pub latency_to_obs_ms: Option<f64>,
    pub daw_buffer: i32,
    pub sharing: bool,
}

/// One active Track (C++ `TrackView`: `mon` = you_hear, `str` = viewers_hear, `trimDb` = headphone_db, `gainDb` =
/// viewers_db).
#[derive(Clone, Debug, Default, PartialEq)]
pub struct TrackView {
    pub slot: i32,
    pub name: String,
    pub active: bool,
    pub you_hear: bool,
    pub viewers_hear: bool,
    pub headphone_db: f32,
    pub viewers_db: f32,
    pub delay_ms: f32,
    pub solo: bool,
    pub bypassed: bool,
}

/// One App Audio instance (C++ `SourceView`). `slot` is its headphone slot, -1 = none yet (too old for remote levels).
#[derive(Clone, Debug, Default, PartialEq)]
pub struct SourceView {
    pub index: i32,
    pub name: String,
    /// "chrome.exe", "*system*", "" = none
    pub program: String,
    pub on: bool,
    pub active: bool,
    pub slot: i32,
    pub you_hear: bool,
    pub viewers_hear: bool,
    pub headphone_db: f32,
    pub viewers_db: f32,
    pub delay_ms: f32,
    pub recording: bool,
}

/// What the REST API needs from the Hub plug-in. All methods are called on the message thread.
pub trait HubControl {
    /// `HEARASIDE_VERSION`
    fn version(&self) -> String;
    fn hub_state(&self) -> HubState;
    /// The link to give to listeners (only asked while `hub_state().sharing`).
    fn listen_link(&self) -> String;
    /// Active slots, in insertion order.
    fn tracks(&self) -> Vec<TrackView>;
    /// App Audio instances, in insertion order.
    fn sources(&self) -> Vec<SourceView>;

    /// Range of a Hub parameter in plain values: `(-60, 12)` for Master, `(-60, 6)` for Headphones.
    fn hub_range(&self, p: HubParam) -> (f32, f32);
    /// Range of a Track parameter, for `ParamId::StrGainDb` (-60..12), `MonTrimDb` (-60..6) and `StrDelayMs` (0..500).
    fn track_range(&self, id: u32) -> (f32, f32);
    /// Sets a Hub parameter to `plain` (already inside `hub_range`) the way a click in the Hub does: the Hub owns the
    /// change gesture (begin, `setValueNotifyingHost(convertTo0to1(plain))`, end), so the DAW can undo, automate and
    /// save it. Afterwards `hub_state()` shows the new value.
    fn set_param(&mut self, p: HubParam, plain: f32);

    /// A command to the Track in `slot` through its mailbox (`id` is a `hs_bus::protocol::ParamId`).
    fn send(&mut self, slot: i32, id: u32, value: f32);
    /// A command to App Audio `index` (`id` is a `hs_bus::protocol::SourceParam`).
    fn send_source(&mut self, index: i32, id: u32, value: f32);
    fn start_auto_sync(&mut self);
    fn cancel_auto_sync(&mut self);
    /// Share links on / off; `hub_state().sharing` shows it at once.
    fn set_sharing(&mut self, on: bool);

    /// Whether this Hub owns the bus. A second Hub on the same bus only passes audio and does not open the API.
    fn is_bus_owner(&self) -> bool;
    /// The "REST API" switch in the settings.
    fn rest_api_enabled(&self) -> bool;
    /// The API key in the settings ("" = none yet).
    fn rest_api_key(&self) -> String;
    fn set_rest_api_key(&mut self, key: &str);
}

// ---- JSON of the views -------------------------------------------------------------------------------------------

fn hub_json(hub: &(impl HubControl + ?Sized)) -> Json {
    let s = hub.hub_state();
    let lufs = f64::from(s.loudness_lufs);
    Json::obj([
        ("stream_muted", Json::Bool(s.stream_muted)),
        ("hearing_viewers_mix", Json::Bool(s.hearing_viewers_mix)),
        ("stream_db", Json::float(s.stream_db)),
        ("headphones_db", Json::float(s.headphones_db)),
        ("peak_protection", Json::Bool(s.peak_protection)),
        (
            "loudness_lufs",
            if lufs < -70.0 {
                Json::Null
            } else {
                Json::Num((lufs * 10.0).round() / 10.0)
            },
        ),
        ("obs_connected", Json::Bool(s.obs_connected)),
        ("viewers_silent", Json::Bool(s.viewers_silent)),
        (
            "latency_to_obs_ms",
            s.latency_to_obs_ms.map_or(Json::Null, Json::Num),
        ),
        ("daw_buffer", Json::Int(i64::from(s.daw_buffer))),
        ("sharing", Json::Bool(s.sharing)),
        (
            "listen_link",
            Json::Str(if s.sharing {
                hub.listen_link()
            } else {
                String::new()
            }),
        ),
    ])
}

fn track_json(t: &TrackView) -> Json {
    Json::obj([
        ("slot", Json::Int(i64::from(t.slot))),
        ("name", Json::str(t.name.as_str())),
        ("active", Json::Bool(t.active)),
        ("you_hear", Json::Bool(t.you_hear)),
        ("viewers_hear", Json::Bool(t.viewers_hear)),
        ("headphone_db", Json::float(t.headphone_db)),
        ("viewers_db", Json::float(t.viewers_db)),
        ("delay_ms", Json::float(t.delay_ms)),
        ("solo", Json::Bool(t.solo)),
        ("bypassed", Json::Bool(t.bypassed)),
    ])
}

fn source_json(s: &SourceView) -> Json {
    Json::obj([
        ("index", Json::Int(i64::from(s.index))),
        ("name", Json::str(s.name.as_str())),
        ("program", Json::str(s.program.as_str())),
        ("on", Json::Bool(s.on)),
        ("active", Json::Bool(s.active)),
        ("you_hear", Json::Bool(s.you_hear)),
        ("viewers_hear", Json::Bool(s.viewers_hear)),
        ("headphone_db", Json::float(s.headphone_db)),
        ("viewers_db", Json::float(s.viewers_db)),
        ("delay_ms", Json::float(s.delay_ms)),
        ("recording", Json::Bool(s.recording)),
    ])
}

fn tracks_json(hub: &(impl HubControl + ?Sized)) -> Json {
    Json::Arr(hub.tracks().iter().map(track_json).collect())
}

fn sources_json(hub: &(impl HubControl + ?Sized)) -> Json {
    Json::Arr(hub.sources().iter().map(source_json).collect())
}

fn index_json(version: &str) -> Json {
    const ENDPOINTS: [&str; 12] = [
        "GET  /api/v1/state",
        "POST /api/v1/mute {on?}",
        "POST /api/v1/preview {on?}",
        "POST /api/v1/limiter {on?}",
        "POST /api/v1/stream {db}",
        "POST /api/v1/headphones {db}",
        "GET  /api/v1/tracks",
        "POST /api/v1/tracks/<slot|name> {you_hear?, viewers_hear?, headphone_db?, viewers_db?, delay_ms?, solo?}",
        "GET  /api/v1/sources",
        "POST /api/v1/sources/<index|name> {on?, you_hear?, viewers_hear?, headphone_db?, viewers_db?, record?}",
        "POST /api/v1/sync {action: start|cancel}",
        "POST /api/v1/share {on?}",
    ];
    Json::obj([
        ("name", Json::str("HEARASIDE Hub")),
        ("version", Json::str(version)),
        ("docs", Json::str("docs/rest-api.md")),
        (
            "endpoints",
            Json::Arr(ENDPOINTS.iter().map(|e| Json::str(*e)).collect()),
        ),
    ])
}

// ---- reading the request -----------------------------------------------------------------------------------------

fn fail(status: u16, message: &str) -> Response {
    Response::error(status, message)
}

/// `juce::URL::removeEscapeChars`: `+` is a space, `%XX` is a byte (the bytes are UTF-8). A `%` without two hex digits
/// stays as it is.
fn unescape(s: &str) -> String {
    let b = s.as_bytes();
    let mut out = Vec::with_capacity(b.len());
    let mut i = 0;
    while i < b.len() {
        let hex = |c: u8| char::from(c).to_digit(16);
        match b[i] {
            b'+' => out.push(b' '),
            b'%' if i + 2 < b.len() && hex(b[i + 1]).is_some() && hex(b[i + 2]).is_some() => {
                let hi = hex(b[i + 1]).unwrap_or(0);
                let lo = hex(b[i + 2]).unwrap_or(0);
                out.push((hi * 16 + lo) as u8);
                i += 2;
            }
            c => out.push(c),
        }
        i += 1;
    }
    String::from_utf8_lossy(&out).into_owned()
}

/// A boolean member, like the C++ `wantOn` with one addition. `None` = absent. A boolean is itself, a number is
/// true unless 0, the strings "true" / "false" (any case, trimmed) are that value. Anything else (`null`, any other
/// string, an array, an object) flips `current`, so a Stream Deck button that sends `{"on":"toggle"}` keeps working.
/// C++ bug: the strings "true" / "false" flipped too, so `{"on": "false"}` on a muted stream un-muted it (and an
/// `int64` number flipped as well).
fn flag(body: &Value, key: &str, current: bool) -> Option<bool> {
    Some(match body.get(key)? {
        Value::Bool(b) => *b,
        Value::Number(n) => n.as_f64().is_some_and(|f| f != 0.0),
        Value::String(s) => match s.trim().to_ascii_lowercase().as_str() {
            "true" => true,
            "false" => false,
            _ => !current,
        },
        _ => !current,
    })
}

/// A number member; `None` = absent or not a number (the C++ ignored those too).
fn number(body: &Value, key: &str) -> Option<f64> {
    body.get(key)?.as_f64().filter(|f| f.is_finite())
}

/// `v` limited to the parameter's own range. C++ bug: the Hub used -60..6 for the viewers level (the Track's goes to
/// +12) and gave the Hub's own parameters the raw `float(db)`, which is `inf` for 1e300.
fn clamp_db(v: f64, (lo, hi): (f32, f32)) -> f32 {
    v.max(f64::from(lo)).min(f64::from(hi)) as f32
}

fn on_off(on: bool) -> f32 {
    if on { 1.0 } else { 0.0 }
}

/// A row by number or name. A key of digits is the slot / index first, then the name, any case; the first row that
/// matches wins (two rows with one name: the first, as in the C++, so scripts that say "App Audio" keep working).
/// C++ bug: the number was matched or the name, whichever row came first, so a Track called "2" hid slot 2; and a
/// number too big for an `int` wrapped (4294967296 was slot 0).
fn find_row<T: Clone>(
    rows: &[T],
    key: &str,
    number: impl Fn(&T) -> i32,
    name: impl Fn(&T) -> &str,
) -> Option<T> {
    if !key.is_empty()
        && key.bytes().all(|b| b.is_ascii_digit())
        && let Ok(n) = key.parse::<i32>()
        && let Some(r) = rows.iter().find(|r| number(r) == n)
    {
        return Some(r.clone());
    }
    let wanted = key.to_lowercase();
    rows.iter()
        .find(|r| name(r).to_lowercase() == wanted)
        .cloned()
}

/// you_hear / viewers_hear / headphone_db / viewers_db: the settings Tracks and App Audio share (through their
/// headphone slot).
struct Row {
    you_hear: Option<bool>,
    viewers_hear: Option<bool>,
    headphone_db: Option<f64>,
    viewers_db: Option<f64>,
}

impl Row {
    fn read(body: &Value, mon: bool, str_: bool) -> Row {
        Row {
            you_hear: flag(body, "you_hear", mon),
            viewers_hear: flag(body, "viewers_hear", str_),
            headphone_db: number(body, "headphone_db"),
            viewers_db: number(body, "viewers_db"),
        }
    }

    fn send<H: HubControl + ?Sized>(&self, hub: &mut H, slot: i32) {
        if let Some(on) = self.you_hear {
            hub.send(slot, ParamId::Mon, on_off(on));
        }
        if let Some(on) = self.viewers_hear {
            hub.send(slot, ParamId::Str, on_off(on));
        }
        if let Some(v) = self.headphone_db {
            let r = hub.track_range(ParamId::MonTrimDb);
            hub.send(slot, ParamId::MonTrimDb, clamp_db(v, r));
        }
        if let Some(v) = self.viewers_db {
            let r = hub.track_range(ParamId::StrGainDb);
            hub.send(slot, ParamId::StrGainDb, clamp_db(v, r));
        }
    }
}

const OLD_APP_AUDIO: &str = "this App Audio is too old for remote levels";

// ---- routes ------------------------------------------------------------------------------------------------------

enum Route {
    Index,
    State,
    Tracks,
    Sources,
    /// mute, preview, limiter: `{on?}`
    Switch(HubParam),
    /// stream, headphones: `{db}`
    Level(HubParam),
    Track(String),
    Source(String),
    Sync,
    Share,
}

fn route(seg: &[String]) -> Option<Route> {
    let seg: Vec<&str> = seg.iter().map(String::as_str).collect();
    Some(match seg.as_slice() {
        [] => Route::Index,
        ["state"] => Route::State,
        ["tracks"] => Route::Tracks,
        ["sources"] => Route::Sources,
        ["mute"] => Route::Switch(HubParam::Panic),
        ["preview"] => Route::Switch(HubParam::Preview),
        ["limiter"] => Route::Switch(HubParam::LimiterOn),
        ["stream"] => Route::Level(HubParam::Master),
        ["headphones"] => Route::Level(HubParam::Headphones),
        ["tracks", key] => Route::Track((*key).to_owned()),
        ["sources", key] => Route::Source((*key).to_owned()),
        ["sync"] => Route::Sync,
        ["share"] => Route::Share,
        _ => return None,
    })
}

/// The C++ `HubProcessor::handleControl`: one request in, the answer out. Message thread.
pub fn handle<H: HubControl + ?Sized>(hub: &mut H, req: &Request) -> Response {
    // C++ bug: only the start was checked, so "/api/v1state" was "/api/v1/state" and "/api/v1/mute/x" was mute
    let Some(rest) = req
        .path
        .strip_prefix("/api/v1")
        .filter(|r| r.is_empty() || r.starts_with('/'))
    else {
        return fail(404, "unknown path (try GET /api/v1)");
    };
    let seg: Vec<String> = rest
        .split('/')
        .filter(|p| !p.is_empty())
        .map(unescape)
        .collect();
    // C++ bug: a GET of an unknown path was "405 POST only", not 404
    let Some(route) = route(&seg) else {
        return fail(404, "unknown path (try GET /api/v1)");
    };
    let get = req.method == "GET";
    let post = req.method == "POST";
    let body = &req.body;

    if matches!(
        route,
        Route::Index | Route::State | Route::Tracks | Route::Sources
    ) {
        if !get {
            // as the C++: a POST to the list paths is "unknown path" (404), anything else but GET is 405
            return match (&route, post) {
                (Route::Tracks | Route::Sources, true) => {
                    if !body.is_null() && !body.is_object() {
                        fail(400, "the body must be a JSON object")
                    } else {
                        fail(404, "unknown path (try GET /api/v1)")
                    }
                }
                (Route::Tracks | Route::Sources, false) => fail(405, "POST only"),
                _ => fail(405, "GET only"),
            };
        }
        let json = match route {
            Route::Index => index_json(&hub.version()),
            Route::State => Json::obj([
                ("hub", hub_json(hub)),
                ("tracks", tracks_json(hub)),
                ("sources", sources_json(hub)),
            ]),
            Route::Tracks => tracks_json(hub),
            _ => sources_json(hub),
        };
        return Response::new(200, json);
    }
    if !post {
        return fail(405, "POST only");
    }
    if !body.is_null() && !body.is_object() {
        return fail(400, "the body must be a JSON object");
    }
    match route {
        // the switches that change what the viewers get are applied at once, like the buttons
        Route::Switch(p) => {
            let s = hub.hub_state();
            let current = match p {
                HubParam::Panic => s.stream_muted,
                HubParam::Preview => s.hearing_viewers_mix,
                _ => s.peak_protection,
            };
            let want = flag(body, "on", current).unwrap_or(!current);
            hub.set_param(p, on_off(want));
            Response::new(200, hub_json(hub))
        }
        Route::Level(p) => {
            let Some(db) = number(body, "db") else {
                return fail(400, "needs {\"db\": number}");
            };
            let v = clamp_db(db, hub.hub_range(p));
            hub.set_param(p, v);
            Response::new(200, hub_json(hub))
        }
        Route::Track(key) => {
            let Some(t) = find_row(&hub.tracks(), &key, |t| t.slot, |t| &t.name) else {
                return fail(404, &format!("no track \"{key}\" (GET /api/v1/tracks)"));
            };
            let row = Row::read(body, t.you_hear, t.viewers_hear);
            let delay = number(body, "delay_ms");
            let solo = flag(body, "solo", t.solo);
            if t.slot < 0 {
                return fail(400, OLD_APP_AUDIO);
            }
            row.send(hub, t.slot);
            if let Some(v) = delay {
                let r = hub.track_range(ParamId::StrDelayMs);
                hub.send(t.slot, ParamId::StrDelayMs, clamp_db(v, r));
            }
            if let Some(on) = solo {
                hub.send(t.slot, ParamId::StrSolo, on_off(on));
            }
            // the Track applies it within one block
            Response::new(
                200,
                Json::obj([
                    ("ok", Json::Bool(true)),
                    ("slot", Json::Int(i64::from(t.slot))),
                    ("name", Json::str(t.name.as_str())),
                ]),
            )
        }
        Route::Source(key) => {
            let Some(s) = find_row(&hub.sources(), &key, |s| s.index, |s| &s.name) else {
                return fail(
                    404,
                    &format!("no App Audio \"{key}\" (GET /api/v1/sources)"),
                );
            };
            let row = Row::read(body, s.you_hear, s.viewers_hear);
            let on = flag(body, "on", s.on);
            let record = flag(body, "record", s.recording);
            let touches_row = ["you_hear", "viewers_hear", "headphone_db", "viewers_db"]
                .iter()
                .any(|k| body.get(k).is_some());
            if touches_row {
                if s.slot < 0 {
                    return fail(400, OLD_APP_AUDIO);
                }
                row.send(hub, s.slot);
            }
            if let Some(on) = on {
                hub.send_source(s.index, SourceParam::On, on_off(on));
            }
            if let Some(on) = record {
                hub.send_source(s.index, SourceParam::Record, on_off(on));
            }
            Response::new(
                200,
                Json::obj([
                    ("ok", Json::Bool(true)),
                    ("index", Json::Int(i64::from(s.index))),
                    ("name", Json::str(s.name.as_str())),
                ]),
            )
        }
        Route::Sync => {
            let action = match body.get("action") {
                None => "start",
                Some(Value::String(s)) => s.as_str(),
                Some(_) => "",
            };
            match action {
                "cancel" => hub.cancel_auto_sync(),
                "start" => hub.start_auto_sync(),
                _ => return fail(400, "action is start or cancel"),
            }
            Response::new(200, Json::obj([("ok", Json::Bool(true))]))
        }
        Route::Share => {
            let current = hub.hub_state().sharing;
            let want = flag(body, "on", current).unwrap_or(!current);
            hub.set_sharing(want);
            Response::new(200, hub_json(hub))
        }
        Route::Index | Route::State | Route::Tracks | Route::Sources => {
            fail(404, "unknown path (try GET /api/v1)")
        }
    }
}

// ---- starting and stopping the server (serviceControl) -------------------------------------------------------------

/// A busy port is tried again this often, not 20 times a second.
const RETRY_MS: u32 = 5000;

/// The Hub's `ControlServer` and the C++ `HubProcessor::serviceControl`: call [`RestService::service`] from the
/// message thread's timer. The server runs while the REST API is switched on and this Hub owns the bus; the key is
/// made on first use.
pub struct RestService {
    server: ControlServer,
    key: String,
    last_try: Option<u32>,
}

impl Default for RestService {
    fn default() -> Self {
        Self::new()
    }
}

impl RestService {
    pub fn new() -> Self {
        Self::with_server(ControlServer::new())
    }

    pub fn with_server(server: ControlServer) -> Self {
        RestService {
            server,
            key: String::new(),
            last_try: None,
        }
    }

    pub fn server(&self) -> &ControlServer {
        &self.server
    }

    /// `now_ms`: a millisecond clock that may wrap (`juce::Time::getMillisecondCounter`).
    pub fn service<H: HubControl + ?Sized>(&mut self, hub: &mut H, now_ms: u32) {
        // only the Hub that owns the bus answers (a second Hub only passes audio through)
        let want = hub.rest_api_enabled() && hub.is_bus_owner();
        if want && hub.rest_api_key().is_empty() {
            hub.set_rest_api_key(&ControlServer::new_key());
        }
        let key = hub.rest_api_key();
        let running = self.server.running();
        // C++ bug: a new key waited for the 5 s retry timer, the old (maybe leaked) key worked until then
        let rotated = running && self.key != key;
        let due = self
            .last_try
            .is_none_or(|t| now_ms.wrapping_sub(t) > RETRY_MS);
        if want && (rotated || (!running && due)) {
            self.last_try = Some(now_ms);
            self.key = key;
            self.server.start(&self.key);
        }
        if !want && self.server.running() {
            self.server.stop();
        }
        if self.server.running() {
            self.server.service(|r| handle(hub, r));
        }
    }
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn escapes_like_juce() {
        assert_eq!(unescape("Vocal"), "Vocal");
        assert_eq!(unescape("Lead%20Vox"), "Lead Vox");
        assert_eq!(unescape("Lead+Vox"), "Lead Vox");
        assert_eq!(unescape("a%2Bb"), "a+b");
        assert_eq!(unescape("%E0%B8%81"), "ก");
        assert_eq!(unescape("100%"), "100%");
        assert_eq!(unescape("%zz%4"), "%zz%4");
        assert_eq!(unescape("a%25b"), "a%b");
        assert_eq!(unescape("%2"), "%2");
    }

    #[test]
    fn parameter_ids_never_change() {
        assert_eq!(HubParam::Master.id(), "streamMaster");
        assert_eq!(HubParam::LimiterOn.id(), "limiterOn");
        assert_eq!(HubParam::Preview.id(), "preview");
        assert_eq!(HubParam::Panic.id(), "panic");
        assert_eq!(HubParam::Headphones.id(), "monitorMaster");
    }

    #[test]
    fn rows_are_found_by_number_then_name() {
        #[derive(Clone, Debug, PartialEq)]
        struct R(i32, &'static str);
        let rows = [R(0, "Vox"), R(1, "2"), R(2, "Gtr"), R(3, "vox")];
        let f = |k: &str| find_row(&rows, k, |r| r.0, |r| r.1);
        assert_eq!(f("2"), Some(R(2, "Gtr")), "the slot wins over a name");
        assert_eq!(f("1"), Some(R(1, "2")));
        assert_eq!(f("gtr"), Some(R(2, "Gtr")));
        assert_eq!(f("VOX"), Some(R(0, "Vox")), "two with one name: the first");
        assert_eq!(f("9"), None);
        assert_eq!(f("4294967296"), None, "no wrapping to slot 0");
        assert_eq!(f("99999999999999999999"), None);
        assert_eq!(f(""), None);
    }
}
