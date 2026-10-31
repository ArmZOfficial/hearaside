//! HAS Direct's device logic (`Device` in HasAsio.cpp) against a fake driver: the order of driver calls, the errors,
//! buffer sizes, the buffer switch, restarts. See `common/mod.rs` for the fake.

mod common;

use common::*;
use hs_win::asio::abi::{
    K_BUFFER_SIZE_CHANGE, K_ENGINE_VERSION, K_LATENCIES_CHANGED, K_RESET_REQUEST, K_RESYNC_REQUEST,
    K_SELECTOR_SUPPORTED,
};
use hs_win::asio::{
    AudioCallback, Block, DeviceType, OpenError, StreamInfo, TYPE_NAME, from_float, to_float,
};
use std::sync::atomic::{AtomicBool, AtomicUsize, Ordering::SeqCst};
use std::sync::{Arc, Mutex};

/// out[c] = in[c] * gain for as many channels as both sides have; remembers what it was told.
struct Gain {
    gain: f32,
    events: Arc<Mutex<Vec<String>>>,
    blocks: Arc<AtomicUsize>,
}

impl Gain {
    fn new(gain: f32) -> (Box<Self>, Arc<Mutex<Vec<String>>>, Arc<AtomicUsize>) {
        let events = Arc::new(Mutex::new(Vec::new()));
        let blocks = Arc::new(AtomicUsize::new(0));
        (
            Box::new(Self {
                gain,
                events: events.clone(),
                blocks: blocks.clone(),
            }),
            events,
            blocks,
        )
    }
}

impl AudioCallback for Gain {
    fn about_to_start(&mut self, info: &StreamInfo) {
        self.events.lock().unwrap().push(format!(
            "start {} {} {}/{}",
            info.sample_rate, info.buffer_size, info.num_inputs, info.num_outputs
        ));
    }
    fn process(&mut self, io: &mut Block<'_>) {
        for c in 0..io.num_outputs().min(io.num_inputs()) {
            for i in 0..io.frames() {
                let v = io.input(c)[i] * self.gain;
                io.output(c)[i] = v;
            }
        }
        self.blocks.fetch_add(1, SeqCst);
    }
    fn stopped(&mut self) {
        self.events.lock().unwrap().push("stopped".into());
    }
}

fn encode(ty: i32, v: &[f32]) -> Vec<u8> {
    let mut b = vec![0u8; hs_win::asio::bytes_per_sample(ty) * v.len()];
    assert!(from_float(ty, v, &mut b));
    b
}

fn decode(ty: i32, b: &[u8]) -> Vec<f32> {
    let mut v = vec![0.0; b.len() / hs_win::asio::bytes_per_sample(ty)];
    assert!(to_float(ty, b, &mut v));
    v
}

#[test]
fn type_name_and_off_windows_list() {
    assert_eq!(TYPE_NAME, "HAS Direct");
    let t = DeviceType::scan();
    // the registry list is machine dependent; what holds everywhere: names and creation agree
    for n in t.device_names() {
        assert!(t.create_device(n).is_some());
    }
    assert!(t.create_device("no such driver, honest").is_none());
}

#[test]
fn the_driver_loads_on_first_real_use_not_before() {
    let _s = serial();
    let (mut d, probe, _) = device(Cfg {
        inputs: vec![(INT24, "Mic"), (FLOAT32, "")],
        outputs: vec![(INT16, ""), (INT16, "R")],
        ..Cfg::default()
    });
    assert_eq!(d.name(), "Fake ASIO");
    // the device manager asks every driver for these: nothing is loaded for them
    assert_eq!(
        d.available_sample_rates(),
        [44100.0, 48000.0, 88200.0, 96000.0]
    );
    assert!(!d.is_open() && !d.is_playing() && d.has_control_panel());
    assert!(probe.log().is_empty());

    assert_eq!(d.input_channel_names(), ["Mic", "In 2"]);
    assert_eq!(d.output_channel_names(), ["Out 1", "R"]); // a driver that gives no name gets "In n" / "Out n"
    assert_eq!(probe.count("init"), 1);
    d.input_channel_names();
    assert_eq!(probe.count("init"), 1);
    // loaded now: only the rates the driver says it can run
    assert_eq!(d.available_sample_rates(), [44100.0, 48000.0, 96000.0]);
    assert_eq!(d.default_buffer_size(), 128);
    assert!(!d.show_control_panel());
    assert_eq!(probe.count("controlPanel"), 1);
    drop(d);
    assert_eq!(probe.released.load(SeqCst), 1);
}

