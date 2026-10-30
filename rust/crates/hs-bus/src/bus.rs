//! Slot registry, Hub ownership, mailboxes, remote queue, requests, beacon and timeline tags.
//! Port of `libs/ssbus/src/bus.cpp` and `bus.h`; every protocol step (orderings, seqlocks, claim
//! sequence) is kept as in C++ because C++ and Rust processes use the same memory at the same time.
//! Audio-thread safe (no allocation, no locks): `write_tag`, `find_timeline_position`, `post_command`,
//! `poll_commands`. The rest is for message threads.
use crate::protocol::*;
use crate::shm::{current_pid, now_ns, process_alive};
use core::ptr;
use core::sync::atomic::{AtomicU32, AtomicU64, Ordering, fence};
use std::hash::{BuildHasher, Hasher, RandomState};

use Ordering::{AcqRel, Acquire, Relaxed, Release};

pub fn float_bits(f: f32) -> u32 {
    f.to_bits()
}

pub fn bits_float(u: u32) -> f32 {
    f32::from_bits(u)
}

fn rand64() -> u64 {
    let mut h = RandomState::new().build_hasher();
    h.write_u64(now_ns());
    h.finish()
}

/// Non-zero random 64-bit value.
pub fn random_token() -> u64 {
    loop {
        let t = rand64();
        if t != 0 {
            return t;
        }
    }
}

/// 36 characters, RFC 4122 v4 style.
pub fn make_uuid() -> String {
    let a = (rand64() & 0xFFFF_FFFF_FFFF_0FFF) | 0x4000;
    let b = (rand64() & 0x3FFF_FFFF_FFFF_FFFF) | 0x8000_0000_0000_0000;
    format!(
        "{:08x}-{:04x}-{:04x}-{:04x}-{:012x}",
        (a >> 32) as u32,
        (a >> 16) & 0xFFFF,
        a & 0xFFFF,
        b >> 48,
        b & 0xFFFF_FFFF_FFFF
    )
}

// ---------------------------------------------------------------------------------------------
// shared-field helpers

fn put<T>(f: &Shared<T>, v: T) {
    // SAFETY: `get` points into the live mapping; the seqlock / single-writer protocol of the caller
    // makes this the only writer.
    unsafe { f.get().write_volatile(v) }
}

fn take<T: Copy>(f: &Shared<T>) -> T {
    // SAFETY: as above; a torn read is detected by the caller's seqlock.
    unsafe { f.get().read_volatile() }
}

/// UTF-8 name into a fixed NUL-terminated field, cut on a code point boundary (never half a Thai letter).
fn copy_name<const N: usize>(dst: &Shared<[u8; N]>, src: &str) {
    let b = src.as_bytes();
    let mut n = b.len().min(N - 1);
    while n > 0 && n < b.len() && (b[n] & 0xC0) == 0x80 {
        n -= 1;
    }
    let mut tmp = [0u8; N];
    tmp[..n].copy_from_slice(&b[..n]);
    put(dst, tmp);
}

fn clear_name<const N: usize>(dst: &Shared<[u8; N]>) {
    put(dst, [0u8; N]);
}

fn cstring(src: &[u8]) -> String {
    let n = src.iter().position(|&c| c == 0).unwrap_or(src.len());
    String::from_utf8_lossy(&src[..n]).into_owned()
}

/// Single writer seqlock write: odd while writing, even when stable.
fn seq_write(seq: &AtomicU32, body: impl FnOnce()) {
    seq.fetch_add(1, Relaxed);
    fence(Release);
    body();
    seq.fetch_add(1, Release);
}

/// Reader side of that seqlock: up to 8 attempts, `None` when a writer kept interfering.
fn seq_read<R>(seq: &AtomicU32, body: impl Fn() -> R) -> Option<R> {
    for _ in 0..8 {
        let s1 = seq.load(Acquire);
        if s1 & 1 != 0 {
            continue;
        }
        let r = body();
        fence(Acquire);
        if seq.load(Relaxed) != s1 {
            continue;
        }
        return Some(r);
    }
    None
}

