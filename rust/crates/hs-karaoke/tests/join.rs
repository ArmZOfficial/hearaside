//! `JoinClient`: the port of `karaoke: JoinClient and ShareServer loopback in-process connection` from
//! `tests/unit/test_karaoke.cpp`, against the Rust share server on loopback. Where the C++ test only pushed and
//! pulled, this one also checks that the mic reached the Hub's bus and that the Hub's mix reached the headphones.
use hs_bus::ring::ring_write;
use hs_bus::shm::SharedMemory;
use hs_karaoke::join::{ConnectionState, join_client};
use hs_net::share::{CloudflaredSource, FriendLink, ShareServer};
use std::sync::Arc;
use std::sync::atomic::Ordering::SeqCst;
use std::time::{Duration, Instant};

fn wait_for(secs: u64, mut f: impl FnMut() -> bool) -> bool {
    let end = Instant::now() + Duration::from_secs(secs);
    while Instant::now() < end {
        if f() {
            return true;
        }
        std::thread::sleep(Duration::from_millis(10));
    }
    f()
}

#[test]
fn karaoke_join_client_and_share_server_loopback_in_process_connection() {
    let bus =
        Arc::new(SharedMemory::open(&format!("rs-karaoke-join-{}", std::process::id())).unwrap());
    let l = bus.layout();
    l.streamHeader.sampleRate.store(48000, SeqCst);
    l.header.hubSampleRate.store(48000, SeqCst);
    let token = ShareServer::new_token();
    let server = ShareServer::new();
    server.set_cloudflared_source(CloudflaredSource::Disabled);
    server.set_friend_links(vec![FriendLink {
        slot: 0,
        id: 101,
        token: token.clone(),
        name: "Singer".into(),
        host: "Host".into(),
    }]);
    assert!(server.start(
        bus.clone(),
        &ShareServer::new_token(),
        &ShareServer::new_token()
    ));
    let port = server.port();
    assert!(port > 0);

    let (mut client, mut audio) = join_client(48000.0, 256);
    client.connect(&format!("http://127.0.0.1:{port}/s/{token}"));
    assert!(
        wait_for(5, || client.is_live()),
        "live, state {:?}",
        client.state()
    );

    let info = client.room_info();
    assert_eq!((info.name.as_str(), info.host.as_str()), ("Singer", "Host"));
    assert_eq!(info.codec, "opus");
    assert_eq!(info.token, token);

    // the C++ came up in about the time the peers need; a loop that blocks on the socket took 6-7 s here
    let t = Instant::now();
    assert!(wait_for(3, || client.is_udp()), "DataChannel up");
    assert!(t.elapsed() < Duration::from_secs(3), "{:?}", t.elapsed());
    let info = client.room_info();
    assert!(info.is_udp);
    assert_eq!(info.transport, "WebRTC UDP");

    client.set_speaker_mode(true);
    assert!(client.is_speaker_mode());
    client.set_speaker_mode(false);

    // up: the mic reaches the Hub's bus as this friend's audio
    let f = &l.friends.0[0];
    let w0 = f.writePos.0.load(SeqCst);
    let tone: Vec<f32> = (0..480).map(|i| 0.25 * (i as f32 * 0.1).sin()).collect();
    for i in 0..20 {
        audio.push_upstream(&tone, 480, i * 480);
        std::thread::sleep(Duration::from_millis(10));
    }
    assert!(
        wait_for(3, || f.writePos.0.load(SeqCst) >= w0 + 2400),
        "mic frames at the Hub: {}",
        f.writePos.0.load(SeqCst) - w0
    );

    // down: the Hub's mix comes back through the jitter buffer
    let mix: Vec<f32> = (0..240).map(|i| 0.3 * (i as f32 * 0.07).sin()).collect();
    let (mut a, mut b) = (vec![0.0f32; 256], vec![0.0f32; 256]);
    let mut energy = 0.0f32;
    let end = Instant::now() + Duration::from_secs(5);
    while energy < 1.0 && Instant::now() < end {
        ring_write(&l.friendMixAudio.0, &l.friendMixWrite.0, &[&mix, &mix], 240);
        if audio.pull_downstream(&mut a, &mut b, 256) {
            energy += a.iter().map(|v| v * v).sum::<f32>();
        }
        std::thread::sleep(Duration::from_millis(5));
    }
    assert!(
        energy >= 1.0,
        "the room's mix reached the headphones ({energy})"
    );
    assert!(client.room_info().buffer_ms > 0.0);

    client.disconnect();
    assert!(!client.is_live());
    assert_eq!(client.state(), ConnectionState::Disconnected);
    assert!(wait_for(3, || server.friends_connected() == 0));
    server.stop();
}

#[test]
fn a_dead_address_keeps_reconnecting_and_disconnect_is_prompt() {
    let (mut client, _audio) = join_client(48000.0, 256);
    client.connect("http://127.0.0.1:9/s/nobody"); // the discard port: refused
    assert!(wait_for(3, || client.state() == ConnectionState::Reconnecting));
    let t = Instant::now();
    client.disconnect();
    assert!(t.elapsed() < Duration::from_secs(2), "{:?}", t.elapsed());
    assert_eq!(client.state(), ConnectionState::Disconnected);
}
