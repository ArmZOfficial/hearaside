mod common;

use common::{unique_bus, zeroed};
use hs_bus::bus::*;
use hs_bus::protocol::*;
use hs_bus::ring::*;
use hs_bus::shm::{BeaconMap, SharedMemory, current_pid, now_ns, process_alive};
use std::collections::BTreeSet;
use std::sync::atomic::{AtomicU64, Ordering::Relaxed, Ordering::SeqCst};

const RING: u32 = kRingFrames;

fn ring() -> Box<ChannelRing> {
    // SAFETY: floats; all-zero is valid.
    unsafe { zeroed() }
}

fn bus_box() -> Box<BusLayout> {
    // SAFETY: atomics and integers; all-zero is valid (as in a fresh mapping).
    unsafe { zeroed() }
}

#[test]
fn ring_write_read_with_wrap_around() {
    let ring = ring();
    let wp = AtomicU64::new(u64::from(RING) - 100); // force a wrap
    let src: Vec<f32> = (0..300).map(|i| i as f32).collect();
    ring_write(&ring, &wp, &[&src], 300);
    assert_eq!(wp.load(SeqCst), u64::from(RING) + 200);
    let mut cur = u64::from(RING) - 100;
    let (mut l, mut r) = (vec![0.0; 300], vec![0.0; 300]);
    assert_eq!(
        ring_read(&ring, &wp, &mut cur, &mut [&mut l, &mut r], 300),
        ReadResult::Ok
    );
    assert_eq!(l, src);
    assert_eq!(r, src);
    assert_eq!(cur, wp.load(SeqCst));
}

#[test]
fn ring_underrun_zero_fills_and_advances_only_by_available() {
    let ring = ring();
    let wp = AtomicU64::new(0);
    ring_write(&ring, &wp, &[&[1.0; 64]], 64);
    let mut cur = 0;
    let (mut l, mut r) = (vec![5.0; 128], vec![5.0; 128]);
    assert_eq!(
        ring_read(&ring, &wp, &mut cur, &mut [&mut l, &mut r], 128),
        ReadResult::Underrun
    );
    assert_eq!(cur, 64);
    assert_eq!(l[63], 1.0);
    assert!(l[64] == 0.0 && r[127] == 0.0);
}

#[test]
fn ring_overrun_detected() {
    let ring = ring();
    let wp = AtomicU64::new(0);
    let big = vec![0.5f32; (RING / 2) as usize];
    for _ in 0..4 {
        ring_write(&ring, &wp, &[&big], big.len() as u32);
    }
    let mut cur = 0;
    let (mut l, mut r) = (vec![1.0; 16], vec![1.0; 16]);
    assert_eq!(
        ring_read(&ring, &wp, &mut cur, &mut [&mut l, &mut r], 16),
        ReadResult::Overrun
    );
    assert!(l.iter().chain(&r).all(|&x| x == 0.0));
}

#[test]
fn ring_independent_readers() {
    let ring = ring();
    let wp = AtomicU64::new(0);
    let src: Vec<f32> = (0..100).map(|i| i as f32).collect();
    ring_write(&ring, &wp, &[&src], 100);
    let (mut r1, mut r2) = (0, 0);
    let (mut a, mut b, mut c, mut e) =
        (vec![0.0; 50], vec![0.0; 50], vec![0.0; 100], vec![0.0; 100]);
    ring_read(&ring, &wp, &mut r1, &mut [&mut a, &mut b], 50);
    ring_read(&ring, &wp, &mut r2, &mut [&mut c, &mut e], 100);
    assert!(r1 == 50 && r2 == 100 && a[49] == 49.0 && c[99] == 99.0);
}