/// Single-writer text request (rename, program choice): seq is even when stable, +2 per request.
fn request_text(seq: &AtomicU32, buf: &Shared<[u8; kNameBytes]>, text: &str) {
    let s = seq.load(Relaxed) & !1;
    seq.store(s + 1, Relaxed);
    fence(Release);
    copy_name(buf, text);
    seq.store(s + 2, Release);
}

fn poll_text(
    seq: &AtomicU32,
    buf: &Shared<[u8; kNameBytes]>,
    last_seq: &mut u32,
) -> Option<String> {
    let s1 = seq.load(Acquire);
    if s1 & 1 != 0 || s1 == *last_seq {
        return None;
    }
    let tmp = take(buf);
    fence(Acquire);
    if seq.load(Relaxed) != s1 {
        return None;
    }
    *last_seq = s1;
    Some(cstring(&tmp))
}

fn dead_owner(heartbeat: &AtomicU64, pid: &AtomicU32, now: u64, stale_ns: u64) -> bool {
    let hb = heartbeat.load(Relaxed);
    now > hb && now - hb > stale_ns && !process_alive(pid.load(Relaxed))
}

// ---------------------------------------------------------------------------------------------
// slots (Track side)

pub fn uuid_in_use(bus: &BusLayout, uuid: &str, ignore_index: Option<usize>) -> bool {
    bus.slots.iter().enumerate().any(|(i, s)| {
        Some(i) != ignore_index
            && s.state.load(Acquire) == SlotState::kSlotActive
            && read_slot_identity(s).is_some_and(|(_, id, _)| id == uuid)
    })
}

/// Claims a free slot. `Err(true)`: another active slot already uses `uuid` (pick a new one);
/// `Err(false)`: all 64 slots are in use. Message thread only.
pub fn claim_slot(
    bus: &BusLayout,
    uuid: &str,
    sample_rate: u32,
    num_channels: u32,
) -> Result<usize, bool> {
    if uuid_in_use(bus, uuid, None) {
        return Err(true);
    }
    for (i, s) in bus.slots.iter().enumerate() {
        if s.state
            .compare_exchange(
                SlotState::kSlotFree,
                SlotState::kSlotClaiming,
                AcqRel,
                Relaxed,
            )
            .is_err()
        {
            continue;
        }
        let now = now_ns();
        s.ownerPid.store(current_pid(), Relaxed);
        s.sequence
            .store(bus.header.slotSequence.fetch_add(1, Relaxed) + 1, Relaxed);

        seq_write(&s.nameSeq, || {
            copy_name(&s.uuid, uuid);
            clear_name(&s.name);
            put(&s.colorARGB, 0);
        });

        s.sampleRate.store(sample_rate, Relaxed);
        s.numChannels.store(num_channels, Relaxed);
        s.heartbeatNs.store(now, Relaxed);
        s.claimNs.store(now, Relaxed);
        s.flags
            .store(SlotFlags::kFlagMon | SlotFlags::kFlagStr, Relaxed);
        for bits in [
            &s.chainLatencyBits,
            &s.strGainBits,
            &s.strPanBits,
            &s.strDelayBits,
            &s.monTrimBits,
            &s.fxLatencyBits,
        ] {
            bits.store(float_bits(0.0), Relaxed);
        }
        s.stemIndex.store(-1, Relaxed);
        for c in 0..kMaxChannels {
            s.peakInBits[c].store(0, Relaxed);
            s.peakStreamBits[c].store(0, Relaxed);
        }
        s.hubStatus.store(SlotHubStatus::kHubStatusNone, Relaxed);
        s.fedBy.store(0, Relaxed);
        s.hubLeadFrames.store(0, Relaxed);
        s.hubMute.store(0, Relaxed);
        s.epoch.fetch_add(1, Relaxed);
        s.state.store(SlotState::kSlotActive, Release);
        return Ok(i);
    }
    Err(false)
}

