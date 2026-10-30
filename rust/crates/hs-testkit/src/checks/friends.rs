//! `--test-friends`: the friends room over real sockets (S2): one link per friend, microphone in, the mix out with
//! positions, delay measured from the echo, newest connection wins, a removed friend is cut off, a take on disk, and the
//! room coming back with the project.
use super::wait_for;
use crate::hub::{HubOptions, TestHub};
use crate::report::Report;
use crate::ws::{WsClient, http_get};
use hs_bus::bus::{bits_float, read_friend_name};
use hs_bus::protocol::FriendState;
use hs_bus::shm::SharedMemory;
use serde_json::Value;
use std::path::Path;
use std::sync::atomic::{AtomicBool, AtomicI32, Ordering::SeqCst};
use std::time::Duration;

const HRA2: u32 = 0x3241_5248;

/// 20 ms of microphone (HRA2), saying "I was hearing the Stream Mix `echo` frames ago": 960 stereo frames.
fn mic_packet(i: usize, echo: u64) -> Vec<u8> {
    let mut pk = vec![0u8; 24 + 960 * 4];
    pk[0..4].copy_from_slice(&HRA2.to_le_bytes());
    pk[4..8].copy_from_slice(&48_000u32.to_le_bytes());
    pk[8..10].copy_from_slice(&2u16.to_le_bytes());
    pk[10..12].copy_from_slice(&0u16.to_le_bytes());
    pk[12..16].copy_from_slice(&960u32.to_le_bytes());
    pk[16..24].copy_from_slice(&echo.to_le_bytes());
    for k in 0..960 {
        let v = (8000.0 * (0.05 * (i * 960 + k) as f64).sin()) as i16;
        pk[24 + 4 * k..26 + 4 * k].copy_from_slice(&v.to_le_bytes());
        pk[26 + 4 * k..28 + 4 * k].copy_from_slice(&v.to_le_bytes());
    }
    pk
}

fn friend_state(bus: &SharedMemory, slot: usize) -> u32 {
    bus.layout().friends.0[slot].state.load(SeqCst)
}

