use hs_net::rtc::*;
use hs_net::wire::*;
use std::net::UdpSocket;
use std::time::{Duration, Instant};

fn config() -> RtcPeerConfig {
    // No external dependency: a closed local UDP port acts as an unavailable STUN service.
    RtcPeerConfig {
        ice_servers: vec![IceServerConfig {
            url: "stun:127.0.0.1:9".into(),
            username: String::new(),
            password: String::new(),
        }],
        ..RtcPeerConfig::default()
    }
}

fn forward(from: &mut RtcPeer, to: &mut RtcPeer, received: &mut Vec<Vec<u8>>) {
    if let Some(event) = from.poll(Duration::from_millis(1)).unwrap() {
        match event {
            RtcEvent::LocalDescription { kind, sdp } => {
                to.set_remote_description(kind, &sdp).unwrap()
            }
            RtcEvent::LocalCandidate { candidate, mid } => {
                to.add_remote_candidate(&candidate, &mid).unwrap()
            }
            RtcEvent::Message(data) => received.push(data),
            _ => {}
        }
    }
}

#[test]
fn rtc_peer_webrtc_datachannel_loopback_connectivity_and_transmission() {
    let mut a = RtcPeer::new(&config()).unwrap();
    let mut b = RtcPeer::new(&config()).unwrap();
    a.initiate_offer("audio").unwrap();
    let (mut a_received, mut b_received) = (Vec::new(), Vec::new());
    let deadline = Instant::now() + Duration::from_secs(10);
    while (!a.is_open() || !b.is_open()) && Instant::now() < deadline {
        forward(&mut a, &mut b, &mut a_received);
        forward(&mut b, &mut a, &mut b_received);
    }
    assert!(
        a.is_open() && b.is_open(),
        "{:?} {:?}",
        a.state(),
        b.state()
    );
    assert_eq!(a.state(), RtcState::Connected);
    let remote_addresses: Vec<_> = b.local_addresses().collect();
    let rogue = UdpSocket::bind("127.0.0.1:0").unwrap();
    let alert = [21, 0xfe, 0xfd, 0, 1, 0, 0, 0, 0, 0, 0, 0, 2, 2, 0];
    for address in a
        .local_addresses()
        .filter(|a| a.is_ipv4() && a.ip().is_loopback())
    {
        rogue.send_to(&alert, address).unwrap();
    }
    // An unrelated source cannot inject a DTLS close alert into the connected peer.
    a.poll(Duration::from_millis(10)).unwrap();
    let header = Hra3Header {
        magic: MAGIC3,
        codec: CODEC_OPUS,
        chans: 1,
        seq: 42,
        frames: 480,
        ..Hra3Header::default()
    };
    let mut packet = vec![0u8; HRA3_HEADER_BYTES + 4];
    write_hra3(&mut packet, &header, &[0xaa, 0xbb, 0xcc, 0xdd]).unwrap();
    assert!(a.send(&packet));
    assert!(b.send(&packet));
    assert!(!a.send(&[]));
    assert!(!a.send(&vec![0; 65537]));
    while (a_received.is_empty() || b_received.is_empty()) && Instant::now() < deadline {
        forward(&mut a, &mut b, &mut a_received);
        forward(&mut b, &mut a, &mut b_received);
    }
    assert_eq!(a_received, [packet.clone()]);
    assert_eq!(b_received, [packet]);
    a.close();
    a.close();
    assert!(!a.is_open());
    assert_eq!(a.state(), RtcState::Closed);
    assert!(!a.send(&[1]));
    let deadline = Instant::now() + Duration::from_secs(3);
    while b.is_open() && Instant::now() < deadline {
        b.poll(Duration::from_millis(2)).unwrap();
    }
    assert!(!b.is_open(), "remote close reached peer");
    for address in remote_addresses {
        assert!(
            UdpSocket::bind(address).is_ok(),
            "remote close released {address}"
        );
    }
}

#[test]
fn rtc_peer_invalid_signaling_and_lifecycle() {
    let mut peer = RtcPeer::new(&config()).unwrap();
    assert_eq!(peer.state(), RtcState::New);
    assert!(!peer.send(&[1]));
    assert!(peer.set_remote_description("offer", "garbage").is_err());
    assert!(peer.set_remote_description("unknown", "garbage").is_err());
    assert!(peer.set_remote_description("answer", "garbage").is_err());
    assert!(peer.add_remote_candidate("garbage", "0").is_err());
    assert_eq!(peer.state(), RtcState::New);
    peer.initiate_offer("friend").unwrap();
    assert!(peer.initiate_offer("friend").is_err());
    assert!(
        peer.add_remote_candidate(
            "candidate:1 1 UDP 2122260223 127.0.0.1 50000 typ host",
            "wrong"
        )
        .is_err()
    );
    peer.close();
    assert!(peer.initiate_offer("audio").is_err());
    assert!(peer.set_remote_description("offer", "garbage").is_err());
    assert!(peer.add_remote_candidate("garbage", "0").is_err());
}

