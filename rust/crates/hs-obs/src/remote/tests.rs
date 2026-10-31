//! R4.2 tests: the hotkey paths post the same remote commands as `obs/hearaside-obs/src/remote.cpp`.
//!
//! Every test uses its own bus segment, created by the test (the "Hub") and opened by the code under test the way
//! OBS does (`open_existing`, a second mapping of the same memory). The golden values are what C++ `postRemote`
//! writes: target `int32 -1`, `paramId` = `RemoteParam`, `valueBits` = `floatBits(1.0f)` (0x3F800000) or 0, and
//! `seq = index + 1`. The `cpp_*` tests check that against the real C++ `BusClient` / `pollRemote` through
//! `ss_remote_probe` (tests/integration/remote_probe.cpp) when `SS_REMOTE_CPP` points at it, like `SS_IPC_CPP` in
//! hs-bus; they print a note and skip otherwise.
use super::*;
use hs_bus::bus::poll_remote;
use hs_bus::protocol::kRemoteQueueSize;
use hs_bus::shm::current_pid;
use std::path::PathBuf;
use std::process::Command;
use std::sync::atomic::AtomicU32;
use std::sync::atomic::Ordering::SeqCst;

const HUB: u32 = 0xFFFF_FFFF; // target -1 as the raw 32 bits in the segment
const ONE: u32 = 0x3F80_0000; // floatBits(1.0f)

static N: AtomicU32 = AtomicU32::new(0);

fn unique_bus() -> String {
    format!("r42_{}_{}", current_pid(), N.fetch_add(1, SeqCst))
}

/// One raw queue entry as stored in the segment: `[seq, target, paramId, valueBits]`.
fn raw(l: &BusLayout, i: usize) -> [u32; 4] {
    let e = &l.header.remote[i];
    [
        e.seq.load(SeqCst),
        e.target.load(SeqCst) as u32,
        e.paramId.load(SeqCst),
        e.valueBits.load(SeqCst),
    ]
}

fn reserve(l: &BusLayout) -> u32 {
    l.header.remoteReserve.load(SeqCst)
}

fn set_hub_flags(l: &BusLayout, flags: u32) {
    l.header.hubFlags.store(flags, SeqCst);
}

fn decode(l: &BusLayout) -> Vec<(i32, u32, f32)> {
    let mut cursor = 0;
    let mut out = Vec::new();
    poll_remote(l, &mut cursor, |t, p, v| out.push((t, p, v)));
    out
}

// ---- the two hotkeys (the module-level functions OBS's hotkey callbacks call) -------------------------------------

#[test]
fn mute_hotkey_posts_the_panic_command() {
    let bus = unique_bus();
    let shm = SharedMemory::open(&bus).unwrap();
    let l = shm.layout();
    toggle_panic(&bus); // Hub not muted -> mute
    assert_eq!(reserve(l), 1);
    assert_eq!(raw(l, 0), [1, HUB, 1, ONE]);
    set_hub_flags(l, HubFlags::kHubPanic);
    toggle_panic(&bus); // Hub muted -> unmute
    assert_eq!(reserve(l), 2);
    assert_eq!(raw(l, 1), [2, HUB, 1, 0]);
    assert_eq!(
        raw(l, 0),
        [1, HUB, 1, ONE],
        "earlier entries are not touched"
    );
}

#[test]
fn preview_hotkey_posts_the_preview_command() {
    let bus = unique_bus();
    let shm = SharedMemory::open(&bus).unwrap();
    let l = shm.layout();
    toggle_preview(&bus);
    assert_eq!(reserve(l), 1);
    assert_eq!(raw(l, 0), [1, HUB, 2, ONE]);
    set_hub_flags(l, HubFlags::kHubPreview);
    toggle_preview(&bus);
    assert_eq!(reserve(l), 2);
    assert_eq!(raw(l, 1), [2, HUB, 2, 0]);
}

