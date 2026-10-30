use hs_bus::paced_send::{SendQueue, catch_up};

/// A socket that takes at most `room` bytes before it says "full".
#[derive(Default)]
struct FakeSocket {
    got: Vec<u8>,
    room: usize,
    gone: bool,
}

impl FakeSocket {
    fn take(&mut self, p: &[u8]) -> isize {
        if self.gone {
            return -1;
        }
        let k = p.len().min(self.room);
        self.got.extend_from_slice(&p[..k]);
        self.room -= k;
        k as isize
    }
}

fn bytes(c: u8, n: usize) -> Vec<u8> {
    vec![c; n]
}

#[test]
fn send_queue_a_packet_the_socket_takes_in_pieces_arrives_whole_and_in_order() {
    let mut q = SendQueue::new();
    let mut s = FakeSocket::default();
    let (a, b) = (bytes(b'a', 100), bytes(b'b', 50));
    assert!(q.offer(&a));
    s.room = 30;
    assert!(q.flush(|p| s.take(p)));
    assert!(!q.idle());
    assert_eq!(q.pending(), 70);
    s.room = 1000;
    assert!(q.flush(|p| s.take(p)));
    assert!(q.idle());
    assert!(q.offer(&b));
    assert!(q.flush(|p| s.take(p)));
    assert_eq!(s.got, [a, b].concat());
}

#[test]
fn send_queue_audio_is_dropped_while_a_packet_waits_a_control_message_is_kept_behind_it() {
    let mut q = SendQueue::new();
    let mut s = FakeSocket::default();
    let (a, late, pong) = (bytes(b'a', 100), bytes(b'x', 100), bytes(b'p', 4));
    assert!(q.offer(&a));
    s.room = 0;
    assert!(q.flush(|p| s.take(p))); // "full" is not an error
    assert!(!q.offer(&late)); // dropped: the queue stays at one packet
    q.append(&pong);
    assert_eq!(q.pending(), 104);
    s.room = 1000;
    assert!(q.flush(|p| s.take(p)));
    assert_eq!(s.got, [a, pong].concat());
    assert!(q.idle());
}

#[test]
fn send_queue_a_closed_connection_is_reported_a_full_one_is_not() {
    let mut q = SendQueue::new();
    let mut s = FakeSocket::default();
    q.offer(&bytes(b'a', 10));
    s.gone = true;
    assert!(!q.flush(|p| s.take(p)));
}

#[test]
fn catch_up_a_line_that_falls_behind_jumps_near_the_newest_audio_a_healthy_one_never_does() {
    let rate = 48000;
    assert_eq!(catch_up(1000, 1000 + 4800, rate, 150, 30), 1000); // 100 ms behind, allowed 150
    assert_eq!(catch_up(1000, 1000 + 7200, rate, 150, 30), 1000); // exactly 150 ms: stay
    let w = 100_000;
    assert_eq!(catch_up(1000, w, rate, 150, 30), w - 1440); // far behind: 30 ms before now
    assert_eq!(catch_up(w + 5, w, rate, 150, 30), w + 5); // Hub restarted: not our business here
    assert_eq!(catch_up(0, 500, rate, 1, 1000), 0); // keep is longer than all there is: from the start
}

#[test]
fn send_queue_pongs_behind_a_stuck_line_stop_at_the_cap_however_many_pings_come() {
    let mut q = SendQueue::new();
    let mut s = FakeSocket::default(); // takes nothing: the line is stuck
    let (audio, pong) = (bytes(b'a', 2000), bytes(b'p', 127));
    q.offer(&audio);
    let mut kept = 0;
    for _ in 0..1000 {
        kept += usize::from(q.append_if_room(&pong, 1024));
        assert!(q.flush(|p| s.take(p)));
    }
    assert_eq!(kept, 9); // the audio packet does not count against the pongs: 9 x 127 bytes pass 1 KB
    assert_eq!(q.pending(), audio.len() + 9 * pong.len());
    assert_eq!(q.control_pending(), 9 * pong.len());
}

#[test]
fn send_queue_memory_stays_small_when_a_slow_line_never_lets_the_queue_go_empty() {
    let mut q = SendQueue::new();
    let mut s = FakeSocket::default();
    let mut most = 0;
    let mut all = Vec::new(); // what the queue accepted, in order
    for i in 0..10_000usize {
        // every pong is different: lost or doubled bytes show
        let mut pong = (1_000_000 + i).to_string().into_bytes();
        let mut k = 0;
        while pong.len() < 127 {
            pong.push(b'a' + ((i + k * 7) % 26) as u8);
            k += 1;
        }
        if q.append_if_room(&pong, 1024) {
            all.extend_from_slice(&pong);
        }
        s.room = 100; // the line takes a little each round, so there is always something waiting
        assert!(q.flush(|p| s.take(p)));
        most = most.max(q.stored());
    }
    assert!(!q.idle());
    assert!(most < 8192);
    s.room = 1 << 20;
    assert!(q.flush(|p| s.take(p)));
    assert!(q.idle());
    assert_eq!(s.got, all); // compaction lost and moved nothing
}

#[test]
fn send_queue_an_audio_packet_sent_in_part_keeps_its_place_before_the_pong_behind_it() {
    let mut q = SendQueue::new();
    let mut s = FakeSocket::default();
    let audio: Vec<u8> = (0..6000).map(|i| b'A' + (i % 23) as u8).collect();
    let pong = bytes(b'p', 127);
    assert!(q.offer(&audio));
    s.room = 5000;
    assert!(q.flush(|p| s.take(p))); // 5000 of 6000 out
    assert!(q.append_if_room(&pong, 1024)); // compaction is allowed here: the head is over 4 KB
    assert_eq!(q.control_pending(), pong.len());
    assert_eq!(q.pending(), 1000 + pong.len());
    s.room = 1 << 20;
    assert!(q.flush(|p| s.take(p)));
    assert_eq!(s.got, [audio, pong].concat());
}
