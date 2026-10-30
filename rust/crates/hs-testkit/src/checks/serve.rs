//! `--serve-friend <seconds>`: a Hub with one friend, serving that friend's page; prints `FRIEND_URL=...` and, at the end,
//! what the Hub saw of the friend (for the browser tests in `tests/web-friend`). And `--demo <seconds>`: the Hub and its
//! Tracks kept running on the bus "Snapshot" with the share links on, so the OBS source can be tried live.
use crate::hub::{HubOptions, TestHub};
use hs_net::share::Tunnel;
use std::io::Write;
use std::time::Duration;

/// How long the Hub may take to get the bus and start the share server (another Hub on the bus keeps it for 2 s after it
/// last beat).
const START_WAIT_MS: u64 = 15_000;

/// The `RESULT` line the tests read: `live` `delay` `peak` `playing` `writePos` `rtt` `up` `down` `micGap` `lag` `skips`.
#[derive(Clone, Debug, Default)]
pub struct FriendResult {
    pub live: bool,
    pub delay_ms: f32,
    pub peak: f32,
    pub playing: bool,
    pub write_pos: u64,
    pub rtt_ms: f32,
    pub up_kbps: f32,
    pub down_kbps: f32,
    pub mic_gap_ms: f32,
    pub lag_ms: f32,
    pub skips: u32,
}

impl FriendResult {
    pub fn line(&self) -> String {
        format!(
            "RESULT live={} delay={:.1} peak={:.3} playing={} writePos={} rtt={:.1} up={:.0} down={:.0} micGap={:.0} lag={:.0} skips={}",
            i32::from(self.live),
            self.delay_ms,
            self.peak,
            i32::from(self.playing),
            self.write_pos,
            self.rtt_ms,
            self.up_kbps,
            self.down_kbps,
            self.mic_gap_ms,
            self.lag_ms,
            self.skips
        )
    }
}

fn flush(out: &mut dyn Write) {
    let _ = out.flush();
}

/// Exit code: 0 when the friend was seen live, 1 when not, 2 when the Hub could not start serving.
pub fn serve_friend(opts: &HubOptions, seconds: u32, out: &mut dyn Write) -> i32 {
    let mut hub = TestHub::new(opts);
    let Some(id) = hub.add_friend("Mint") else {
        let _ = writeln!(std::io::stderr(), "hs-testkit: the friends room is full");
        return 2;
    };
    if !hub.pump_until(START_WAIT_MS, |h| h.share().running()) {
        let _ = writeln!(
            std::io::stderr(),
            "hs-testkit: the share server did not start (the bus \"{}\" belongs to another Hub, or ports 47810-47829 are taken)",
            hub.bus_name()
        );
        return 2;
    }
    // the token itself, not the LAN link: a computer with no network address still has a link to this computer
    let _ = writeln!(
        out,
        "FRIEND_URL=http://127.0.0.1:{}/s/{}",
        hub.share().port(),
        hub.friend_token(id)
    );
    flush(out);
    let mut res = FriendResult {
        delay_ms: -1.0,
        ..FriendResult::default()
    };
    // the microphone level comes from the audio that reached the friend's ring, not from the header (the last packet only:
    // a look every 100 ms can fall between the beeps of a fake microphone)
    let mut ring_cursor = 0u64;
    for _ in 0..seconds.saturating_mul(10) {
        hub.pump(100);
        if let Some(f) = hub.friends().first() {
            res.live |= f.live();
            res.delay_ms = res.delay_ms.max(f.delay_ms);
            res.peak = res
                .peak
                .max(hub.friend_ring_peak(usize::try_from(f.slot).unwrap_or(0), &mut ring_cursor));
        }
    }
    let friends = hub.friends();
    let slot = friends.first().map_or(0, |f| f.slot);
    let net = hub.share().friend_stats(slot);
    res.playing = friends.first().is_some_and(|f| f.playing);
    res.write_pos = hub.bus().map_or(0, |b| {
        b.friends.0[usize::try_from(slot).unwrap_or(0)]
            .writePos
            .0
            .load(std::sync::atomic::Ordering::SeqCst)
    });
    res.rtt_ms = net.rtt_ms;
    res.up_kbps = net.up_kbps;
    res.down_kbps = net.down_kbps;
    res.mic_gap_ms = net.mic_gap_max_ms;
    res.lag_ms = net.down_lag_max_ms;
    res.skips = net.down_skips;
    let _ = writeln!(out, "{}", res.line());
    flush(out);
    i32::from(!res.live)
}