#[test]
fn each_hotkey_looks_only_at_its_own_flag() {
    let bus = unique_bus();
    let shm = SharedMemory::open(&bus).unwrap();
    let l = shm.layout();
    // every flag but the panic bit: mute still turns it ON
    set_hub_flags(l, !HubFlags::kHubPanic);
    toggle_panic(&bus);
    assert_eq!(raw(l, 0), [1, HUB, 1, ONE]);
    // every flag but the preview bit: preview still turns it ON
    set_hub_flags(l, !HubFlags::kHubPreview);
    toggle_preview(&bus);
    assert_eq!(raw(l, 1), [2, HUB, 2, ONE]);
    // both bits set: both turn OFF
    set_hub_flags(l, HubFlags::kHubPanic | HubFlags::kHubPreview);
    toggle_panic(&bus);
    toggle_preview(&bus);
    assert_eq!(raw(l, 2), [3, HUB, 1, 0]);
    assert_eq!(raw(l, 3), [4, HUB, 2, 0]);
}

#[test]
fn the_hub_reads_the_commands_back_with_poll_remote() {
    let bus = unique_bus();
    let shm = SharedMemory::open(&bus).unwrap();
    toggle_panic(&bus);
    toggle_preview(&bus);
    assert_eq!(decode(shm.layout()), [(-1, 1, 1.0), (-1, 2, 1.0)]);
}

#[test]
fn hotkeys_from_many_threads_lose_no_command() {
    let bus = unique_bus();
    let shm = SharedMemory::open(&bus).unwrap();
    let (threads, each) = (4, 10);
    assert!(threads * each <= kRemoteQueueSize, "stay inside the queue");
    std::thread::scope(|s| {
        for _ in 0..threads {
            s.spawn(|| {
                for i in 0..each {
                    if i % 2 == 0 {
                        toggle_panic(&bus);
                    } else {
                        toggle_preview(&bus);
                    }
                }
            });
        }
    });
    let got = decode(shm.layout());
    assert_eq!(got.len(), threads * each);
    assert_eq!(got.iter().filter(|c| c.1 == 1).count(), threads * each / 2);
    assert_eq!(got.iter().filter(|c| c.1 == 2).count(), threads * each / 2);
    assert!(got.iter().all(|&(t, _, v)| t == -1 && v == 1.0));
}

// ---- BusClient behaviour (same as the C++ class) ------------------------------------------------------------------

#[test]
fn no_bus_means_nothing_posted_and_nothing_created() {
    let bus = unique_bus();
    toggle_panic(&bus);
    toggle_preview(&bus);
    assert!(
        SharedMemory::open_existing(&bus).is_err(),
        "a hotkey must not create the bus"
    );
    assert!(!BusClient::new().with(&bus, |_| panic!("no bus: must not run")));
}

#[test]
fn opening_a_missing_bus_is_retried_at_most_once_a_second() {
    let bus = unique_bus();
    let c = BusClient::new();
    let t0 = 5_000_000_000;
    assert!(!c.with_at(&bus, t0, |_| ()));
    let _shm = SharedMemory::open(&bus).unwrap(); // the DAW starts now
    assert!(!c.with_at(&bus, t0 + RETRY_NS - 1, |_| ()), "too early");
    assert!(c.with_at(&bus, t0 + RETRY_NS, |_| ()), "one second later");
    let mut ran = false;
    assert!(c.with_at(&bus, t0 + RETRY_NS + 1, |_| ran = true));
    assert!(ran, "an open mapping is used without any wait");
}

#[test]
fn release_all_drops_the_mappings_and_the_retry_limit_still_applies() {
    let bus = unique_bus();
    let _shm = SharedMemory::open(&bus).unwrap();
    let c = BusClient::new();
    let t0 = 5_000_000_000;
    assert!(c.with_at(&bus, t0, |_| ()));
    c.release_all();
    assert!(
        !c.with_at(&bus, t0 + RETRY_NS / 2, |_| ()),
        "like C++: the retry time survives releaseAll"
    );
    assert!(c.with_at(&bus, t0 + RETRY_NS, |_| ()));
}

