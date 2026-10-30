//! StreamConsumer behaviour beyond the golden happy path.
use hs_bus::shm::SharedMemory;
use hs_dsp::consumer::StreamConsumer;

#[test]
fn no_bus_gives_silence_and_reconnects_later() {
    let name = format!("consumer_none_{}", hs_bus::shm::current_pid());
    let mut c = StreamConsumer::new();
    c.set_report_to_hub(false);
    c.configure(&name, 0, 30.0, 48000);
    let (mut l, mut r) = (vec![1.0f32; 480], vec![1.0f32; 480]);
    c.pull([&mut l, &mut r], 480, 0.010);
    assert!(l.iter().chain(&r).all(|&x| x == 0.0));
    let st = c.status();
    assert!(!st.connected && !st.hub_alive && !st.streaming);

    // the bus appears, but no Hub owns it: still silent, but connected
    let shm = SharedMemory::open(&name).unwrap();
    c.pull([&mut l, &mut r], 480, 2.0); // reconnect timer elapsed
    let st = c.status();
    assert!(st.connected && !st.hub_alive && !st.streaming);
    assert!(l.iter().all(|&x| x == 0.0));
    drop(shm);
}

#[test]
fn odd_configuration_does_not_panic() {
    let mut c = StreamConsumer::new();
    // empty bus name -> "Main", output index and buffer length clamped
    c.configure("", 99, 1000.0, 44100);
    c.configure("", -5, 0.0, 96000); // adaptive buffer
    assert!(c.status().auto_buffer);
    let (mut l, mut r) = (vec![0.0f32; 16], vec![0.0f32; 16]);
    c.pull([&mut l, &mut r], 16, 0.0);
}

#[test]
fn zero_out_rate_and_short_output_buffers_do_not_panic() {
    let name = format!("consumer_zero_{}", hs_bus::shm::current_pid());
    let shm = SharedMemory::open(&name).unwrap();
    let l = shm.layout();
    l.header
        .hubOwnerToken
        .store(1, std::sync::atomic::Ordering::SeqCst);
    l.header.hubOwnerPid.store(
        hs_bus::shm::current_pid(),
        std::sync::atomic::Ordering::SeqCst,
    );
    l.header
        .hubHeartbeatNs
        .store(u64::MAX, std::sync::atomic::Ordering::SeqCst);
    l.streamHeader
        .sampleRate
        .store(48000, std::sync::atomic::Ordering::SeqCst);
    l.streamHeader
        .active
        .store(1, std::sync::atomic::Ordering::SeqCst);
    let mut c = StreamConsumer::new();
    c.set_report_to_hub(false);
    c.configure(&name, 0, 30.0, 0); // 0 Hz is replaced by 48 kHz
    let (mut a, mut b) = (vec![0.0f32; 8], vec![0.0f32; 8]);
    c.pull([&mut a, &mut b], 480, 0.010); // asks for more than the buffers hold
}