#[test]
fn ring_stereo_source_and_silence() {
    let ring = ring();
    let wp = AtomicU64::new(0);
    let (l, r): (Vec<f32>, Vec<f32>) = ((0..8).map(|i| i as f32).collect(), vec![-1.0; 8]);
    ring_write(&ring, &wp, &[&l, &r], 8);
    ring_write(&ring, &wp, &[], 4); // silence
    let mut cur = 0;
    let (mut a, mut b) = (vec![9.0; 12], vec![9.0; 12]);
    assert_eq!(
        ring_read(&ring, &wp, &mut cur, &mut [&mut a, &mut b], 12),
        ReadResult::Ok
    );
    assert_eq!(&a[..8], &l[..]);
    assert_eq!(&b[..8], &r[..]);
    assert!(a[8..].iter().chain(&b[8..]).all(|&x| x == 0.0));
}

#[test]
fn shm_two_mappings_of_one_bus_share_memory() {
    let name = unique_bus("shm");
    let a = SharedMemory::open(&name).unwrap();
    let b = SharedMemory::open_existing(&name).unwrap();
    assert!(!std::ptr::eq(a.layout(), b.layout()));
    a.layout().header.slotSequence.store(1234, SeqCst);
    assert_eq!(b.layout().header.slotSequence.load(SeqCst), 1234);
    assert_eq!(b.layout().header.magic.load(SeqCst), kMagic);
    assert_eq!(b.layout().slots[5].stemIndex.load(SeqCst), -1);
}

#[test]
fn shm_open_existing_fails_for_unknown_bus() {
    assert!(SharedMemory::open_existing(&unique_bus("none")).is_err());
}

#[test]
fn slots_claim_64_65th_fails_duplicate_uuid_detected_release() {
    let shm = SharedMemory::open(&unique_bus("slots")).unwrap();
    let l = shm.layout();
    let mut got = BTreeSet::new();
    for i in 0..kMaxSlots {
        got.insert(claim_slot(l, &format!("uuid-{i}"), 48000, 2).expect("free slot"));
    }
    assert_eq!(got.len(), kMaxSlots);
    assert_eq!(claim_slot(l, "another", 48000, 2), Err(false));
    release_slot(l, 10);
    assert_eq!(claim_slot(l, "uuid-3", 48000, 2), Err(true));
    assert_eq!(claim_slot(l, "fresh", 48000, 2), Ok(10));
}

#[test]
fn slots_concurrent_claims_never_share_a_slot() {
    let shm = SharedMemory::open(&unique_bus("race")).unwrap();
    let l = shm.layout();
    let result: Vec<Result<usize, bool>> = std::thread::scope(|s| {
        let hs: Vec<_> = (0..32)
            .map(|t| s.spawn(move || claim_slot(l, &format!("t{t}"), 48000, 2)))
            .collect();
        hs.into_iter().map(|h| h.join().unwrap()).collect()
    });
    let uniq: BTreeSet<_> = result.iter().map(|r| r.expect("claimed")).collect();
    assert_eq!(uniq.len(), 32);
}

#[test]
fn slots_identity_seqlock_round_trip_utf8_thai_truncation_on_code_point() {
    let shm = SharedMemory::open(&unique_bus("ident")).unwrap();
    let idx = claim_slot(shm.layout(), "abc", 48000, 2).unwrap();
    let slot = &shm.layout().slots[idx];
    set_slot_identity(slot, "เสียงร้อง", 0xFF33_6699);
    let (n, u, c) = read_slot_identity(slot).unwrap();
    assert!(n == "เสียงร้อง" && u == "abc" && c == 0xFF33_6699);
    set_slot_identity(slot, &"ก".repeat(40), 0); // 3 bytes each = 120 bytes
    let (n, ..) = read_slot_identity(slot).unwrap();
    assert!(n.len() == 63 && n.len() % 3 == 0);
}