pub fn release_slot(bus: &BusLayout, index: usize) {
    let Some(s) = bus.slots.get(index) else {
        return;
    };
    s.flags.store(0, Relaxed);
    s.epoch.fetch_add(1, Relaxed);
    s.state.store(SlotState::kSlotFree, Release);
}

pub fn set_slot_identity(slot: &SlotHeader, name: &str, color_argb: u32) {
    seq_write(&slot.nameSeq, || {
        copy_name(&slot.name, name);
        put(&slot.colorARGB, color_argb);
    });
}

/// `(name, uuid, colorARGB)` copied consistently; `None` if a writer kept interfering.
pub fn read_slot_identity(slot: &SlotHeader) -> Option<(String, String, u32)> {
    seq_read(&slot.nameSeq, || {
        (take(&slot.name), take(&slot.uuid), take(&slot.colorARGB))
    })
    .map(|(n, u, c)| (cstring(&n), cstring(&u), c))
}

/// Hub side: frees slots, App Audio sources and feeders whose owner process died.
pub fn reclaim_dead_slots(bus: &BusLayout, stale_ns: u64) -> usize {
    let now = now_ns();
    let mut count = 0;
    for s in &bus.slots {
        let st = s.state.load(Acquire);
        if st == SlotState::kSlotFree {
            continue;
        }
        if dead_owner(&s.heartbeatNs, &s.ownerPid, now, stale_ns)
            && s.state
                .compare_exchange(st, SlotState::kSlotFree, AcqRel, Relaxed)
                .is_ok()
        {
            s.flags.store(0, Relaxed);
            s.epoch.fetch_add(1, Relaxed);
            count += 1;
        }
    }
    for s in &bus.sources {
        let st = s.state.load(Acquire);
        if st != SlotState::kSlotFree
            && dead_owner(&s.heartbeatNs, &s.ownerPid, now, stale_ns)
            && s.state
                .compare_exchange(st, SlotState::kSlotFree, AcqRel, Relaxed)
                .is_ok()
        {
            count += 1;
        }
    }
    for f in &bus.feeders {
        let st = f.state.load(Acquire);
        if st != SlotState::kSlotFree
            && dead_owner(&f.heartbeatNs, &f.ownerPid, now, stale_ns)
            && f.state
                .compare_exchange(st, SlotState::kSlotFree, AcqRel, Relaxed)
                .is_ok()
        {
            count += 1;
        }
    }
    count
}

// ---------------------------------------------------------------------------------------------
// Hub ownership

pub fn hub_alive(bus: &BusLayout, stale_ns: u64) -> bool {
    let h = &bus.header;
    if h.hubOwnerToken.load(Acquire) == 0 {
        return false;
    }
    let hb = h.hubHeartbeatNs.load(Relaxed);
    let now = now_ns();
    if now > hb && now - hb > stale_ns {
        return false;
    }
    process_alive(h.hubOwnerPid.load(Relaxed))
}

/// Tries to become the bus' Hub: succeeds if no Hub owns it, or the owner is stale.
pub fn try_claim_hub(bus: &BusLayout, token: u64, stale_ns: u64) -> bool {
    let h = &bus.header;
    let cur = h.hubOwnerToken.load(Acquire);
    if cur == token {
        return true;
    }
    if cur != 0 && hub_alive(bus, stale_ns) {
        return false;
    }
    if h.hubOwnerToken
        .compare_exchange(cur, token, AcqRel, Acquire)
        .is_err()
    {
        return false;
    }
    h.hubOwnerPid.store(current_pid(), Relaxed);
    h.hubHeartbeatNs.store(now_ns(), Release);
    true
}

pub fn release_hub(bus: &BusLayout, token: u64) {
    if bus
        .header
        .hubOwnerToken
        .compare_exchange(token, 0, AcqRel, Relaxed)
        .is_ok()
    {
        bus.header.hubFlags.store(0, Relaxed);
        bus.streamHeader.active.store(0, Release);
    }
}

// ---------------------------------------------------------------------------------------------
// mailboxes (Hub -> Track / App Audio)

