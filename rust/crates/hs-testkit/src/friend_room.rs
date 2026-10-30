//! The friends room of a Hub (S2): up to eight friends, each with a send-in link of their own. Port of
//! `plugins/hub/FriendRoom.{h,cpp}`. Message thread only. It owns the list (names, tokens, levels), publishes ids and names
//! into shared memory (so App Audio and HEARASIDE Track can list them), pushes the levels to the engine and hands the share
//! server its table of links. Audio never goes through here.
//!
//! The project state is a JSON value here; the Rust Hub plug-in will write the same fields in its XML state (R8). Candidate
//! for `hs-app` or the Hub crate once that exists: it is the one piece of Hub logic between the engine and the share server.
use hs_bus::bus::{bits_float, float_bits, read_friend_name, set_friend_name};
use hs_bus::protocol::{BusLayout, FriendRoute, FriendState, kMaxFriends};
use hs_engine::measure::{FRIEND_MON, FRIEND_SOLO, FRIEND_STR, HubMeasurer};
use hs_net::share::{FriendLink, ShareServer};
use serde_json::{Value, json};
use std::sync::atomic::Ordering::{Relaxed, Release};

pub const MAX: usize = kMaxFriends;

/// A secret under 24 characters is not a link (130 bits of secure random make 26).
const MIN_TOKEN: usize = 24;

#[derive(Clone, Debug, PartialEq)]
pub struct Entry {
    pub id: u32,
    pub slot: i32,
    pub name: String,
    pub token: String,
    pub volume_db: f32,
    pub pan: f32,
    pub mon: bool,
    pub str_: bool,
    pub solo: bool,
}

/// What the screens show for one friend.
#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub enum State {
    /// never opened the link
    Waiting,
    /// singing
    Live,
    /// left
    Offline,
}

#[derive(Clone, Debug)]
pub struct FriendView {
    pub id: u32,
    pub slot: i32,
    pub name: String,
    pub state: State,
    pub volume_db: f32,
    pub pan: f32,
    pub mon: bool,
    pub str_: bool,
    pub solo: bool,
    /// how late the voice arrives (network + browser), -1 = not measured yet
    pub delay_ms: f32,
    /// + the Hub's small jitter buffer: what Line up works with
    pub total_ms: f32,
    /// extra delay the Hub gives this friend so they land on the beat
    pub line_ms: f32,
    /// microphone level (linear)
    pub peak: f32,
    /// the Hub is playing the audio right now
    pub playing: bool,
    /// slower than the Line-up limit: not waited for
    pub over_limit: bool,
    /// a DAW track carries this friend (S7), not the Hub
    pub in_daw: bool,
    pub feeder: i32,
    pub out_slot: i32,
    pub leak_detected: bool,
    pub echo_in_master: bool,
}

impl FriendView {
    pub fn live(&self) -> bool {
        self.state == State::Live
    }
}

#[derive(Default)]
pub struct FriendRoom {
    entries: Vec<Entry>,
    next_id: u32,
    dirty: bool,
    push_all: bool,
    table_dirty: bool,
    /// the bus the headers were last written to (its address)
    published: Option<usize>,
    published_host: String,
}

/// A saved level inside its range; a missing, damaged or NaN one is 0.
fn level(v: &Value, lo: f32, hi: f32) -> f32 {
    let x = v.as_f64().unwrap_or(0.0) as f32;
    if x.is_nan() { 0.0 } else { x.clamp(lo, hi) }
}

fn default_name(n: usize) -> String {
    format!("Friend {n}")
}

impl FriendRoom {
    pub fn new() -> Self {
        FriendRoom {
            next_id: 100,
            dirty: true,
            push_all: true,
            table_dirty: true,
            ..FriendRoom::default()
        }
    }

    pub fn count(&self) -> usize {
        self.entries.len()
    }

    pub fn full(&self) -> bool {
        self.entries.len() >= MAX
    }

    pub fn entries(&self) -> &[Entry] {
        &self.entries
    }

    pub fn find(&self, id: u32) -> Option<&Entry> {
        self.entries.iter().find(|e| e.id == id)
    }

    fn get(&mut self, id: u32) -> Option<&mut Entry> {
        self.entries.iter_mut().find(|e| e.id == id)
    }

