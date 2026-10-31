//! Real ASIO drivers on this machine (Windows only, ignored by default: loading a driver can open its dialogs and
//! needs the interface plugged in). Run by hand:
//!
//!   cargo test -p hs-win --test installed -- --ignored --nocapture
//!       lists `HKLM\SOFTWARE\ASIO`
//!   HS_ASIO_LOAD="FlexASIO" cargo test ... (or HS_ASIO_LOAD="*" for all): loads each named driver and prints its
//!       channels, rates, buffer sizes and latencies
//!   HS_ASIO_RUN=1 together with HS_ASIO_LOAD: also opens the first two inputs and outputs for two seconds with a
//!       silent output callback, and prints how many buffer switches came and with what jitter
#![cfg(windows)]

use hs_win::asio::{AudioCallback, Block, DeviceType, StreamInfo};
use std::sync::Arc;
use std::sync::atomic::{AtomicU64, AtomicUsize, Ordering::Relaxed};
use std::time::{Duration, Instant};

#[test]
#[ignore = "reads the machine's real ASIO driver list"]
fn list_installed_drivers() {
    let t = DeviceType::scan();
    let names: Vec<&str> = t.device_names().collect();
    println!("{} ASIO driver(s) in HKLM\\SOFTWARE\\ASIO:", names.len());
    for n in &names {
        println!("  {n}");
        assert!(t.create_device(n).is_some());
    }
}

struct Counter {
    blocks: Arc<AtomicUsize>,
    peak_in: Arc<AtomicU64>,
    last: Option<Instant>,
    worst_gap_us: Arc<AtomicU64>,
}

impl AudioCallback for Counter {
    fn about_to_start(&mut self, info: &StreamInfo) {
        println!("    started: {info:?}");
    }
    fn process(&mut self, io: &mut Block<'_>) {
        let now = Instant::now();
        if let Some(l) = self.last {
            self.worst_gap_us
                .fetch_max(now.duration_since(l).as_micros() as u64, Relaxed);
        }
        self.last = Some(now);
        let peak = (0..io.num_inputs())
            .flat_map(|c| io.input(c).iter().copied())
            .fold(0.0f32, |m, v| m.max(v.abs()));
        self.peak_in.fetch_max((peak * 1.0e6) as u64, Relaxed);
        self.blocks.fetch_add(1, Relaxed);
    }
    fn stopped(&mut self) {
        println!("    stopped");
    }
}

#[test]
#[ignore = "loads real drivers named in HS_ASIO_LOAD"]
fn load_named_drivers() {
    let Ok(want) = std::env::var("HS_ASIO_LOAD") else {
        println!("set HS_ASIO_LOAD to a driver name (or *) to load it");
        return;
    };
    let run = std::env::var("HS_ASIO_RUN").is_ok();
    let t = DeviceType::scan();
    let names: Vec<String> = t
        .device_names()
        .filter(|n| want == "*" || *n == want)
        .map(str::to_owned)
        .collect();
    assert!(!names.is_empty(), "no installed driver is called {want:?}");
    for name in names {
        println!("{name}");
        let mut d = t.create_device(&name).unwrap();
        let (ins, outs) = (
            d.input_channel_names().to_vec(),
            d.output_channel_names().to_vec(),
        );
        if ins.is_empty() && outs.is_empty() {
            println!(
                "    did not load: {}",
                d.open(&[true], &[true], 0.0, 0).unwrap_err()
            );
            continue;
        }
        println!("    inputs  {ins:?}");
        println!("    outputs {outs:?}");
        println!(
            "    rates {:?}, buffer sizes {:?}, preferred {}",
            d.available_sample_rates(),
            d.available_buffer_sizes(),
            d.default_buffer_size()
        );
        if !run {
            continue;
        }
        let on = |n: usize| (0..n).map(|c| c < 2).collect::<Vec<_>>();
        match d.open(&on(ins.len()), &on(outs.len()), 0.0, 0) {
            Err(e) => println!("    open failed: {e}"),
            Ok(()) => {
                println!(
                    "    open: {} Hz, {} samples, latency in {} out {}, {} bit",
                    d.current_sample_rate(),
                    d.current_buffer_size(),
                    d.input_latency(),
                    d.output_latency(),
                    d.current_bit_depth()
                );
                let (blocks, peak, gap) = (
                    Arc::new(AtomicUsize::new(0)),
                    Arc::new(AtomicU64::new(0)),
                    Arc::new(AtomicU64::new(0)),
                );
                assert!(d.start(Box::new(Counter {
                    blocks: blocks.clone(),
                    peak_in: peak.clone(),
                    last: None,
                    worst_gap_us: gap.clone()
                })));
                std::thread::sleep(Duration::from_secs(2));
                let expect = 2.0 * d.current_sample_rate() / f64::from(d.current_buffer_size());
                println!(
                    "    {} buffer switches in 2 s (expected about {expect:.0}), worst gap {} us, input peak {:.3}, restart asked: {}",
                    blocks.load(Relaxed),
                    gap.load(Relaxed),
                    peak.load(Relaxed) as f64 / 1.0e6,
                    d.poll_restart()
                );
                d.close();
            }
        }
    }
}