#[test]
fn buses_are_kept_apart() {
    let (a, b) = (unique_bus(), unique_bus());
    let (sa, sb) = (
        SharedMemory::open(&a).unwrap(),
        SharedMemory::open(&b).unwrap(),
    );
    let c = BusClient::new();
    c.toggle_panic(&a);
    assert_eq!((reserve(sa.layout()), reserve(sb.layout())), (1, 0));
    c.toggle_preview(&b);
    assert_eq!((reserve(sa.layout()), reserve(sb.layout())), (1, 1));
}

// ---- C++ <-> Rust through the real bus segment --------------------------------------------------------------------

fn cpp_probe() -> Option<PathBuf> {
    let Some(p) = std::env::var_os("SS_REMOTE_CPP") else {
        println!("SS_REMOTE_CPP not set: skipped");
        return None;
    };
    let p = PathBuf::from(p);
    assert!(p.is_file(), "SS_REMOTE_CPP does not point at a file: {p:?}");
    Some(p)
}

fn run_probe(exe: &std::path::Path, mode: &str, bus: &str) -> String {
    let out = Command::new(exe).args([mode, bus]).output().unwrap();
    let text = String::from_utf8_lossy(&out.stdout).into_owned();
    assert!(
        out.status.success(),
        "ss_remote_probe {mode} failed: {text}"
    );
    text
}

#[test]
fn cpp_hotkeys_and_rust_hotkeys_write_identical_queue_bytes() {
    let Some(exe) = cpp_probe() else { return };
    let (cpp_bus, rust_bus) = (unique_bus(), unique_bus());
    let cpp_shm = SharedMemory::open(&cpp_bus).unwrap();
    let rust_shm = SharedMemory::open(&rust_bus).unwrap();
    let states = [
        0,
        HubFlags::kHubPanic,
        HubFlags::kHubPreview,
        HubFlags::kHubPanic | HubFlags::kHubPreview,
        !0,
    ];
    for (round, flags) in states.into_iter().enumerate() {
        set_hub_flags(cpp_shm.layout(), flags);
        set_hub_flags(rust_shm.layout(), flags);
        run_probe(&exe, "toggle-panic", &cpp_bus);
        run_probe(&exe, "toggle-preview", &cpp_bus);
        toggle_panic(&rust_bus);
        toggle_preview(&rust_bus);
        assert_eq!(
            reserve(cpp_shm.layout()),
            reserve(rust_shm.layout()),
            "round {round}"
        );
        for i in 0..(2 * (round + 1)) {
            assert_eq!(
                raw(cpp_shm.layout(), i),
                raw(rust_shm.layout(), i),
                "round {round}, entry {i}, hubFlags {flags:#x}"
            );
        }
    }
    assert_eq!(reserve(cpp_shm.layout()), 10);
    // and those C++ bytes are the golden ones
    assert_eq!(raw(cpp_shm.layout(), 0), [1, HUB, 1, ONE]);
    assert_eq!(raw(cpp_shm.layout(), 1), [2, HUB, 2, ONE]);
}

#[test]
fn cpp_probe_with_no_bus_posts_nothing_and_creates_nothing() {
    let Some(exe) = cpp_probe() else { return };
    let bus = unique_bus();
    run_probe(&exe, "toggle-panic", &bus);
    run_probe(&exe, "toggle-preview", &bus);
    assert!(SharedMemory::open_existing(&bus).is_err());
}

#[test]
fn cpp_hub_side_poll_remote_reads_the_rust_commands() {
    let Some(exe) = cpp_probe() else { return };
    let bus = unique_bus();
    let shm = SharedMemory::open(&bus).unwrap();
    toggle_panic(&bus);
    toggle_preview(&bus);
    set_hub_flags(shm.layout(), HubFlags::kHubPanic | HubFlags::kHubPreview);
    toggle_panic(&bus);
    toggle_preview(&bus);
    let out = run_probe(&exe, "poll", &bus);
    let lines: Vec<&str> = out.lines().collect();
    assert_eq!(
        lines,
        [
            "cmd -1 1 3f800000",
            "cmd -1 2 3f800000",
            "cmd -1 1 00000000",
            "cmd -1 2 00000000",
            "POLLED"
        ]
    );
}