/// The client: what the friends' browsers do, on a thread of its own (the hub pumps on the main thread).
#[allow(clippy::too_many_lines)]
fn client(
    r: &Report,
    bus: &SharedMemory,
    port: u16,
    t_mint: &str,
    t_beam: &str,
    state: &AtomicI32,
    finished: &AtomicBool,
) {
    let layout = bus.layout();
    let (status, body) = http_get(port, &format!("/s/{t_mint}"));
    r.expect(
        "GET /s/<token> serves the page",
        status == 200 && body.contains("HEARASIDE"),
    );
    let (status, body) = http_get(port, &format!("/info/s/{t_mint}"));
    let name: Value = serde_json::from_str(&body).unwrap_or(Value::Null);
    r.expect(
        "GET /info/s/<token> names the friend",
        status == 200 && name["name"] == "Mint",
    );
    let (status, _) = http_get(port, "/s/aaaaaaaaaaaaaaaaaaaaaaaaaa");
    r.expect("a link nobody has -> 404", status == 404);
    let (status, _) = http_get(port, "/l/x");
    r.expect(
        "listen links are off while sharing is off -> 404",
        status == 404,
    );
    let mut evil = WsClient::new();
    evil.open(port, &format!("/ws/s/{t_mint}"), "https://evil.example");
    r.expect(
        "a web page from another site may not connect -> 403",
        evil.status == 403,
    );

    let mut a = WsClient::new();
    r.expect(
        "the friend's WebSocket opens",
        a.open(port, &format!("/ws/s/{t_mint}"), ""),
    );
    r.expect(
        "the friend shows as singing",
        wait_for(1000, || friend_state(bus, 0) == FriendState::kFriendLive),
    );
    let (mut down_frames, mut down_packets, mut rate) = (0u32, 0u32, 0u32);
    let mut last_end = 0u64;
    let (mut contiguous, mut saw_hello) = (true, false);
    let mut hello_name = String::new();
    for i in 0..60 {
        let w = layout.friendMixWrite.0.load(SeqCst);
        let echo = if w > 4800 { w - 4800 } else { 1 };
        a.send(&mic_packet(i, echo));
        // what comes back: the mix with its position
        for _ in 0..4 {
            let mut closed = false;
            let Some(m) = a.receive(5, &mut closed) else {
                continue;
            };
            if m.len() < 24 {
                continue;
            }
            let mg = u32::from_le_bytes(m[0..4].try_into().unwrap_or_default());
            rate = u32::from_le_bytes(m[4..8].try_into().unwrap_or_default());
            let fr = u32::from_le_bytes(m[12..16].try_into().unwrap_or_default());
            let pos = u64::from_le_bytes(m[16..24].try_into().unwrap_or_default());
            if mg != HRA2 {
                // text: the hello ({"v":2,"name":...}) or the delay report
                let v: Value = serde_json::from_slice(&m).unwrap_or(Value::Null);
                if v["v"].as_i64().unwrap_or(0) >= 2 {
                    saw_hello = true;
                    hello_name = v["name"].as_str().unwrap_or("").to_owned();
                }
                continue;
            }
            if last_end != 0 && pos != last_end {
                contiguous = false;
            }
            last_end = pos + u64::from(fr);
            down_frames += fr;
            down_packets += 1;
        }
        std::thread::sleep(Duration::from_millis(18));
    }
    r.expect(
        "the friend hears the Stream Mix in HRA2 packets with positions",
        down_packets > 10 && rate > 0 && down_frames > 4800,
    );
    r.expect(
        "the Hub greets the friend by name first",
        saw_hello && hello_name == "Mint",
    );
    r.expect(
        "those positions are contiguous (nothing lost, nothing repeated)",
        contiguous,
    );
    r.expect(
        "the friend's audio is in the bus",
        layout.friends.0[0].writePos.0.load(SeqCst) >= 40 * 960,
    );
    let d = bits_float(layout.friends.0[0].delayBits.load(SeqCst));
    r.line(format!(
        "     measured delay {d:.1} ms (the echo said 100 ms heard, minus half a 20 ms packet = about 90)"
    ));
    r.expect(
        "the delay was measured from the echo (about 90 ms)",
        d > 70.0 && d < 130.0,
    );

    let mut b = WsClient::new();
    r.expect(
        "the same friend connects again (a second tab)",
        b.open(port, &format!("/ws/s/{t_mint}"), ""),
    );
    let mut closed = false;
    for _ in 0..80 {
        if closed {
            break;
        }
        a.receive(25, &mut closed);
    }
    r.expect(
        "the newest connection wins: the first one is closed",
        closed,
    );
    r.expect(
        "the friend is still singing (on the new connection)",
        friend_state(bus, 0) == FriendState::kFriendLive,
    );

    let mut other = WsClient::new();
    r.expect(
        "another friend has their own slot",
        other.open(port, &format!("/ws/s/{t_beam}"), ""),
    );
    r.expect(
        "without closing the first friend",
        friend_state(bus, 1) == FriendState::kFriendLive
            && friend_state(bus, 0) == FriendState::kFriendLive,
    );
    state.store(1, SeqCst);
    for _ in 0..400 {
        if state.load(SeqCst) != 1 {
            break;
        }
        std::thread::sleep(Duration::from_millis(10)); // the main thread removes Mint
    }
    let mut closed_b = false;
    for _ in 0..100 {
        if closed_b {
            break;
        }
        b.receive(25, &mut closed_b);
    }
    r.expect("a removed friend is cut off", closed_b);
    let (status, _) = http_get(port, &format!("/s/{t_mint}"));
    r.expect("and their link no longer works -> 404", status == 404);
    r.expect(
        "the other friend is not affected",
        friend_state(bus, 1) == FriendState::kFriendLive,
    );
    finished.store(true, SeqCst);
}