fn post_to(write: &AtomicU32, cmds: &[Shared<Command>; kMailboxSize], id: u32, value: f32) {
    let w = write.load(Relaxed);
    put(
        &cmds[w as usize % kMailboxSize],
        Command { paramId: id, value },
    );
    write.store(w.wrapping_add(1), Release);
}

fn poll_from(
    write: &AtomicU32,
    cmds: &[Shared<Command>; kMailboxSize],
    cursor: &mut u32,
    mut f: impl FnMut(u32, f32),
) -> usize {
    let w = write.load(Acquire);
    if w.wrapping_sub(*cursor) > kMailboxSize as u32 {
        *cursor = w.wrapping_sub(kMailboxSize as u32); // dropped oldest
    }
    let mut count = 0;
    while *cursor != w {
        let c = take(&cmds[*cursor as usize % kMailboxSize]);
        f(c.paramId, c.value);
        *cursor = cursor.wrapping_add(1);
        count += 1;
    }
    count
}

/// `id` is a `ParamId` value.
pub fn post_command(slot: &SlotHeader, id: u32, value: f32) {
    post_to(&slot.cmdWrite, &slot.cmds, id, value);
}

/// Delivers the commands posted since `cursor` (a `ParamId` value and the float); returns how many.
pub fn poll_commands(slot: &SlotHeader, cursor: &mut u32, f: impl FnMut(u32, f32)) -> usize {
    poll_from(&slot.cmdWrite, &slot.cmds, cursor, f)
}

pub fn request_rename(slot: &SlotHeader, name: &str) {
    request_text(&slot.renameSeq, &slot.renameTo, name);
}

pub fn poll_rename(slot: &SlotHeader, last_seq: &mut u32) -> Option<String> {
    poll_text(&slot.renameSeq, &slot.renameTo, last_seq)
}

// ---------------------------------------------------------------------------------------------
// remote control (any process -> Hub), multi-writer

/// `target` -1 addresses the Hub (`param_id` is a `RemoteParam`), 0..63 a Track slot (a `ParamId`).
pub fn post_remote(bus: &BusLayout, target: i32, param_id: u32, value: f32) {
    let h = &bus.header;
    let idx = h.remoteReserve.fetch_add(1, AcqRel);
    let e = &h.remote[idx as usize % kRemoteQueueSize];
    e.seq.store(0, Relaxed);
    fence(Release);
    e.target.store(target, Relaxed);
    e.paramId.store(param_id, Relaxed);
    e.valueBits.store(float_bits(value), Relaxed);
    e.seq.store(idx.wrapping_add(1), Release);
}

/// Hub side (single reader): delivers complete commands in order as `(target, param_id, value)`.
/// An entry still being written stops delivery until the next call.
pub fn poll_remote(bus: &BusLayout, cursor: &mut u32, mut f: impl FnMut(i32, u32, f32)) -> usize {
    let h = &bus.header;
    let reserve = h.remoteReserve.load(Acquire);
    if reserve.wrapping_sub(*cursor) > kRemoteQueueSize as u32 {
        *cursor = reserve.wrapping_sub(kRemoteQueueSize as u32); // dropped oldest
    }
    let mut count = 0;
    while *cursor != reserve {
        let e = &h.remote[*cursor as usize % kRemoteQueueSize];
        let s = e.seq.load(Acquire);
        let want = cursor.wrapping_add(1);
        if s == want {
            let target = e.target.load(Relaxed);
            let pid = e.paramId.load(Relaxed);
            let v = bits_float(e.valueBits.load(Relaxed));
            fence(Acquire);
            if e.seq.load(Relaxed) == s {
                f(target, pid, v);
                count += 1;
            }
        } else if (s.wrapping_sub(want) as i32) <= 0 {
            break; // claimed but not published yet: try again next time
        }
        *cursor = cursor.wrapping_add(1); // delivered, or overwritten by a newer command
    }
    count
}

// ---------------------------------------------------------------------------------------------
// App sources