#[test]
fn a_driver_with_no_rate_in_the_list_offers_the_one_it_runs() {
    let _s = serial();
    let (mut d, _, _) = device(Cfg {
        rates: vec![],
        rate: 32000.0,
        ..Cfg::default()
    });
    d.input_channel_names();
    assert_eq!(d.available_sample_rates(), [32000.0]);
}

#[test]
fn buffer_size_lists() {
    let _s = serial();
    let sizes = |buffer: (i32, i32, i32, i32)| {
        let (mut d, _, _) = device(Cfg {
            buffer,
            ..Cfg::default()
        });
        (d.available_buffer_sizes(), d.default_buffer_size())
    };
    // granularity -1: powers of two from min
    assert_eq!(
        sizes((64, 1024, 256, -1)),
        (vec![64, 128, 256, 512, 1024], 256)
    );
    // a granularity: steps of it
    assert_eq!(sizes((64, 256, 128, 64)), (vec![64, 128, 192, 256], 128));
    // the preferred size is added in its place when the steps skip it
    assert_eq!(
        sizes((64, 512, 192, -1)),
        (vec![64, 128, 192, 256, 512], 192)
    );
    // min = max, granularity 0: just the preferred one
    assert_eq!(sizes((256, 256, 256, 0)), (vec![256], 256));
    // min 0 in the power-of-two list starts at 1; a driver with no preferred size defaults to 512
    assert_eq!(sizes((0, 8, 0, -1)), (vec![1, 2, 4, 8], 512));
}

