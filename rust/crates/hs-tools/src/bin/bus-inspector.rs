//! bus-inspector: live view of a HEARASIDE bus (slot table, heartbeats, fill levels).
//! Rust port of `tools/bus-inspector/main.cpp`; same arguments and same output layout.
//!   bus-inspector [BusName] [--once]
use hs_bus::bus::{bits_float, hub_alive, read_slot_identity, read_source_identity};
use hs_bus::protocol::*;
use hs_bus::shm::{SharedMemory, Status, now_ns};
use std::io::Write;
use std::sync::atomic::Ordering::Relaxed;
use std::time::Duration;

fn age_ms(ns: u64) -> f64 {
    if ns == 0 {
        return -1.0;
    }
    let now = now_ns();
    (now.saturating_sub(ns)) as f64 / 1.0e6
}

fn db(lin: f32) -> f64 {
    if lin <= 1e-9 {
        -120.0
    } else {
        20.0 * f64::from(lin).log10()
    }
}

fn flag(v: u32, bit: u32, s: &'static str) -> &'static str {
    if v & bit != 0 { s } else { "" }
}

fn print_bus(bus: &str, shm: Option<&SharedMemory>, err: Option<Status>) {
    println!(
        "HEARASIDE bus \"{}\"  ({}, protocol v{}, {:.1} MB)",
        bus,
        SharedMemory::segment_name(bus),
        kProtocolVersion,
        kLayoutSize as f64 / 1_048_576.0
    );
    let Some(shm) = shm else {
        let why = match err {
            Some(Status::CreateFailed) | None => "no plug-in has opened this bus yet".to_owned(),
            Some(e) => e.to_string(),
        };
        println!("  not available: {why}");
        return;
    };
    let l = shm.layout();
    let h = &l.header;
    let f = h.hubFlags.load(Relaxed);
    println!(
        "Hub: {}  pid={}  {} Hz  block={}  heartbeat={:.0} ms  flags={}{}{}{}{}",
        if hub_alive(l, 1_000_000_000) {
            "ALIVE"
        } else {
            "none "
        },
        h.hubOwnerPid.load(Relaxed),
        h.hubSampleRate.load(Relaxed),
        h.hubBlockSize.load(Relaxed),
        age_ms(h.hubHeartbeatNs.load(Relaxed)),
        flag(f, HubFlags::kHubPreview, "preview "),
        flag(f, HubFlags::kHubPanic, "PANIC "),
        flag(f, HubFlags::kHubBypassed, "bypassed "),
        flag(f, HubFlags::kHubOffline, "offline "),
        flag(f, HubFlags::kHubLimiter, "limiter"),
    );
    println!(
        "Consumer (OBS/Bridge): heartbeat={:.0} ms  buffer={} ms",
        age_ms(h.consumerHeartbeatNs.load(Relaxed)),
        h.consumerBufferMs.load(Relaxed)
    );
    let sh = &l.streamHeader;
    println!(
        "Stream Mix: active={}  {} Hz  writePos={}  peak L {:.1} / R {:.1} dBFS  LUFS-M {:.1}  LUFS-S {:.1}\n",
        sh.active.load(Relaxed),
        sh.sampleRate.load(Relaxed),
        sh.writePos.0[0].load(Relaxed),
        db(bits_float(sh.peakBits[0][0].load(Relaxed))),
        db(bits_float(sh.peakBits[0][1].load(Relaxed))),
        bits_float(sh.loudnessMBits.load(Relaxed)),
        bits_float(sh.loudnessSBits.load(Relaxed)),
    );
    println!(
        " # state  pid    name                 rate   MON STR SOLO  gain   pan  delay stem  in dB  str dB   lead  hb ms  status"
    );
    for (i, s) in l.slots.iter().enumerate() {
        let state = s.state.load(Relaxed);
        if state == SlotState::kSlotFree {
            continue;
        }
        let name = read_slot_identity(s).map(|(n, ..)| n).unwrap_or_default();
        let fl = s.flags.load(Relaxed);
        let hs = s.hubStatus.load(Relaxed);
        let on = |bit, yes: &'static str, no: &'static str| if fl & bit != 0 { yes } else { no };
        println!(
            "{:2} {:<6} {:<6} {:<20.20} {:<6}  {}   {}   {}  {:+5.1} {:+5.2} {:5.1}  {:3}  {:6.1}  {:6.1} {:6} {:6.0}  {}{}{}{}{}{}",
            i,
            if state == SlotState::kSlotActive {
                "active"
            } else {
                "claim"
            },
            s.ownerPid.load(Relaxed),
            if name.is_empty() { "(unnamed)" } else { &name },
            s.sampleRate.load(Relaxed),
            on(SlotFlags::kFlagMon, "on ", "off"),
            on(SlotFlags::kFlagStr, "on ", "off"),
            on(SlotFlags::kFlagSolo, "on ", "-  "),
            bits_float(s.strGainBits.load(Relaxed)),
            bits_float(s.strPanBits.load(Relaxed)),
            bits_float(s.strDelayBits.load(Relaxed)),
            s.stemIndex.load(Relaxed),
            db(bits_float(s.peakInBits[0].load(Relaxed))),
            db(bits_float(s.peakStreamBits[0].load(Relaxed))),
            s.hubLeadFrames.load(Relaxed),
            age_ms(s.heartbeatNs.load(Relaxed)),
            flag(hs, SlotHubStatus::kHubStatusOk, "ok "),
            flag(hs, SlotHubStatus::kHubStatusRateMismatch, "RATE "),
            flag(hs, SlotHubStatus::kHubStatusAhead, "AHEAD "),
            flag(hs, SlotHubStatus::kHubStatusUnderrun, "underrun "),
            flag(fl, SlotFlags::kFlagBypassed, "BYPASSED "),
            flag(fl, SlotFlags::kFlagOffline, "offline"),
        );
    }
    println!(
        "\nApp Audio sources\n # pid    name                 app                  capture  level  peak dB  latency  delay  slot  hb ms"
    );
    for (i, s) in l.sources.iter().enumerate() {
        if s.state.load(Relaxed) == SlotState::kSlotFree {
            continue;
        }
        let (name, app, _) = read_source_identity(s).unwrap_or_default();
        println!(
            "{:2} {:<6} {:<20.20} {:<20.20} {}        {:+5.1}  {:6.1}   {:5.1}  {:5.1}  {:4}  {:6.0}",
            i,
            s.ownerPid.load(Relaxed),
            if name.is_empty() { "(unnamed)" } else { &name },
            app,
            s.capture.load(Relaxed),
            bits_float(s.levelBits.load(Relaxed)),
            db(bits_float(s.peakBits.load(Relaxed))),
            bits_float(s.latencyBits.load(Relaxed)),
            bits_float(s.delayBits.load(Relaxed)),
            s.trackSlot.load(Relaxed),
            age_ms(s.heartbeatNs.load(Relaxed)),
        );
    }
}

fn main() {
    let mut bus = "Main".to_owned();
    let mut once = false;
    for a in std::env::args().skip(1) {
        match a.as_str() {
            "--once" => once = true,
            "--help" => {
                println!("usage: bus-inspector [BusName] [--once]");
                return;
            }
            _ => bus = a,
        }
    }
    let mut shm: Option<SharedMemory> = None;
    let mut err = None;
    loop {
        if shm.is_none() {
            match SharedMemory::open_existing(&bus) {
                Ok(s) => shm = Some(s),
                Err(e) => err = Some(e),
            }
        }
        if !once {
            print!("\x1b[2J\x1b[H");
        }
        print_bus(&bus, shm.as_ref(), err);
        if once {
            break;
        }
        let _ = std::io::stdout().flush();
        std::thread::sleep(Duration::from_millis(250));
    }
}