pub fn claim_source(bus: &BusLayout) -> Option<usize> {
    for (i, s) in bus.sources.iter().enumerate() {
        if s.state
            .compare_exchange(
                SlotState::kSlotFree,
                SlotState::kSlotClaiming,
                AcqRel,
                Relaxed,
            )
            .is_err()
        {
            continue;
        }
        s.ownerPid.store(current_pid(), Relaxed);
        s.sequence
            .store(bus.header.slotSequence.fetch_add(1, Relaxed) + 1, Relaxed);
        set_source_identity(s, "", "", 0);
        s.flags.store(0, Relaxed);
        s.heartbeatNs.store(now_ns(), Relaxed);
        s.capture.store(0, Relaxed);
        s.levelBits.store(float_bits(0.0), Relaxed);
        s.peakBits.store(0, Relaxed);
        s.latencyBits.store(0, Relaxed);
        s.recordMs.store(0, Relaxed);
        s.delayBits.store(float_bits(0.0), Relaxed);
        s.hubMute.store(0, Relaxed);
        s.trackSlot.store(-1, Relaxed);
        s.state.store(SlotState::kSlotActive, Release);
        return Some(i);
    }
    None
}

pub fn release_source(bus: &BusLayout, index: usize) {
    let Some(s) = bus.sources.get(index) else {
        return;
    };
    s.flags.store(0, Relaxed);
    s.state.store(SlotState::kSlotFree, Release);
}

pub fn set_source_identity(src: &SourceHeader, name: &str, app: &str, color_argb: u32) {
    seq_write(&src.nameSeq, || {
        copy_name(&src.name, name);
        copy_name(&src.app, app);
        put(&src.colorARGB, color_argb);
    });
}

/// `(name, app, colorARGB)`.
pub fn read_source_identity(src: &SourceHeader) -> Option<(String, String, u32)> {
    seq_read(&src.nameSeq, || {
        (take(&src.name), take(&src.app), take(&src.colorARGB))
    })
    .map(|(n, a, c)| (cstring(&n), cstring(&a), c))
}

/// `id` is a `SourceParam` value.
pub fn post_source_command(src: &SourceHeader, id: u32, value: f32) {
    post_to(&src.cmdWrite, &src.cmds, id, value);
}

pub fn poll_source_commands(
    src: &SourceHeader,
    cursor: &mut u32,
    f: impl FnMut(u32, f32),
) -> usize {
    poll_from(&src.cmdWrite, &src.cmds, cursor, f)
}

/// The Hub asks the App Audio to capture another program ("" = none).
pub fn request_source_app(src: &SourceHeader, exe: &str) {
    request_text(&src.appSeq, &src.appTo, exe);
}

pub fn poll_source_app(src: &SourceHeader, last_seq: &mut u32) -> Option<String> {
    poll_text(&src.appSeq, &src.appTo, last_seq)
}

// ---------------------------------------------------------------------------------------------
// friends room + feeders (v13)

pub fn set_friend_name(f: &FriendHeader, name: &str) {
    seq_write(&f.nameSeq, || copy_name(&f.name, name));
}

/// Consistent copy (empty if a writer kept interfering).
pub fn read_friend_name(f: &FriendHeader) -> String {
    seq_read(&f.nameSeq, || take(&f.name)).map_or_else(String::new, |n| cstring(&n))
}

pub fn claim_feeder(bus: &BusLayout) -> Option<usize> {
    for (i, f) in bus.feeders.iter().enumerate() {
        if f.state
            .compare_exchange(
                SlotState::kSlotFree,
                SlotState::kSlotClaiming,
                AcqRel,
                Relaxed,
            )
            .is_err()
        {
            continue;
        }
        f.ownerPid.store(current_pid(), Relaxed);
        f.friendId.store(0, Relaxed);
        set_feeder_identity(f, "", 0);
        f.heartbeatNs.store(now_ns(), Relaxed);
        f.blockCount.store(0, Relaxed);
        f.status.store(FeederStatus::kFeederNoFriend, Relaxed);
        f.reply.store(0, Relaxed);
        f.hubCommand.store(0, Relaxed);
        f.sampleRate.store(0, Relaxed);
        // SAFETY: `mono` is `[f32; kTapFrames]` inside the live mapping.
        unsafe { ptr::write_bytes(bus.feederTap.0[i].mono.get().cast::<f32>(), 0, kTapFrames) };
        f.tapWrite.0.store(0, Relaxed);
        f.state.store(SlotState::kSlotActive, Release);
        return Some(i);
    }
    None
}