#[test]
fn rtc_peer_binds_inside_firewall_range_and_releases_ports() {
    let cfg = RtcPeerConfig {
        port_range_begin: HUB_UDP_FIRST,
        port_range_end: HUB_UDP_LAST,
        ..config()
    };
    let mut peer = RtcPeer::new(&cfg).unwrap();
    let addresses: Vec<_> = peer.local_addresses().collect();
    assert!(!addresses.is_empty());
    assert!(
        addresses
            .iter()
            .all(|a| (HUB_UDP_FIRST..=HUB_UDP_LAST).contains(&a.port()))
    );
    peer.close();
    for address in addresses {
        assert!(UdpSocket::bind(address).is_ok(), "released {address}");
    }
    assert!(
        RtcPeer::new(&RtcPeerConfig {
            port_range_begin: 49000,
            port_range_end: 48000,
            ..config()
        })
        .is_err()
    );
    assert!(
        RtcPeer::new(&RtcPeerConfig {
            port_range_begin: 0,
            port_range_end: 48000,
            ..config()
        })
        .is_err()
    );
}

#[test]
fn rtc_peer_rejects_unsupported_ice_server_config_explicitly() {
    for url in [
        "turn:example.test:3478",
        "turns:example.test:5349",
        "stuns:example.test:5349",
        "junk",
        "stun:",
    ] {
        let result = RtcPeer::new(&RtcPeerConfig {
            ice_servers: vec![IceServerConfig {
                url: url.into(),
                username: "user".into(),
                password: "pass".into(),
            }],
            ..RtcPeerConfig::default()
        });
        assert!(result.is_err(), "{url}");
    }
}

#[test]
fn rtc_peer_stun_gathers_and_trickles_a_server_reflexive_candidate() {
    let server = UdpSocket::bind("127.0.0.1:0").unwrap();
    server.set_nonblocking(true).unwrap();
    let mut peer = RtcPeer::new(&RtcPeerConfig {
        ice_servers: vec![IceServerConfig {
            url: format!("stun:{}", server.local_addr().unwrap()),
            username: String::new(),
            password: String::new(),
        }],
        ..RtcPeerConfig::default()
    })
    .unwrap();
    peer.initiate_offer("audio").unwrap();
    let deadline = Instant::now() + Duration::from_secs(3);
    let mut found = false;
    let mut replay = None;
    while Instant::now() < deadline && !found {
        if let Some(RtcEvent::LocalCandidate { candidate, mid }) =
            peer.poll(Duration::from_millis(1)).unwrap()
            && candidate.contains("typ srflx")
        {
            assert!(candidate.contains("203.0.113.5 50001"));
            assert!(!mid.is_empty());
            found = true;
        }
        let mut buffer = [0u8; 2048];
        if let Ok((len, source)) = server.recv_from(&mut buffer) {
            assert!(len >= 20);
            buffer[..4].copy_from_slice(&[1, 1, 0, 12]);
            buffer[20..24].copy_from_slice(&[0, 0x20, 0, 8]);
            buffer[24..28].copy_from_slice(&[
                0,
                1,
                ((50001u16 ^ 0x2112) >> 8) as u8,
                (50001u16 ^ 0x2112) as u8,
            ]);
            buffer[28..32].copy_from_slice(&[203 ^ 0x21, 0x12, 113 ^ 0xa4, 5 ^ 0x42]);
            server.send_to(&buffer[..32], source).unwrap();
            replay = Some((<[u8; 32]>::try_from(&buffer[..32]).unwrap(), source));
        }
    }
    assert!(found);
    let (mut duplicate, source) = replay.unwrap();
    duplicate[31] = 9 ^ 0x42;
    server.send_to(&duplicate, source).unwrap();
    let deadline = Instant::now() + Duration::from_millis(30);
    while Instant::now() < deadline {
        if let Some(RtcEvent::LocalCandidate { candidate, .. }) =
            peer.poll(Duration::from_millis(1)).unwrap()
        {
            assert!(
                !candidate.contains("typ srflx"),
                "completed STUN transaction accepted replay"
            );
        }
    }
}

#[test]
fn rtc_peer_bounds_and_deduplicates_remote_candidates() {
    let mut peer = RtcPeer::new(&config()).unwrap();
    peer.initiate_offer("audio").unwrap();
    for n in 0..128 {
        let candidate = format!(
            "candidate:{n} 1 UDP 2122260223 127.0.0.1 {} typ host",
            50000 + n
        );
        peer.add_remote_candidate(&candidate, "").unwrap();
        peer.add_remote_candidate(&candidate, "").unwrap();
    }
    assert!(
        peer.add_remote_candidate(
            "candidate:129 1 UDP 2122260223 127.0.0.1 50129 typ host",
            ""
        )
        .is_err()
    );
    // Unsupported browser mDNS/TCP candidates are validated, then ignored like C++.
    peer.add_remote_candidate("candidate:1 1 UDP 2122260223 host.local 50000 typ host", "")
        .unwrap();
    peer.add_remote_candidate(
        "candidate:1 1 TCP 2122260223 127.0.0.1 9 typ host tcptype active",
        "",
    )
    .unwrap();
    let mut remote = RtcPeer::new(&config()).unwrap();
    let sdp = loop {
        if let Some(RtcEvent::LocalDescription { sdp, .. }) = peer.poll(Duration::ZERO).unwrap() {
            break sdp;
        }
    };
    let candidates: String = (0..129)
        .map(|n| {
            format!(
                "a=candidate:{n} 1 UDP 2122260223 127.0.0.1 {} typ host\r\n",
                50000 + n
            )
        })
        .collect();
    assert!(
        remote
            .set_remote_description("offer", &(sdp + &candidates))
            .is_err()
    );
    assert_eq!(remote.state(), RtcState::New);
}