    fn touch(&mut self) {
        self.dirty = true;
        self.push_all = true;
        self.table_dirty = true;
    }

    fn free_slot(&self) -> Option<i32> {
        (0..MAX as i32).find(|&s| !self.entries.iter().any(|e| e.slot == s))
    }

    pub fn slot_of(&self, id: u32) -> i32 {
        self.find(id).map_or(-1, |e| e.slot)
    }

    pub fn token(&self, id: u32) -> String {
        self.find(id).map(|e| e.token.clone()).unwrap_or_default()
    }

    /// `None` = the room is full (or the OS gave no secure random). The name defaults to "Friend N".
    pub fn add(&mut self, name: &str) -> Option<u32> {
        let slot = self.free_slot()?;
        let token = ShareServer::new_token();
        if token.len() < MIN_TOKEN {
            return None; // no secure random: no link rather than a guessable one
        }
        let id = self.next_id;
        // an id is never reused: when they run out there is no room for another friend
        self.next_id = id.checked_add(1)?;
        let name = name.trim();
        self.entries.push(Entry {
            id,
            slot,
            name: if name.is_empty() {
                default_name(self.entries.len() + 1)
            } else {
                name.to_owned()
            },
            token,
            volume_db: 0.0,
            pan: 0.0,
            mon: true,
            str_: true,
            solo: false,
        });
        self.touch();
        Some(id)
    }

    /// Revokes the token: the friend's link stops working.
    pub fn remove(&mut self, id: u32) -> bool {
        let Some(i) = self.entries.iter().position(|e| e.id == id) else {
            return false;
        };
        self.entries.remove(i);
        self.touch();
        true
    }

    /// "New link": a new token, the old link stops working ("" = failed).
    pub fn new_link(&mut self, id: u32) -> String {
        let t = ShareServer::new_token();
        if t.len() < MIN_TOKEN {
            return String::new();
        }
        let Some(e) = self.get(id) else {
            return String::new();
        };
        e.token.clone_from(&t);
        self.touch();
        t
    }

    pub fn rename(&mut self, id: u32, name: &str) {
        let name = name.trim();
        let Some(e) = self.get(id) else { return };
        let n = if name.is_empty() {
            default_name(e.slot as usize + 1)
        } else {
            name.to_owned()
        };
        if e.name != n {
            e.name = n;
            self.touch();
        }
    }

    /// C++ bug: `jlimit` passes a NaN on, and a NaN gain in the engine silences everything the friend touches. A NaN is ignored.
    pub fn set_volume_db(&mut self, id: u32, db: f32) {
        if db.is_nan() {
            return;
        }
        if let Some(e) = self.get(id) {
            e.volume_db = db.clamp(-60.0, 6.0);
        }
    }

    /// -1 .. +1, viewers only
    pub fn set_pan(&mut self, id: u32, pan: f32) {
        if pan.is_nan() {
            return;
        }
        if let Some(e) = self.get(id) {
            e.pan = pan.clamp(-1.0, 1.0);
        }
    }

    pub fn set_mon(&mut self, id: u32, on: bool) {
        if let Some(e) = self.get(id) {
            e.mon = on;
        }
    }

    pub fn set_str(&mut self, id: u32, on: bool) {
        if let Some(e) = self.get(id) {
            e.str_ = on;
        }
    }

    pub fn set_solo(&mut self, id: u32, on: bool) {
        if let Some(e) = self.get(id) {
            e.solo = on;
        }
    }

    /// L60 .. R60, equally apart (needs two friends).
    pub fn spread_out(&mut self) {
        let n = self.entries.len();
        if n < 2 {
            return;
        }
        for (i, e) in self.entries.iter_mut().enumerate() {
            e.pan = -0.6 + 1.2 * i as f32 / (n - 1) as f32;
        }
    }

    /// The list changed since the last call (links / names): refresh the share table.
    pub fn changed(&mut self) -> bool {
        std::mem::take(&mut self.dirty)
    }

    // ---- project state: ids, names, tokens and levels. The tokens ARE the links: they live in the Hub's state (the
    // project) and nowhere else (never in a log).
    pub fn to_state(&self) -> Value {
        json!({
            "nextId": self.next_id,
            "friends": self.entries.iter().map(|e| json!({
                "id": e.id, "name": e.name, "token": e.token, "vol": e.volume_db,
                "pan": e.pan, "mon": e.mon, "str": e.str_, "solo": e.solo,
            })).collect::<Vec<_>>(),
        })
    }

