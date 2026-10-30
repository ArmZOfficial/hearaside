//! The send-in link (`/s/<token>`, App Audio "*link*"): a browser microphone written into the bus. The newest sender
//! wins. Port of `ShareServer::receiveFrom`.
use super::ws::{self, MAGIC1};
use super::{CountGuard, Ctx, MAX_PCM_FRAMES, lock};
use hs_bus::ring::ring_write;
use hs_bus::shm::now_ns;
use std::net::TcpStream;
use std::sync::atomic::Ordering;

/// Mono / stereo PCM16 of an HRA1 / HRA2 body to two float channels (a mono packet fills both).
pub(crate) fn pcm_to_float(
    pcm: &[u8],
    chans: usize,
    frames: usize,
    l: &mut Vec<f32>,
    r: &mut Vec<f32>,
) {
    l.clear();
    r.clear();
    for i in 0..frames {
        let at = |c: usize| {
            let o = (i * chans + c) * 2;
            f32::from(i16::from_le_bytes([pcm[o], pcm[o + 1]])) / 32768.0
        };
        l.push(at(0));
        r.push(at(chans - 1));
    }
}

pub(crate) fn receive_from(ctx: &Ctx, s: &TcpStream) {
    let inner = &ctx.inner;
    let li = &ctx.bus.layout().linkIn;
    let gen_ = inner.sender_gen.fetch_add(1, Ordering::SeqCst) + 1;
    let _count = CountGuard::new(&inner.senders);
    let current = || inner.is_running() && inner.sender_gen.load(Ordering::SeqCst) == gen_;
    let mut msg: Vec<u8> = Vec::new();
    let (mut l, mut r) = (Vec::new(), Vec::new());
    while current() {
        let op = ws::read_message(s, &mut msg, 5000, &current, None);
        if op == 0 || op == 8 {
            break;
        }
        if op != 2 || msg.len() < 16 {
            continue;
        }
        let word = |at: usize| u32::from_le_bytes([msg[at], msg[at + 1], msg[at + 2], msg[at + 3]]);
        let (magic, rate, frames) = (word(0), word(4), word(12));
        let chans = usize::from(u16::from_le_bytes([msg[8], msg[9]]));
        if magic != MAGIC1 || !(1..=2).contains(&chans) || !(8000..=192_000).contains(&rate) {
            continue;
        }
        let frames = (frames as usize).min((msg.len() - 16) / (2 * chans));
        if frames > MAX_PCM_FRAMES {
            continue; // not audio: a page sends ~10 ms
        }
        pcm_to_float(&msg[16..], chans, frames, &mut l, &mut r);
        let _w = lock(&inner.link_in_write);
        if inner.sender_gen.load(Ordering::SeqCst) != gen_ {
            break;
        }
        li.sampleRate.store(rate, Ordering::Relaxed);
        let layout = ctx.bus.layout();
        ring_write(
            &layout.linkInAudio.0,
            &li.writePos.0,
            &[&l[..], &r[..]],
            frames as u32,
        );
        li.heartbeatNs.store(now_ns(), Ordering::Relaxed);
        li.active.store(1, Ordering::Release);
    }
    if inner.sender_gen.load(Ordering::SeqCst) == gen_ {
        li.active.store(0, Ordering::Release);
    }
}