pub fn run(r: &Report, opts: &HubOptions) -> i32 {
    let mut hub = TestHub::new(opts);
    r.expect(
        "no friends: the share server is not running",
        !hub.share().running(),
    );
    let mint = hub.add_friend("Mint");
    let beam = hub.add_friend("Beam");
    let (Some(mint), Some(beam)) = (mint, beam) else {
        r.expect("two friends added", false);
        return r.finish("friends room");
    };
    r.expect("two friends added", hub.friend_count() == 2);
    hub.pump_until(5000, |h| h.share().running());
    r.expect(
        "the share server starts for the friends even with the listen link off",
        hub.share().running() && !hub.sharing(),
    );
    let port = hub.share().port();
    let (t_mint, t_beam) = (hub.friend_token(mint), hub.friend_token(beam));
    r.expect(
        "links are 26 characters of secure random and differ",
        t_mint.len() == 26 && t_beam.len() == 26 && t_mint != t_beam,
    );
    // the link a friend gets is the share server's link with their token (a computer with no network address has none:
    // then the link is empty too and this check says so instead of passing)
    let link = hub.friend_link(mint);
    if hub.share().friend_url("x").is_empty() {
        r.line("     no network address on this computer: the friend link is empty, link checks skipped");
        r.expect(
            "without an address the friend link is empty",
            link.is_empty(),
        );
    } else {
        r.expect(
            "the link a friend gets is the share server's link with their token",
            link == hub.share().friend_url(&t_mint) && link.ends_with(&format!("/s/{t_mint}")),
        );
    }
    let Some(bus) = hub.shared_memory() else {
        r.expect("the Hub is on its bus", false);
        return r.finish("friends room");
    };
    let state = AtomicI32::new(0);
    let finished = AtomicBool::new(false);
    std::thread::scope(|scope| {
        scope.spawn(|| client(r, &bus, port, &t_mint, &t_beam, &state, &finished));
        hub.start_friend_recording(mint);
        r.expect("Mint recording started", hub.is_friend_recording(mint));
        for _ in 0..2000 {
            if state.load(SeqCst) == 1 || finished.load(SeqCst) {
                break;
            }
            hub.pump(10);
        }
        r.expect(
            "friend has recorded audio seconds",
            hub.friend_record_sec(mint) > 0.0,
        );
        hub.stop_friend_recording(mint);
        r.expect("friend recording stopped", !hub.is_friend_recording(mint));
        let take = hub.friend_take(mint);
        r.expect(
            "Mint WAV take was written to disk",
            take.as_deref().is_some_and(|t| take_is_ok(t, "Mint ")),
        );
        if let Some(t) = take {
            let _ = std::fs::remove_file(t);
        }
        if state.load(SeqCst) == 1 {
            hub.remove_friend(mint);
            hub.pump(50);
            state.store(2, SeqCst);
        }
        for _ in 0..2000 {
            if finished.load(SeqCst) {
                break;
            }
            hub.pump(10);
        }
    });
    let l = bus.layout();
    r.expect(
        "the removed friend's slot is free",
        l.friends.0[0].id.load(SeqCst) == 0
            && l.friends.0[0].state.load(SeqCst) == FriendState::kFriendFree,
    );
    r.expect(
        "the removed friend's name left shared memory",
        read_friend_name(&l.friends.0[0]).is_empty(),
    );
    // the room is saved with the project; the links survive a reload
    let saved = hub.save_state();
    let saved_token = hub.friend_token(beam);
    hub.remove_friend(beam);
    r.expect(
        "an empty room stops the server again",
        hub.pump_until(3000, |h| !h.share().running()),
    );
    r.expect("the saved project loads", hub.load_state(&saved));
    hub.pump(300);
    let back = hub.pump_until(3000, |h| h.share().running())
        && hub.friend_count() == 1
        && hub.friends().first().is_some_and(|f| f.name == "Beam")
        && hub
            .friends()
            .first()
            .is_some_and(|f| hub.friend_token(f.id) == saved_token);
    r.expect("the project brings Beam back with the same link", back);
    // permanent links: the same link every stream, through the share web site
    hub.settings().set_permanent_links(true);
    hub.settings().set_share_base("https://share.example");
    r.expect(
        "permanent links: the friend link is the share site's /s/<token>",
        hub.permanent_links_set()
            && hub.friends().first().is_some_and(|f| {
                hub.friend_link(f.id)
                    == format!("https://share.example/s/{}", hub.friend_token(f.id))
            }),
    );
    hub.settings().set_share_base("");
    r.expect(
        "and without a share site the links are the server's again",
        !hub.permanent_links_set(),
    );
    r.finish("friends room")
}

/// A finished take: a file `<prefix>*.wav` of more than 1 KiB.
fn take_is_ok(path: &Path, prefix: &str) -> bool {
    path.file_name().is_some_and(|n| {
        n.to_string_lossy().starts_with(prefix) && n.to_string_lossy().ends_with(".wav")
    }) && std::fs::metadata(path).is_ok_and(|m| m.len() > 1024)
}