    pub fn from_state(&mut self, state: &Value) {
        self.entries.clear();
        let next = state["nextId"]
            .as_i64()
            .unwrap_or(100)
            .clamp(100, i64::from(i32::MAX));
        self.next_id = next as u32;
        for f in state["friends"].as_array().into_iter().flatten() {
            if self.entries.len() >= MAX {
                break;
            }
            let id = f["id"]
                .as_u64()
                .and_then(|v| u32::try_from(v).ok())
                .unwrap_or(0);
            let token = f["token"].as_str().unwrap_or("");
            // a damaged entry: skip it rather than make up a link
            // (u32::MAX would leave no next id)
            if id == 0 || id == u32::MAX || token.len() < MIN_TOKEN || self.find(id).is_some() {
                continue;
            }
            let Some(slot) = self.free_slot() else { break };
            let name = f["name"]
                .as_str()
                .map_or_else(|| default_name(self.entries.len() + 1), str::to_owned);
            self.entries.push(Entry {
                id,
                slot,
                name,
                token: token.to_owned(),
                volume_db: level(&f["vol"], -60.0, 6.0),
                pan: level(&f["pan"], -1.0, 1.0),
                mon: f["mon"].as_bool().unwrap_or(true),
                str_: f["str"].as_bool().unwrap_or(true),
                solo: f["solo"].as_bool().unwrap_or(false),
            });
            self.next_id = self.next_id.max(id.saturating_add(1));
        }
        self.touch();
    }

    // ---- every tick ---------------------------------------------------------------------------------------------------

    /// Publishes to the bus, pushes the levels to the engine and keeps the share server's table current.
    pub fn service(
        &mut self,
        bus: Option<&BusLayout>,
        engine: &HubMeasurer,
        share: &ShareServer,
        host: &str,
    ) {
        if self.table_dirty || host != self.published_host {
            self.table_dirty = false;
            share.set_friend_links(
                self.entries
                    .iter()
                    .map(|e| FriendLink {
                        slot: e.slot,
                        id: e.id,
                        token: e.token.clone(),
                        name: e.name.clone(),
                        host: host.to_owned(),
                    })
                    .collect(),
            );
            host.clone_into(&mut self.published_host);
        }
        // a friend who left the room: end their connection (the table above no longer knows the token)
        for s in 0..MAX {
            let used = self.entries.iter().any(|e| e.slot == s as i32);
            if !used && bus.is_some_and(|b| b.friends.0[s].id.load(Relaxed) != 0) {
                share.kick_friend(s as i32);
            }
        }
        for e in &self.entries {
            let Ok(s) = usize::try_from(e.slot) else {
                continue;
            };
            let c = engine.friend_control(s);
            let flags = if e.mon { FRIEND_MON } else { 0 }
                | if e.str_ { FRIEND_STR } else { 0 }
                | if e.solo { FRIEND_SOLO } else { 0 };
            c.flags.store(flags, Relaxed);
            c.gain_bits.store(float_bits(e.volume_db), Relaxed);
            c.pan_bits.store(float_bits(e.pan), Relaxed);
            share.set_friend_leak_detected(
                e.slot,
                engine.friend_info(s).leak_detected.load(Relaxed) != 0,
            );
        }
        let Some(bus) = bus else {
            self.published = None;
            return;
        };
        let id = std::ptr::from_ref(bus) as usize;
        if self.published == Some(id) && !self.push_all {
            return;
        }
        // ids and names into shared memory (App Audio / HEARASIDE Track list them); the share server owns the live state
        for s in 0..MAX {
            let h = &bus.friends.0[s];
            let Some(e) = self.entries.iter().find(|x| x.slot == s as i32) else {
                if h.id.load(Relaxed) != 0 {
                    h.state.store(FriendState::kFriendFree, Release);
                    h.id.store(0, Release);
                    h.route.store(FriendRoute::kRouteDirect, Relaxed);
                    h.feeder.store(-1, Relaxed);
                    h.outSlot.store(-1, Relaxed);
                    h.peakBits.store(0, Relaxed);
                    set_friend_name(h, "");
                }
                continue;
            };
            set_friend_name(h, &e.name);
            if h.id.load(Relaxed) != e.id {
                // a new friend in this slot
                h.state.store(FriendState::kFriendWaiting, Release);
                h.id.store(e.id, Release);
                h.route.store(FriendRoute::kRouteDirect, Relaxed);
                h.feeder.store(-1, Relaxed);
                h.outSlot.store(-1, Relaxed);
                h.delayBits.store(float_bits(-1.0), Relaxed);
            } else if h.state.load(Relaxed) == FriendState::kFriendFree {
                h.state.store(FriendState::kFriendWaiting, Release);
            }
        }
        self.published = Some(id);
        self.push_all = false;
    }