#[test]
fn open_start_and_the_buffer_switch_convert_both_ways() {
    let _s = serial();
    let cfg = Cfg::default(); // in: Int24 Mic, Float32; out: Int16, Int32LSB24, Float64
    let (mut d, probe, ctl) = device(cfg);
    // rate change asked for, size 100 sits inside [64, 256]; inputs both on, outputs 0 and 2 on, 1 off
    d.open(&[true, true], &[true, false, true], 48000.0, 100)
        .unwrap();
    assert!(d.is_open());
    let log = probe.log();
    let at = |what: &str| {
        log.iter()
            .position(|c| c.starts_with(what))
            .unwrap_or_else(|| panic!("{what} not called: {log:?}"))
    };
    // the C++ order: sample rate, buffers (inputs first, then outputs), start
    assert!(
        at("init") < at("setSampleRate 48000")
            && at("setSampleRate 48000") < at("createBuffers 4 100")
            && at("createBuffers 4 100") < at("start")
    );
    assert_eq!(d.current_buffer_size(), 100);
    assert_eq!(d.current_sample_rate(), 48000.0);
    assert_eq!(d.current_bit_depth(), 24); // the first channel's type
    assert_eq!((d.input_latency(), d.output_latency()), (130, 260));
    assert_eq!(d.active_input_channels(), [true, true]);
    assert_eq!(d.active_output_channels(), [true, false, true]);

    let (cb, events, blocks) = Gain::new(0.5);
    assert!(d.start(cb));
    assert!(d.is_playing());
    assert_eq!(events.lock().unwrap().as_slice(), ["start 48000 100 2/2"]);

    // input 0 (Int24): a ramp; input 1 (Float32): another
    let in0: Vec<f32> = (0..100).map(|i| (i as f32 - 50.0) / 60.0).collect();
    let in1: Vec<f32> = (0..100).map(|i| (50.0 - i as f32) / 70.0).collect();
    ctl.set_input(0, 0, &encode(INT24, &in0));
    ctl.set_input(1, 0, &encode(FLOAT32, &in1));
    let probed = probe.output_ready_calls.load(SeqCst); // open() asked once whether the driver wants outputReady
    ctl.switch(0);
    assert_eq!(blocks.load(SeqCst), 1);
    assert_eq!(probe.output_ready_calls.load(SeqCst), probed + 1);
    // out 0 (Int16) = in0 * 0.5, out 2 (Float64, the second active output) = in1 * 0.5
    let out0 = decode(INT16, &ctl.output(0, 0));
    let out2 = decode(FLOAT64, &ctl.output(2, 0));
    let want0 = decode(
        INT16,
        &encode(
            INT16,
            &decode(INT24, &encode(INT24, &in0))
                .iter()
                .map(|v| v * 0.5)
                .collect::<Vec<_>>(),
        ),
    );
    assert_eq!(out0, want0);
    for i in 0..100 {
        assert!((out0[i] - in0[i] * 0.5).abs() < 1.0e-4);
        assert!((out2[i] - in1[i] * 0.5).abs() < 1.0e-6);
    }
    // the other half buffer is its own, and only the half asked for is written
    assert!(ctl.output(0, 1).iter().all(|b| *b == 0));
    ctl.set_input(0, 1, &encode(INT24, &in1));
    ctl.switch(1);
    assert!((decode(INT16, &ctl.output(0, 1))[0] - in1[0] * 0.5).abs() < 1.0e-4);
    assert_eq!(blocks.load(SeqCst), 2);
    // bufferSwitchTimeInfo is the same switch
    assert!(ctl.switch_time_info(0).is_null());
    assert_eq!(blocks.load(SeqCst), 3);
    // an index that is not 0 or 1 is ignored
    ctl.switch(2);
    ctl.switch(-1);
    assert_eq!(blocks.load(SeqCst), 3);

    d.close();
    assert!(!d.is_open() && !d.is_playing());
    assert_eq!(
        events.lock().unwrap().as_slice(),
        ["start 48000 100 2/2", "stopped"]
    );
    let log = probe.log();
    let n = log.len();
    assert_eq!(&log[n - 2..], ["stop", "disposeBuffers"]);
    d.close(); // twice is nothing
    assert_eq!(probe.count("disposeBuffers"), 1);
}

#[test]
fn a_dry_output_is_silence_and_blocks_stop_when_the_callback_is_taken() {
    let _s = serial();
    let (mut d, _, ctl) = device(Cfg::default());
    assert!(!d.start(Gain::new(1.0).0)); // not open yet: refused
    d.open(&[true, false], &[true, true, false], 0.0, 0)
        .unwrap();
    assert_eq!(d.current_buffer_size(), 128); // 0 = the preferred size
    assert_eq!(d.current_sample_rate(), 44100.0); // 0 = keep the driver's
    // no callback: whatever the driver had in the output buffer is replaced by silence
    ctl.set_output(0, 0, 0x55);
    ctl.set_output(1, 0, 0x55);
    ctl.switch(0);
    assert!(
        ctl.output(0, 0)
            .iter()
            .chain(ctl.output(1, 0).iter())
            .all(|b| *b == 0)
    );

    let (cb, events, blocks) = Gain::new(1.0);
    assert!(d.start(cb));
    ctl.switch(0);
    assert_eq!(blocks.load(SeqCst), 1);
    let cb = d.stop().expect("the callback comes back");
    assert!(!d.is_playing());
    ctl.switch(0);
    assert_eq!(blocks.load(SeqCst), 1);
    assert!(d.stop().is_none());
    // the same callback goes again, and a second start replaces (and stops) a running one
    assert!(d.start(cb));
    let (cb2, events2, _) = Gain::new(1.0);
    assert!(d.start(cb2));
    assert_eq!(
        events.lock().unwrap().as_slice(),
        [
            "start 44100 128 1/2",
            "stopped",
            "start 44100 128 1/2",
            "stopped"
        ]
    );
    assert_eq!(events2.lock().unwrap().as_slice(), ["start 44100 128 1/2"]);
}