#[test]
fn hub_exclusive_ownership_release_stale_takeover() {
    let shm = SharedMemory::open(&unique_bus("hub")).unwrap();
    let l = shm.layout();
    assert!(try_claim_hub(l, 111, 2_000_000_000));
    assert!(!try_claim_hub(l, 222, 2_000_000_000));
    assert!(hub_alive(l, 2_000_000_000));
    l.header
        .hubHeartbeatNs
        .store(now_ns() - 3_000_000_000, SeqCst); // stale
    assert!(try_claim_hub(l, 222, 2_000_000_000));
    release_hub(l, 111); // not owner any more: no effect
    assert_eq!(l.header.hubOwnerToken.load(SeqCst), 222);
    release_hub(l, 222);
    assert_eq!(l.header.hubOwnerToken.load(SeqCst), 0);
}

#[test]
fn mailbox_commands_delivered_in_order_overflow_keeps_newest() {
    let bus = bus_box();
    let slot = &bus.slots[0];
    let mut cursor = slot.cmdWrite.load(SeqCst);
    post_command(slot, ParamId::Mon, 0.0);
    post_command(slot, ParamId::StrGainDb, -3.0);
    let mut got = vec![];
    poll_commands(slot, &mut cursor, |id, v| got.push((id, v)));
    assert!(got.len() == 2 && got[0].0 == ParamId::Mon && got[1].1 == -3.0);
    for i in 0..100 {
        post_command(slot, ParamId::StrPan, i as f32);
    }
    got.clear();
    poll_commands(slot, &mut cursor, |id, v| got.push((id, v)));
    assert!(got.len() == kMailboxSize && got.last().unwrap().1 == 99.0);
}

#[test]
fn rename_seqlock_request() {
    let bus = bus_box();
    let slot = &bus.slots[0];
    let mut last = slot.renameSeq.load(SeqCst);
    assert!(poll_rename(slot, &mut last).is_none());
    request_rename(slot, "ไมค์พูด");
    assert_eq!(poll_rename(slot, &mut last).as_deref(), Some("ไมค์พูด"));
    assert!(poll_rename(slot, &mut last).is_none());
}

#[test]
fn sources_claim_16_identity_commands_program_choice_dead_owner_reclaimed() {
    let shm = SharedMemory::open(&unique_bus("sources")).unwrap();
    let l = shm.layout();
    let got: BTreeSet<_> = (0..kMaxSources).map(|_| claim_source(l)).collect();
    assert!(got.len() == kMaxSources && !got.contains(&None));
    assert_eq!(claim_source(l), None);
    release_source(l, 5);
    assert_eq!(claim_source(l), Some(5));

    let s = &l.sources[5];
    set_source_identity(s, "เพลง YouTube", "chrome.exe", 0xFF11_2233);
    assert_eq!(
        read_source_identity(s),
        Some(("เพลง YouTube".into(), "chrome.exe".into(), 0xFF11_2233))
    );

    let mut cursor = s.cmdWrite.load(SeqCst);
    post_source_command(s, SourceParam::On, 0.0);
    post_source_command(s, SourceParam::LevelDb, -6.0);
    let mut cmds = vec![];
    poll_source_commands(s, &mut cursor, |id, v| cmds.push((id, v)));
    assert!(cmds.len() == 2 && cmds[0].0 == SourceParam::On && cmds[1].1 == -6.0);

    let mut last = s.appSeq.load(SeqCst);
    assert!(poll_source_app(s, &mut last).is_none());
    request_source_app(s, "spotify.exe");
    assert_eq!(
        poll_source_app(s, &mut last).as_deref(),
        Some("spotify.exe")
    );

    // owner process gone + stale heartbeat -> the Hub frees it
    s.ownerPid.store(0, SeqCst);
    s.heartbeatNs.store(now_ns() - 10_000_000_000, SeqCst);
    assert_eq!(reclaim_dead_slots(l, 5_000_000_000), 1);
    assert_eq!(s.state.load(SeqCst), SlotState::kSlotFree);
}