    pub fn views(&self, bus: Option<&BusLayout>, engine: &HubMeasurer) -> Vec<FriendView> {
        self.entries
            .iter()
            .map(|e| {
                let mut v = FriendView {
                    id: e.id,
                    slot: e.slot,
                    name: e.name.clone(),
                    state: State::Waiting,
                    volume_db: e.volume_db,
                    pan: e.pan,
                    mon: e.mon,
                    str_: e.str_,
                    solo: e.solo,
                    delay_ms: -1.0,
                    total_ms: 0.0,
                    line_ms: 0.0,
                    peak: 0.0,
                    playing: false,
                    over_limit: false,
                    in_daw: false,
                    feeder: -1,
                    out_slot: -1,
                    leak_detected: false,
                    echo_in_master: false,
                };
                let s = e.slot as usize;
                if let Some(b) = bus.filter(|b| b.friends.0[s].id.load(Relaxed) == e.id) {
                    let h = &b.friends.0[s];
                    v.state = match h.state.load(std::sync::atomic::Ordering::Acquire) {
                        FriendState::kFriendLive => State::Live,
                        FriendState::kFriendOffline => State::Offline,
                        _ => State::Waiting,
                    };
                    v.delay_ms = bits_float(h.delayBits.load(Relaxed));
                    v.peak = bits_float(h.peakBits.load(Relaxed));
                    v.in_daw = h.route.load(Relaxed) == FriendRoute::kRouteDaw;
                    v.feeder = h.feeder.load(Relaxed);
                    v.out_slot = h.outSlot.load(Relaxed);
                    let info = engine.friend_info(s);
                    v.playing = info.playing.load(Relaxed) != 0;
                    v.total_ms = bits_float(info.total_ms_bits.load(Relaxed));
                    v.line_ms = bits_float(info.line_ms_bits.load(Relaxed));
                    v.over_limit = info.over_limit.load(Relaxed) != 0;
                    v.leak_detected = info.leak_detected.load(Relaxed) != 0;
                    v.echo_in_master = info.echo_in_master.load(Relaxed) != 0;
                }
                v
            })
            .collect()
    }

    /// The name a friend has in shared memory (what other plug-ins list).
    pub fn published_name(bus: &BusLayout, slot: usize) -> String {
        read_friend_name(&bus.friends.0[slot])
    }
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn add_remove_rename_and_slots() {
        let mut r = FriendRoom::new();
        let a = r.add("Mint").unwrap();
        let b = r.add("  ").unwrap();
        assert_eq!(r.find(a).unwrap().slot, 0);
        assert_eq!(
            r.find(b).unwrap().name,
            "Friend 2",
            "an empty name becomes Friend N"
        );
        assert!(r.find(a).unwrap().token.len() == 26 && r.token(a) != r.token(b));
        assert!(r.remove(a));
        assert!(!r.remove(a));
        let c = r.add("Fah").unwrap();
        assert_eq!(r.find(c).unwrap().slot, 0, "the free slot is used again");
        assert!(c > b, "ids are never reused");
        r.rename(c, "");
        assert_eq!(r.find(c).unwrap().name, "Friend 1");
        let old = r.token(c);
        let new = r.new_link(c);
        assert!(new.len() == 26 && new != old && r.token(c) == new);
        assert_eq!(r.new_link(9999), "");
    }

    #[test]
    fn the_room_has_eight_places() {
        let mut r = FriendRoom::new();
        for i in 0..MAX {
            assert!(r.add(&format!("F{i}")).is_some());
        }
        assert!(r.full());
        assert!(r.add("one more").is_none());
    }