/// `--demo <seconds> [--share-base <url>] [--state <file>]`.
pub fn demo(
    opts: &HubOptions,
    seconds: u32,
    share_base: Option<&str>,
    state: Option<&std::path::Path>,
    out: &mut dyn Write,
) -> i32 {
    let mut hub = TestHub::new(opts);
    if let Some(base) = share_base {
        // permanent links through that share web site (web/share-vercel)
        hub.settings().set_share_base(base);
    }
    if let Some(f) = state
        && let Ok(data) = std::fs::read(f)
        && !hub.load_state(&data)
    {
        let _ = writeln!(
            out,
            "the state file {} is not an hs-testkit state (a plug-in project is not read); ignored",
            f.display()
        );
    }
    hub.set_sharing(true); // try the share links (listen / send pages) while the demo runs
    hub.pump(500);
    let _ = writeln!(
        out,
        "while starting (tunnel {}): listen link \"{}\"",
        tunnel_number(hub.share().tunnel()),
        hub.share().listen_url()
    );
    // the internet link (cloudflared)
    hub.pump_until(15_000, |h| h.share().tunnel() != Tunnel::Starting);
    if hub.permanent_links_set() {
        hub.pump_until(10_000, |h| {
            h.directory().state() == hs_app::DirectoryState::Online
        });
    }
    if let Some(f) = state {
        let _ = std::fs::write(f, hub.save_state());
    }
    let _ = writeln!(out, "listen link {}", hub.share().listen_url());
    let _ = writeln!(out, "send link {}", hub.share().send_url());
    if hub.permanent_links_set() {
        let online = hub.directory().state() == hs_app::DirectoryState::Online;
        let _ = writeln!(
            out,
            "permanent listen link {} ({})",
            hub.directory().listen_url(),
            if online { "online" } else { "not online" }
        );
    }
    let _ = writeln!(
        out,
        "demo running for {seconds} s on bus \"{}\"",
        hub.bus_name()
    );
    flush(out);
    hub.pump(u64::from(seconds) * 1000);
    std::thread::sleep(Duration::from_millis(1));
    0
}

/// The number the C++ `enum class Tunnel` prints as.
fn tunnel_number(t: Tunnel) -> i32 {
    match t {
        Tunnel::Off => 0,
        Tunnel::Missing => 1,
        Tunnel::Starting => 2,
        Tunnel::Ready => 3,
        Tunnel::Failed => 4,
    }
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn the_result_line_has_the_keys_the_node_tests_read() {
        let l = FriendResult {
            live: true,
            delay_ms: 87.25,
            peak: 0.3,
            playing: true,
            write_pos: 123_456,
            rtt_ms: 12.5,
            up_kbps: 64.4,
            down_kbps: 96.0,
            mic_gap_ms: 31.0,
            lag_ms: 5.0,
            skips: 2,
        }
        .line();
        assert_eq!(
            l,
            "RESULT live=1 delay=87.2 peak=0.300 playing=1 writePos=123456 rtt=12.5 up=64 down=96 micGap=31 lag=5 skips=2"
        );
        // each key is found by `new RegExp(k + '=(-?[0-9.]+)')`: none may also appear inside another key
        for k in [
            "live", "delay", "peak", "playing", "writePos", "rtt", "up", "down", "micGap", "lag",
            "skips",
        ] {
            assert_eq!(l.matches(&format!("{k}=")).count(), 1, "{k}");
        }
        let unmeasured = FriendResult {
            delay_ms: -1.0,
            rtt_ms: -1.0,
            ..FriendResult::default()
        }
        .line();
        assert!(unmeasured.contains("delay=-1.0") && unmeasured.contains("rtt=-1.0"));
    }
}