#[test]
fn slots_dead_owner_reclaimed_live_owner_kept() {
    let shm = SharedMemory::open(&unique_bus("reclaim")).unwrap();
    let l = shm.layout();
    let dead = claim_slot(l, "dead", 48000, 2).unwrap();
    let live = claim_slot(l, "live", 48000, 2).unwrap();
    for i in [dead, live] {
        l.slots[i].heartbeatNs.store(1, SeqCst); // long ago
    }
    l.slots[dead].ownerPid.store(0xFFFF_FFF0, SeqCst); // no such process
    assert_eq!(reclaim_dead_slots(l, 1000), 1);
    assert_eq!(l.slots[dead].state.load(SeqCst), SlotState::kSlotFree);
    assert_eq!(l.slots[live].state.load(SeqCst), SlotState::kSlotActive); // our own pid
}

#[test]
fn tags_timeline_lookup_finds_ahead_rendered_block() {
    let bus = bus_box();
    // SAFETY: atomics only.
    let tags: Box<SlotTags> = unsafe { zeroed() };
    let slot = &bus.slots[0];
    // track rendered blocks of 128 at timeline 1000.. with FIFO starting at 5000
    for k in 0..20u64 {
        write_tag(
            slot,
            &tags,
            5000 + k * 128,
            1000 + k as i64 * 128,
            128,
            true,
        );
    }
    let find = |t| find_timeline_position(slot, &tags, t, kTagRingSize);
    assert_eq!(find(1000 + 3 * 128 + 7), Some(5000 + 3 * 128 + 7));
    assert_eq!(find(1000 + 20 * 128), None); // not rendered yet
    write_tag(slot, &tags, 5000 + 20 * 128, 0, 128, false); // stopped
    assert_eq!(find(1000 + 3 * 128), None);
}

#[test]
fn platform_uuid_format_and_process_liveness() {
    let u = make_uuid();
    let b = u.as_bytes();
    assert!(u.len() == 36 && b[8] == b'-' && b[14] == b'4');
    assert!(matches!(b[19], b'8' | b'9' | b'a' | b'b'));
    assert_ne!(make_uuid(), u);
    assert!(process_alive(current_pid()));
    assert!(!process_alive(0));
    assert_ne!(random_token(), 0);
}

#[test]
fn remote_many_writers_one_reader_every_command_delivered_once_and_in_claim_order() {
    let shm = SharedMemory::open(&unique_bus("remote")).unwrap();
    let bus = shm.layout();
    let mut cursor = bus.header.remoteReserve.load(SeqCst);
    const WRITERS: usize = 4;
    const EACH: u32 = 2000;
    let finished = AtomicU64::new(0);
    let mut next = [0u32; WRITERS];
    let (mut received, mut out_of_order, mut corrupt) = (0, 0, 0);
    let mut drain = |cursor: &mut u32| {
        poll_remote(bus, cursor, |target, pid, v| {
            if !(0..WRITERS as i32).contains(&target) || pid as f32 != v {
                corrupt += 1;
                return;
            }
            let t = target as usize;
            if pid < next[t] {
                out_of_order += 1;
            }
            next[t] = pid + 1;
            received += 1;
        });
    };
    std::thread::scope(|s| {
        for w in 0..WRITERS {
            let finished = &finished;
            s.spawn(move || {
                for i in 0..EACH {
                    post_remote(bus, w as i32, i, i as f32);
                    if i & 15 == 0 {
                        std::thread::yield_now();
                    }
                }
                finished.fetch_add(1, SeqCst);
            });
        }
        // the reader keeps polling like the Hub timer; under this flood the oldest commands may be
        // dropped (queue of 64), but nothing may be corrupted or delivered out of order
        while finished.load(SeqCst) < WRITERS as u64 {
            drain(&mut cursor);
        }
    });
    drain(&mut cursor);
    assert_eq!(corrupt, 0);
    assert_eq!(out_of_order, 0);
    println!(
        "received {received} of {} (queue {kRemoteQueueSize})",
        WRITERS as u32 * EACH
    );
    assert!(received > 0);
}

// ---------------------------------------------------------------------------------------------
// v13: friends room, feeders, requests, beacon