    #[test]
    fn levels_are_limited_and_spread() {
        let mut r = FriendRoom::new();
        let a = r.add("A").unwrap();
        let b = r.add("B").unwrap();
        r.set_volume_db(a, 40.0);
        r.set_pan(a, -9.0);
        assert_eq!(
            (r.find(a).unwrap().volume_db, r.find(a).unwrap().pan),
            (6.0, -1.0)
        );
        r.spread_out();
        assert!(
            (r.find(a).unwrap().pan + 0.6).abs() < 1e-6
                && (r.find(b).unwrap().pan - 0.6).abs() < 1e-6
        );
    }

    #[test]
    fn a_nan_level_changes_nothing() {
        let mut r = FriendRoom::new();
        let a = r.add("A").unwrap();
        r.set_volume_db(a, -4.0);
        r.set_pan(a, 0.5);
        r.set_volume_db(a, f32::NAN);
        r.set_pan(a, f32::NAN);
        let e = r.find(a).unwrap();
        assert_eq!((e.volume_db, e.pan), (-4.0, 0.5));
        r.set_volume_db(a, f32::INFINITY);
        assert_eq!(r.find(a).unwrap().volume_db, 6.0);
    }

    #[test]
    fn the_state_brings_the_same_links_back() {
        let mut r = FriendRoom::new();
        let a = r.add("Mint").unwrap();
        let b = r.add("Beam").unwrap();
        r.set_volume_db(b, -3.5);
        r.set_solo(b, true);
        r.remove(a);
        let state = r.to_state();
        let (tb, nb) = (r.token(b), r.find(b).unwrap().name.clone());
        let mut q = FriendRoom::new();
        q.from_state(&state);
        assert_eq!(q.count(), 1);
        assert_eq!((q.token(b), q.find(b).unwrap().name.clone()), (tb, nb));
        assert_eq!(q.find(b).unwrap().volume_db, -3.5);
        assert!(q.find(b).unwrap().solo);
        let c = q.add("New").unwrap();
        assert!(c > b, "the next id carries on");
    }

    #[test]
    fn a_state_cannot_overflow_ids_or_bring_wild_levels() {
        let tok = "abcdefghijkmnpqrstuvwxyz23";
        let mut q = FriendRoom::new();
        q.from_state(&json!({"nextId": 4294967295u64, "friends": [
            {"id": 4294967295u64, "name": "max", "token": tok},
            {"id": 4294967294u64, "name": "near", "token": tok, "vol": 500.0, "pan": -7.0},
            {"id": 104, "name": "wild", "token": tok, "vol": -1.0e30, "pan": 99.0},
        ]}));
        assert!(
            q.find(u32::MAX).is_none(),
            "an id with no successor is skipped"
        );
        let near = q.find(u32::MAX - 1).unwrap();
        assert_eq!((near.volume_db, near.pan), (6.0, -1.0));
        let wild = q.find(104).unwrap();
        assert_eq!((wild.volume_db, wild.pan), (-60.0, 1.0));
        // the ids are used up: another friend is refused, never a wrapped id
        assert!(q.add("late").is_none());
        let mut z = FriendRoom::new();
        z.from_state(&json!({"nextId": 5, "friends": [{"id": 101, "name": "a", "token": tok, "vol": null, "pan": "x"}]}));
        assert_eq!(
            (z.find(101).unwrap().volume_db, z.find(101).unwrap().pan),
            (0.0, 0.0)
        );
    }

    #[test]
    fn a_damaged_state_makes_no_links_up() {
        let mut q = FriendRoom::new();
        q.from_state(&json!({"nextId": 5, "friends": [
            {"id": 101, "name": "short", "token": "abc"},
            {"id": 0, "name": "no id", "token": "abcdefghijkmnpqrstuvwxyz23"},
            {"id": 102, "name": "ok", "token": "abcdefghijkmnpqrstuvwxyz23"},
            {"id": 102, "name": "twice", "token": "abcdefghijkmnpqrstuvwxyz23"},
        ]}));
        assert_eq!(q.count(), 1);
        assert_eq!(q.find(102).unwrap().name, "ok");
        assert!(q.add("n").unwrap() >= 103);
        let mut z = FriendRoom::new();
        z.from_state(&json!("garbage"));
        assert_eq!(z.count(), 0);
    }
}
