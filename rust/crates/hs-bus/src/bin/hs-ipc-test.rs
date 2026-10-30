//! Cross-process bus test, the Rust twin of `tests/integration/ipc_test.cpp` (same CLI, same sample
//! pattern, so either side can be the writer or the reader):
//!   hs-ipc-test [--writer-cmd <exe>]   reader: spawns `<exe> --writer <bus>` (default: this program),
//!                                      checks every sample, kills the writer, checks slot reclaim
//!   hs-ipc-test --writer <bus>         writer: claims a slot, writes ~2 s of audio, then idles
//! Exit code 0 = passed.
use hs_bus::bus::{claim_slot, read_slot_identity, reclaim_dead_slots};
use hs_bus::protocol::{SlotState, kMaxSlots};
use hs_bus::ring::{ReadResult, ring_read, ring_write};
use hs_bus::shm::{SharedMemory, current_pid, now_ns};
use std::process::{Command, ExitCode};
use std::sync::atomic::Ordering::{Acquire, Relaxed};
use std::time::{Duration, Instant};

const BLOCK: usize = 256;

fn value_at(frame: u64, ch: usize) -> f32 {
    (frame % 16_000_000) as i64 as f32 * if ch == 0 { 1.0 } else { -1.0 }
}

fn writer(bus: &str, seconds: f64) -> ExitCode {
    let Ok(shm) = SharedMemory::open(bus) else {
        return ExitCode::from(2);
    };
    let layout = shm.layout();
    let Ok(idx) = claim_slot(layout, "ipc-writer", 48000, 2) else {
        return ExitCode::from(3);
    };
    let slot = &layout.slots[idx];
    let (mut l, mut r) = (vec![0.0f32; BLOCK], vec![0.0f32; BLOCK]);
    let mut frame = 0u64;
    let end = Instant::now() + Duration::from_secs_f64(seconds);
    while Instant::now() < end {
        for i in 0..BLOCK {
            l[i] = value_at(frame + i as u64, 0);
            r[i] = value_at(frame + i as u64, 1);
        }
        slot.heartbeatNs.store(now_ns(), Relaxed);
        ring_write(
            &layout.slotAudio.0[idx],
            &slot.writePos.0,
            &[&l, &r],
            BLOCK as u32,
        );
        frame += BLOCK as u64;
        std::thread::sleep(Duration::from_micros(500)); // ~10x real time
    }
    // keep the slot claimed and idle without releasing (the reader kills us: simulates a crash)
    std::thread::sleep(Duration::from_secs(30));
    ExitCode::SUCCESS
}

fn reader(writer_exe: Option<String>) -> ExitCode {
    let bus = format!("ipc_{}", current_pid());
    let Ok(shm) = SharedMemory::open(&bus) else {
        println!("cannot open bus");
        return ExitCode::FAILURE;
    };
    let layout = shm.layout();
    let exe = writer_exe.map_or_else(|| std::env::current_exe().unwrap(), Into::into);
    let mut child = match Command::new(&exe).args(["--writer", &bus]).spawn() {
        Ok(c) => c,
        Err(e) => {
            println!("spawn failed: {e}");
            return ExitCode::FAILURE;
        }
    };

    // find the writer's slot
    let mut found = None;
    for _ in 0..2000 {
        found = (0..kMaxSlots).find(|&i| {
            layout.slots[i].state.load(Acquire) == SlotState::kSlotActive
                && read_slot_identity(&layout.slots[i]).is_some_and(|(_, u, _)| u == "ipc-writer")
        });
        if found.is_some() {
            break;
        }
        std::thread::sleep(Duration::from_millis(1));
    }
    let Some(idx) = found else {
        println!("writer slot not found");
        let _ = child.kill();
        return ExitCode::FAILURE;
    };
    let slot = &layout.slots[idx];

    let mut cursor = slot.writePos.0.load(Acquire);
    let (mut checked, mut bad, mut overruns) = (0u64, 0u64, 0u64);
    let (mut l, mut r) = (vec![0.0f32; 4096], vec![0.0f32; 4096]);
    let end = Instant::now() + Duration::from_millis(1800);
    while Instant::now() < end {
        let w = slot.writePos.0.load(Acquire);
        let n = w.saturating_sub(cursor).min(4096) as u32;
        if n == 0 {
            std::thread::yield_now();
            continue;
        }
        let start = cursor;
        let res = ring_read(
            &layout.slotAudio.0[idx],
            &slot.writePos.0,
            &mut cursor,
            &mut [&mut l, &mut r],
            n,
        );
        if res == ReadResult::Overrun {
            overruns += 1;
            cursor = slot.writePos.0.load(Acquire).saturating_sub(1024);
            continue;
        }
        for i in 0..n as usize {
            if l[i] != value_at(start + i as u64, 0) || r[i] != value_at(start + i as u64, 1) {
                bad += 1;
            }
            checked += 1;
        }
    }
    println!("checked {checked} frames across processes, {bad} mismatches, {overruns} overruns");

    // kill the writer, then the slot must be reclaimable once its heartbeat is stale
    let _ = child.kill();
    let _ = child.wait();
    slot.heartbeatNs
        .store(now_ns().saturating_sub(10_000_000_000), Relaxed);
    let reclaimed = reclaim_dead_slots(layout, 5_000_000_000);
    println!("reclaimed {reclaimed} dead slot(s)");

    let ok = checked > 20_000
        && bad == 0
        && reclaimed == 1
        && slot.state.load(Acquire) == SlotState::kSlotFree;
    println!(
        "{}",
        if ok {
            "IPC TEST PASSED"
        } else {
            "IPC TEST FAILED"
        }
    );
    if ok {
        ExitCode::SUCCESS
    } else {
        ExitCode::FAILURE
    }
}

fn main() -> ExitCode {
    let args: Vec<String> = std::env::args().skip(1).collect();
    match args.as_slice() {
        [f, bus, ..] if f == "--writer" => writer(bus, 2.0),
        [f, exe, ..] if f == "--writer-cmd" => reader(Some(exe.clone())),
        _ => reader(None),
    }
}