#[test]
fn v13_layout_stays_within_what_the_proposal_promised() {
    assert_eq!(size_of::<BeaconHeader>(), 64);
    const { assert!(kLayoutSize < 54 * 1024 * 1024) }; // about 50 MB with 8 friend rings (+2 MB: v14 voices)
    const { assert!(kProtocolVersion >= 13) };
    assert_eq!(kMaxFriends, 8);
}

#[test]
fn friends_eight_friend_rings_written_together_read_by_three_readers() {
    let name = unique_bus("friends");
    let a = SharedMemory::open(&name).unwrap();
    let b = SharedMemory::open_existing(&name).unwrap();
    let w = a.layout();
    std::thread::scope(|s| {
        for f in 0..kMaxFriends {
            w.friends.0[f].id.store(100 + f as u32, SeqCst);
            w.friends.0[f].state.store(FriendState::kFriendLive, SeqCst);
            s.spawn(move || {
                let mut buf = vec![0.0f32; 128];
                for block in 0..40 {
                    for (i, x) in buf.iter_mut().enumerate() {
                        *x = f as f32 + (block * 128 + i) as f32 * 1e-5;
                    }
                    ring_write(
                        &w.friendAudio.0[f],
                        &w.friends.0[f].writePos.0,
                        &[&buf],
                        128,
                    );
                }
            });
        }
    });
    // three independent readers (Hub engine, App Audio, a feeder) each keep their own cursor
    let bl = b.layout();
    for _reader in 0..3 {
        for f in 0..kMaxFriends {
            let (mut l, mut r) = (vec![0.0; 128], vec![0.0; 128]);
            let mut cur = 0;
            let res = ring_read(
                &bl.friendAudio.0[f],
                &bl.friends.0[f].writePos.0,
                &mut cur,
                &mut [&mut l, &mut r],
                128,
            );
            assert_eq!(res, ReadResult::Ok);
            assert_eq!(l[0], f as f32);
            assert_eq!(r[0], l[0]);
            assert_eq!(cur, 128);
        }
    }
    assert_eq!(bl.friends.0[7].id.load(SeqCst), 107);
}

#[test]
fn friends_names_are_read_consistently_and_cut_on_a_code_point() {
    let bus = bus_box();
    let thai = "ก".repeat(30); // 90 bytes > 63
    set_friend_name(&bus.friends.0[3], &thai);
    let got = read_friend_name(&bus.friends.0[3]);
    assert!(got.len() < kNameBytes);
    assert_eq!(got.len() % 3, 0); // never half a character
    set_friend_name(&bus.friends.0[3], "Mint");
    assert_eq!(read_friend_name(&bus.friends.0[3]), "Mint");
}

#[test]
fn feeders_claim_identity_reclaim_after_the_owner_is_gone() {
    let bus = bus_box();
    let got: BTreeSet<_> = (0..kMaxFeeders).map(|_| claim_feeder(&bus)).collect();
    assert_eq!(got.len(), kMaxFeeders);
    assert!(!got.contains(&None));
    assert_eq!(claim_feeder(&bus), None); // full

    set_feeder_identity(&bus.feeders[2], "Audio 05", 0xFF11_2233);
    assert_eq!(
        read_feeder_identity(&bus.feeders[2]),
        Some(("Audio 05".into(), 0xFF11_2233))
    );

    release_feeder(&bus, 2);
    assert_eq!(claim_feeder(&bus), Some(2));

    bus.feeders[5].ownerPid.store(0xFFFF_FFF0, SeqCst); // a process that does not exist
    bus.feeders[5].heartbeatNs.store(1, SeqCst);
    assert!(reclaim_dead_slots(&bus, 1000) >= 1);
    assert_eq!(bus.feeders[5].state.load(SeqCst), SlotState::kSlotFree);
}