#[test]
fn a_panicking_callback_is_contained_and_the_device_goes_silent() {
    struct Boom;
    impl AudioCallback for Boom {
        fn process(&mut self, io: &mut Block<'_>) {
            io.output(0).fill(0.9);
            panic!("test panic: a callback must not take the driver thread down");
        }
    }
    let _s = serial();
    let (mut d, _, ctl) = device(Cfg::default());
    d.open(&[false, false], &[true, false, false], 0.0, 0)
        .unwrap();
    assert!(d.start(Box::new(Boom)));
    ctl.switch(0); // the panic stops here, not in the driver
    assert!(d.faulted());
    assert!(
        ctl.output(0, 0).iter().all(|b| *b == 0),
        "what the callback wrote before it panicked must not be played"
    );
    ctl.switch(1);
    ctl.switch(0);
    assert!(ctl.output(0, 1).iter().all(|b| *b == 0));
    // a new callback gets a clean start
    let (cb, _, blocks) = Gain::new(1.0);
    assert!(d.start(cb));
    assert!(!d.faulted());
    ctl.switch(0);
    assert_eq!(blocks.load(SeqCst), 1);
}

#[test]
fn open_errors_say_what_the_cpp_said() {
    let _s = serial();
    // a rate the driver can't run
    let (mut d, probe, _) = device(Cfg::default());
    let e = d
        .open(&[true, false], &[true, false, false], 22050.0, 0)
        .unwrap_err();
    assert_eq!(e, OpenError::Rate(22050.0));
    assert_eq!(e.to_string(), "The driver can't run at 22050 Hz");
    assert_eq!(d.last_error(), Some(&e));
    assert_eq!(probe.count("createBuffers"), 0);
    // no channels switched on
    assert_eq!(
        d.open(&[false, false], &[false, false, false], 0.0, 0)
            .unwrap_err()
            .to_string(),
        "No channels are switched on"
    );
    assert_eq!(d.open(&[], &[], 0.0, 0).unwrap_err(), OpenError::NoChannels);
    assert!(!d.is_open());
    // a good open clears the error
    d.open(&[true], &[true], 0.0, 0).unwrap();
    assert!(d.last_error().is_none());
    drop(d);

    // the driver can't make its buffers
    let (mut d, _, _) = device(Cfg {
        create_ok: false,
        ..Cfg::default()
    });
    let e = d.open(&[true, false], &[], 0.0, 96).unwrap_err();
    assert_eq!(
        e.to_string(),
        "The driver couldn't make its buffers (96 samples)"
    );
    // ...and the host did not stay "active": another device can open
    let (mut other, _, _) = device(Cfg::default());
    other.open(&[true], &[], 0.0, 0).unwrap();
    drop(other);
    drop(d);

    // a sample type this host doesn't take (Float32MSB = 3): the buffers go back
    let (mut d, probe, _) = device(Cfg {
        inputs: vec![(3, "Mic")],
        ..Cfg::default()
    });
    let e = d.open(&[true], &[], 0.0, 0).unwrap_err();
    assert_eq!(
        e.to_string(),
        "Unsupported sample format from the driver (3)"
    );
    assert_eq!(probe.count("disposeBuffers"), 1);
    drop(d);

    // the driver doesn't start
    let (mut d, probe, _) = device(Cfg {
        start_ok: false,
        ..Cfg::default()
    });
    let e = d.open(&[true], &[true], 0.0, 0).unwrap_err();
    assert_eq!(e.to_string(), "The driver didn't start");
    assert_eq!(probe.count("disposeBuffers"), 1);
    assert!(!d.is_open());
}