pub fn release_feeder(bus: &BusLayout, index: usize) {
    let Some(f) = bus.feeders.get(index) else {
        return;
    };
    f.friendId.store(0, Relaxed);
    f.state.store(SlotState::kSlotFree, Release);
}

pub fn set_feeder_identity(f: &FeederRecord, track_name: &str, color_argb: u32) {
    seq_write(&f.nameSeq, || {
        copy_name(&f.trackName, track_name);
        put(&f.colorARGB, color_argb);
    });
}

/// `(trackName, colorARGB)`.
pub fn read_feeder_identity(f: &FeederRecord) -> Option<(String, u32)> {
    seq_read(&f.nameSeq, || (take(&f.trackName), take(&f.colorARGB))).map(|(n, c)| (cstring(&n), c))
}

/// Any plug-in -> Hub (multi-writer). `kind` is a `RequestKind` value; `text` may be empty. The answer
/// arrives in `feeders[feeder].reply` / `replySeq`.
pub fn post_request(bus: &BusLayout, feeder: i32, kind: u32, arg: u32, text: &str) {
    let idx = bus.requestReserve.0.fetch_add(1, AcqRel);
    let e = &bus.requests[idx as usize % kRequestQueueSize];
    e.seq.store(0, Relaxed);
    fence(Release);
    e.feeder.store(feeder, Relaxed);
    e.kind.store(kind, Relaxed);
    e.arg.store(arg, Relaxed);
    copy_name(&e.text, text);
    e.seq.store(idx.wrapping_add(1), Release);
}

/// Hub side (single reader): delivers complete requests in order as `(feeder, kind, arg, text)`, then
/// wipes the text (a pasted token must not stay in shared memory).
pub fn poll_requests(
    bus: &BusLayout,
    cursor: &mut u32,
    mut f: impl FnMut(i32, u32, u32, String),
) -> usize {
    let reserve = bus.requestReserve.0.load(Acquire);
    if reserve.wrapping_sub(*cursor) > kRequestQueueSize as u32 {
        *cursor = reserve.wrapping_sub(kRequestQueueSize as u32); // dropped oldest
    }
    let mut count = 0;
    while *cursor != reserve {
        let e = &bus.requests[*cursor as usize % kRequestQueueSize];
        let s = e.seq.load(Acquire);
        let want = cursor.wrapping_add(1);
        if s == want {
            let feeder = e.feeder.load(Relaxed);
            let kind = e.kind.load(Relaxed);
            let arg = e.arg.load(Relaxed);
            let mut text = take(&e.text);
            fence(Acquire);
            if e.seq.load(Relaxed) == s {
                clear_name(&e.text);
                text[kNameBytes - 1] = 0;
                f(feeder, kind, arg, cstring(&text));
                count += 1;
            }
        } else if (s.wrapping_sub(want) as i32) <= 0 {
            break; // claimed but not published yet
        }
        *cursor = cursor.wrapping_add(1);
    }
    count
}

/// Hub -> feeder answer (single writer per feeder).
pub fn post_reply(f: &FeederRecord, reply: u32) {
    f.reply.store(reply, Relaxed);
    f.replySeq.fetch_add(1, Release);
}

/// The answer once `replySeq` moved past `last_seq`. Poll from a timer; never block the message thread.
pub fn poll_reply(f: &FeederRecord, last_seq: &mut u32) -> Option<u32> {
    let s = f.replySeq.load(Acquire);
    if s == *last_seq {
        return None;
    }
    let reply = f.reply.load(Relaxed);
    *last_seq = s;
    Some(reply)
}

/// Hub: name of stream output `index` (0 = Stream Mix, 1..8 stems); cut on a code point.
pub fn set_stream_name(sh: &StreamOutHeader, index: usize, name: &str) {
    seq_write(&sh.nameSeq, || copy_name(&sh.names[index], name));
}