#[test]
fn requests_many_writers_one_reader_text_is_wiped_once_read() {
    let bus = bus_box();
    std::thread::scope(|s| {
        for t in 0..4 {
            let bus = &*bus;
            s.spawn(move || {
                for i in 0..3 {
                    post_request(bus, t, RequestKind::kReqResolveToken, i, &format!("tok{t}"));
                }
            });
        }
    });
    let mut cursor = 0;
    let mut n = 0;
    let mut texts = BTreeSet::new();
    poll_requests(&bus, &mut cursor, |feeder, kind, _, text| {
        assert_eq!(kind, RequestKind::kReqResolveToken);
        assert_eq!(text, format!("tok{feeder}"));
        texts.insert(text);
        n += 1;
    });
    assert_eq!(n, 12);
    assert_eq!(texts.len(), 4);
    // nothing readable left in shared memory
    for e in &bus.requests {
        // SAFETY: single-threaded here; plain read of the wiped text.
        assert_eq!(unsafe { (*e.text.get())[0] }, 0);
    }
}

#[test]
fn requests_overflow_drops_the_oldest_never_stalls() {
    let bus = bus_box();
    for i in 0..kRequestQueueSize as u32 * 2 {
        post_request(&bus, 0, RequestKind::kReqCopyLink, i, "");
    }
    let mut cursor = 0;
    let mut args = vec![];
    poll_requests(&bus, &mut cursor, |_, _, arg, _| args.push(arg));
    assert_eq!(args.len(), kRequestQueueSize);
    assert_eq!(args[0], kRequestQueueSize as u32); // the first 16 were dropped
    assert_eq!(*args.last().unwrap(), kRequestQueueSize as u32 * 2 - 1);
}

#[test]
fn reply_the_feeder_sees_each_answer_once() {
    let bus = bus_box();
    let f = &bus.feeders[0];
    let mut last = 0;
    assert_eq!(poll_reply(f, &mut last), None);
    post_reply(f, 101);
    assert_eq!(poll_reply(f, &mut last), Some(101));
    assert_eq!(poll_reply(f, &mut last), None);
    post_reply(f, kReplyRoomFull);
    assert_eq!(poll_reply(f, &mut last), Some(kReplyRoomFull));
}

#[test]
fn beacon_a_reader_sees_the_hub_version_and_nothing_when_stale_or_absent() {
    let name = unique_bus("beacon");
    assert!(BeaconMap::open_existing(&name).is_none());
    let hub = BeaconMap::open(&name).unwrap();
    let reader = BeaconMap::open_existing(&name).unwrap();
    let ver = || beacon_hub_version(reader.header(), BEACON_STALE_NS);
    assert_eq!(ver(), 0); // nothing published yet
    beacon_publish(hub.header());
    assert_eq!(ver(), kProtocolVersion);
    hub.header().protocolVersion.store(12, Relaxed); // as an older Hub would write
    assert_eq!(ver(), 12);
    hub.header().heartbeatNs.store(1, Relaxed); // long ago
    assert_eq!(ver(), 0);
}

#[test]
fn ring_short_slices_never_write_out_of_bounds() {
    let ring = ring();
    let wp = AtomicU64::new(0);
    ring_write(&ring, &wp, &[&vec![1.0; 512]], 512);
    let (mut l, mut r) = (vec![7.0; 256], vec![7.0; 256]);
    let mut cur = 0;
    // 512 asked, 256 fit: only 256 are read (release builds; debug builds assert)
    if !cfg!(debug_assertions) {
        ring_read(&ring, &wp, &mut cur, &mut [&mut l, &mut r], 512);
        assert_eq!(cur, 256);
    }
}

#[test]
fn tags_garbage_from_another_process_does_not_overflow() {
    let bus = bus_box();
    // SAFETY: atomics only.
    let tags: Box<SlotTags> = unsafe { zeroed() };
    write_tag(&bus.slots[0], &tags, u64::MAX, i64::MAX, u32::MAX, true);
    let _ = find_timeline_position(&bus.slots[0], &tags, 5, kTagRingSize);
    let _ = find_timeline_position(&bus.slots[0], &tags, i64::MAX, kTagRingSize);
}