#[test]
fn a_driver_that_will_not_init_reports_its_own_message() {
    let _s = serial();
    let (mut d, probe, _) = device(Cfg {
        init_ok: false,
        error_message: "Interface not found on USB",
        ..Cfg::default()
    });
    let e = d.open(&[true], &[true], 0.0, 0).unwrap_err();
    assert_eq!(e, OpenError::Load("Interface not found on USB".into()));
    assert_eq!(e.to_string(), "Interface not found on USB");
    assert_eq!(probe.released.load(SeqCst), 1); // released at once, not kept half-loaded
    assert!(d.input_channel_names().is_empty()); // and asking again does not retry or crash
    drop(d);
    assert_eq!(probe.released.load(SeqCst), 1);

    let (mut d, _, _) = device(Cfg {
        init_ok: false,
        ..Cfg::default()
    });
    assert_eq!(
        d.open(&[true], &[true], 0.0, 0).unwrap_err().to_string(),
        "The interface driver didn't start (is the interface plugged in?)"
    );
    assert_eq!(
        OpenError::Load(String::new()).to_string(),
        "The interface driver could not be loaded"
    );
    assert_eq!(
        OpenError::AnotherRunning.to_string(),
        "Another interface driver is already running"
    );
}

#[test]
fn only_one_driver_runs_at_a_time() {
    let _s = serial();
    let (mut a, _, _) = device(Cfg::default());
    let (mut b, _, _) = device(Cfg::default());
    a.open(&[true], &[true], 0.0, 0).unwrap();
    assert_eq!(
        b.open(&[true], &[true], 0.0, 0).unwrap_err(),
        OpenError::AnotherRunning
    );
    // the same device opening again is fine (open closes first)
    a.open(&[true], &[true], 0.0, 0).unwrap();
    a.close();
    b.open(&[true], &[true], 0.0, 0).unwrap();
}

#[test]
fn driver_messages_are_answered_like_the_cpp() {
    let _s = serial();
    let (mut d, _, ctl) = device(Cfg::default());
    d.open(&[true], &[true], 0.0, 0).unwrap();
    for supported in [
        K_ENGINE_VERSION,
        K_RESET_REQUEST,
        K_BUFFER_SIZE_CHANGE,
        K_RESYNC_REQUEST,
        K_LATENCIES_CHANGED,
    ] {
        assert_eq!(
            ctl.message(K_SELECTOR_SUPPORTED, supported),
            1,
            "selector {supported}"
        );
    }
    assert_eq!(ctl.message(K_SELECTOR_SUPPORTED, 7), 0); // kAsioSupportsTimeInfo etc. are not claimed
    assert_eq!(ctl.message(K_SELECTOR_SUPPORTED, 0), 0);
    assert_eq!(ctl.message(K_ENGINE_VERSION, 0), 2);
    assert_eq!(ctl.message(K_RESYNC_REQUEST, 0), 1);
    assert_eq!(ctl.message(K_LATENCIES_CHANGED, 0), 1);
    assert_eq!(ctl.message(42, 0), 0);
    assert!(!d.poll_restart()); // none of those asked for a reset
}