/// Consistent copy of the name of stream output `index` (NUL terminated); `None` while a writer is busy.
pub fn read_stream_name(sh: &StreamOutHeader, index: usize) -> Option<[u8; kNameBytes]> {
    let s1 = sh.nameSeq.load(Acquire);
    if s1 & 1 != 0 {
        return None;
    }
    let mut buf = take(&sh.names[index]);
    fence(Acquire);
    if sh.nameSeq.load(Acquire) != s1 {
        return None;
    }
    buf[kNameBytes - 1] = 0;
    Some(buf)
}

// ---------------------------------------------------------------------------------------------
// beacon

/// Hub: call about every 500 ms from a non-audio thread.
pub fn beacon_publish(b: &BeaconHeader) {
    b.protocolVersion.store(kProtocolVersion, Relaxed);
    b.hubPid.store(current_pid(), Relaxed);
    b.heartbeatNs.store(now_ns(), Relaxed);
    b.magic.store(kBeaconMagic, Release);
}

/// Protocol version of a running Hub on this bus, 0 when there is none (or it is stale).
pub fn beacon_hub_version(b: &BeaconHeader, stale_ns: u64) -> u32 {
    if b.magic.load(Acquire) != kBeaconMagic {
        return 0;
    }
    let hb = b.heartbeatNs.load(Relaxed);
    let now = now_ns();
    if now > hb && now - hb > stale_ns {
        return 0;
    }
    if !process_alive(b.hubPid.load(Relaxed)) {
        return 0;
    }
    b.protocolVersion.load(Relaxed)
}

pub const BEACON_STALE_NS: u64 = 3_000_000_000;

// ---------------------------------------------------------------------------------------------
// timeline tags

/// Audio thread of the Track: call before writing the block to the FIFO. No allocation, no locks.
pub fn write_tag(
    slot: &SlotHeader,
    tags: &SlotTags,
    fifo_pos: u64,
    time_samples: i64,
    num_frames: u32,
    playing: bool,
) {
    let n = slot.tagWrite.load(Relaxed);
    let t = &tags.tags[(n % kTagRingSize as u64) as usize];
    t.seq.store(0, Relaxed);
    fence(Release);
    t.fifoPos.store(fifo_pos, Relaxed);
    t.timeSamples.store(time_samples, Relaxed);
    t.numFrames.store(num_frames, Relaxed);
    t.playing.store(u32::from(playing), Relaxed);
    t.seq.store(n + 1, Release);
    slot.tagWrite.store(n + 1, Release);
}

/// The FIFO frame that corresponds to the requested timeline sample.
pub fn find_timeline_position(
    slot: &SlotHeader,
    tags: &SlotTags,
    time_samples: i64,
    max_tags: usize,
) -> Option<u64> {
    let written = slot.tagWrite.load(Acquire);
    let count = written.min(max_tags.min(kTagRingSize - 4) as u64);
    for k in 0..count {
        let idx = written - 1 - k;
        let t = &tags.tags[(idx % kTagRingSize as u64) as usize];
        let s1 = t.seq.load(Acquire);
        if s1 != idx + 1 {
            continue; // being rewritten or lapped
        }
        let fifo = t.fifoPos.load(Relaxed);
        let time = t.timeSamples.load(Relaxed);
        let n = t.numFrames.load(Relaxed);
        let play = t.playing.load(Relaxed);
        fence(Acquire);
        if t.seq.load(Relaxed) != s1 {
            continue;
        }
        if play == 0 {
            return None; // newest relevant history is "stopped": no timeline
        }
        // The tag comes from another process: wrap instead of panicking on garbage (as C++ does).
        let end = time.wrapping_add(i64::from(n));
        if time_samples >= time && time_samples < end {
            return Some(fifo.wrapping_add(time_samples.wrapping_sub(time) as u64));
        }
        if time_samples >= end {
            return None; // requested time is newer than the track
        }
    }
    None
}