#[test]
fn a_reset_request_reopens_with_the_same_choice_and_the_same_callback() {
    let _s = serial();
    for how in 0..3 {
        let (mut d, probe, ctl) = device(Cfg::default());
        d.open(&[true, true], &[false, true, false], 0.0, 200)
            .unwrap();
        let (cb, events, blocks) = Gain::new(1.0);
        assert!(d.start(cb));
        assert!(!d.poll_restart());
        match how {
            0 => assert_eq!(ctl.message(K_RESET_REQUEST, 0), 1),
            1 => assert_eq!(ctl.message(K_BUFFER_SIZE_CHANGE, 0), 1),
            _ => ctl.rate_changed(),
        }
        assert_eq!(probe.count("createBuffers"), 1); // nothing happens on the driver's thread
        assert!(d.poll_restart());
        assert!(!d.poll_restart()); // asked once, done once
        assert_eq!(probe.count("createBuffers 3 200"), 2);
        assert_eq!(probe.count("disposeBuffers"), 1);
        assert!(d.is_open() && d.is_playing());
        assert_eq!(d.active_output_channels(), [false, true, false]);
        assert_eq!(
            events.lock().unwrap().as_slice(),
            ["start 44100 200 2/1", "stopped", "start 44100 200 2/1"]
        );
        ctl.switch(0);
        assert_eq!(blocks.load(SeqCst), 1);
    }
}

#[test]
fn start_and_stop_while_the_driver_thread_runs() {
    let _s = serial();
    let (mut d, _, ctl) = device(Cfg::default());
    d.open(&[true, true], &[true, true, true], 0.0, 0).unwrap();
    let run = Arc::new(AtomicBool::new(true));
    let switches = Arc::new(AtomicUsize::new(0));
    let t = {
        let (run, switches) = (run.clone(), switches.clone());
        std::thread::spawn(move || {
            let sw = ctl.switch_fn();
            let mut half = 0;
            while run.load(SeqCst) {
                sw(half, 1);
                half ^= 1;
                switches.fetch_add(1, SeqCst);
            }
        })
    };
    let mut ran = 0;
    for _ in 0..300 {
        let (cb, _, blocks) = Gain::new(1.0);
        assert!(d.start(cb));
        let before = switches.load(SeqCst);
        while switches.load(SeqCst) < before + 2 {
            std::thread::yield_now();
        }
        ran += blocks.load(SeqCst);
        d.stop();
    }
    run.store(false, SeqCst);
    t.join().unwrap();
    assert!(
        ran > 0,
        "the callback never ran while the driver thread did"
    );
}

#[test]
fn closing_waits_for_a_buffer_switch_that_is_running() {
    // close() clears the active device and waits for in-flight callbacks before the driver's buffers go
    struct Slow(Arc<AtomicBool>, Arc<AtomicBool>);
    impl AudioCallback for Slow {
        fn process(&mut self, _: &mut Block<'_>) {
            self.0.store(true, SeqCst);
            std::thread::sleep(std::time::Duration::from_millis(150));
            self.1.store(true, SeqCst);
        }
    }
    let _s = serial();
    let (mut d, probe, ctl) = device(Cfg::default());
    d.open(&[true], &[true], 0.0, 0).unwrap();
    let (entered, finished) = (
        Arc::new(AtomicBool::new(false)),
        Arc::new(AtomicBool::new(false)),
    );
    assert!(d.start(Box::new(Slow(entered.clone(), finished.clone()))));
    let t = std::thread::spawn(move || ctl.switch(0));
    while !entered.load(SeqCst) {
        std::thread::yield_now();
    }
    d.close(); // the callback is mid-block: close() must not get past it before it is done
    assert!(
        finished.load(SeqCst),
        "close returned while a callback was still running"
    );
    t.join().unwrap();
    assert_eq!(probe.count("disposeBuffers"), 1);
}

#[test]
fn drop_stops_the_callback_and_releases_the_driver_once() {
    let _s = serial();
    let (mut d, probe, _) = device(Cfg::default());
    d.open(&[true], &[true], 0.0, 0).unwrap();
    let (cb, events, _) = Gain::new(1.0);
    assert!(d.start(cb));
    drop(d);
    assert_eq!(
        events.lock().unwrap().last().map(String::as_str),
        Some("stopped")
    );
    assert_eq!(probe.count("disposeBuffers"), 1);
    assert_eq!(probe.released.load(SeqCst), 1);
}
