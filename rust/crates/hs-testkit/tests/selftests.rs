//! The `--test-*` flags run as cargo tests (each on its own bus, its own REST port and temp files), plus the bugs of the
//! C++ checks fixed here (ledger 106..) and the `--serve-friend` contract the Node tests read.
use hs_testkit::checks::{directory, friends, rest, serve, ui};
use hs_testkit::hub::HubOptions;
use hs_testkit::report::Report;
use hs_testkit::ws::WsClient;
use std::io::Write;
use std::path::PathBuf;
use std::sync::atomic::{AtomicU32, Ordering};
use std::sync::{Arc, Mutex};
use std::time::{Duration, Instant};

static COUNTER: AtomicU32 = AtomicU32::new(0);

/// A folder of its own, emptied first and removed at the end.
struct Temp(PathBuf);

impl Temp {
    fn new(tag: &str) -> Temp {
        let n = COUNTER.fetch_add(1, Ordering::Relaxed);
        let d = std::env::temp_dir().join(format!("hs-testkit-{tag}-{}-{n}", std::process::id()));
        let _ = std::fs::remove_dir_all(&d);
        std::fs::create_dir_all(&d).unwrap();
        Temp(d)
    }
}

impl Drop for Temp {
    fn drop(&mut self) {
        let _ = std::fs::remove_dir_all(&self.0);
    }
}

fn opts(tag: &str, dir: &Temp) -> HubOptions {
    let n = COUNTER.fetch_add(1, Ordering::Relaxed);
    HubOptions {
        bus: format!("tk-{tag}-{}-{n}", std::process::id()),
        settings_path: Some(dir.0.join("settings.xml")),
        recordings_dir: Some(dir.0.join("takes")),
        demo_tracks: true,
        rest_ports: Some(vec![0]),
    }
}

fn assert_passed(r: &Report, code: i32) {
    assert_eq!(code, 0, "{}", r.lines().join("\n"));
    assert_eq!(r.failures(), 0, "{}", r.lines().join("\n"));
}

#[test]
fn test_directory_passes() {
    let r = Report::new(false);
    let code = directory::run(&r);
    assert_passed(&r, code);
    assert_eq!(r.lines().last().unwrap(), "share directory: all passed");
}

#[test]
fn test_ui_passes_and_its_summary_comes_last() {
    // C++ bug (106): "ui: all passed" was printed before the last check, so a failure of that check (and of the account
    // part) showed under a summary that said passed
    let dir = Temp::new("ui");
    let r = Report::new(false);
    let code = ui::run_ui(&r, &dir.0.join("account.dat"));
    assert_passed(&r, code);
    let lines = r.lines();
    assert_eq!(lines.last().unwrap(), "ui: all passed");
    assert_eq!(lines.iter().filter(|l| l.starts_with("ui: ")).count(), 1);
    let buffer = lines
        .iter()
        .position(|l| l.contains("buffer warning"))
        .unwrap();
    let account = lines.iter().position(|l| l.contains("DPAPI")).unwrap();
    assert!(buffer < lines.len() - 1 && account < lines.len() - 1);
}

#[test]
fn a_failed_check_is_reported_at_the_end_and_in_the_exit_code() {
    let r = Report::new(false);
    r.expect("fine", true);
    r.expect("broken", false);
    r.expect("fine again", true);
    assert_eq!(r.finish("thing"), 1);
    let lines = r.lines();
    assert_eq!(lines[1], "FAIL broken");
    assert_eq!(lines.last().unwrap(), "thing: FAILED");
}

#[test]
fn test_rest_passes() {
    let dir = Temp::new("rest");
    let r = Report::new(false);
    let code = rest::run(&r, &opts("rest", &dir));
    assert_passed(&r, code);
    assert_eq!(r.lines().last().unwrap(), "REST API: all passed");
}

#[test]
fn test_friends_passes_and_leaves_other_takes_alone() {
    // C++ bug (107): the check deleted the newest "Mint *.wav" of the user's Recordings folder (a real take of a friend called
    // Mint, if its own take was not the newest). It deletes only the take it made.
    let dir = Temp::new("friends");
    let o = opts("friends", &dir);
    let takes = o.recordings_dir.clone().unwrap();
    std::fs::create_dir_all(&takes).unwrap();
    let keep = takes.join("Mint 2099-01-01 00-00-00.wav");
    std::fs::write(&keep, vec![0u8; 4096]).unwrap();
    // the newest of its name by far: a check that deletes "the newest Mint take" would take this one
    let future = std::time::UNIX_EPOCH + Duration::from_secs(4_070_908_800); // 2099
    std::fs::File::options()
        .write(true)
        .open(&keep)
        .unwrap()
        .set_modified(future)
        .unwrap();
    let r = Report::new(false);
    let code = friends::run(&r, &o);
    assert_passed(&r, code);
    assert!(keep.exists(), "someone else's take was deleted");
    let left: Vec<_> = std::fs::read_dir(&takes).unwrap().flatten().collect();
    assert_eq!(left.len(), 1, "the check's own take is removed: {left:?}");
}

/// What `serve_friend` writes, readable while it runs.
#[derive(Clone, Default)]
struct Shared(Arc<Mutex<Vec<u8>>>);

impl Write for Shared {
    fn write(&mut self, b: &[u8]) -> std::io::Result<usize> {
        self.0.lock().unwrap().extend_from_slice(b);
        Ok(b.len())
    }
    fn flush(&mut self) -> std::io::Result<()> {
        Ok(())
    }
}

impl Shared {
    fn text(&self) -> String {
        String::from_utf8_lossy(&self.0.lock().unwrap()).into_owned()
    }
}

#[test]
fn serve_friend_prints_the_link_and_what_the_hub_saw() {
    let dir = Temp::new("serve");
    let o = opts("serve", &dir);
    let out = Shared::default();
    let sink = out.clone();
    let hub = std::thread::spawn(move || serve::serve_friend(&o, 4, &mut { sink }));
    // the friend: a plain client, like friend-wire.test.mjs
    let start = Instant::now();
    let url = loop {
        if let Some(u) = out
            .text()
            .lines()
            .find_map(|l| l.strip_prefix("FRIEND_URL="))
        {
            break u.to_owned();
        }
        assert!(
            start.elapsed() < Duration::from_secs(20),
            "no FRIEND_URL: {}",
            out.text()
        );
        std::thread::sleep(Duration::from_millis(50));
    };
    let rest = url.strip_prefix("http://127.0.0.1:").unwrap();
    let (port, path) = rest.split_once("/s/").unwrap();
    assert_eq!(path.len(), 26, "the token");
    let mut c = WsClient::new();
    assert!(c.open(port.parse().unwrap(), &format!("/ws/s/{path}"), ""));
    let (mut hello, mut audio) = (false, 0);
    for i in 0..150 {
        // HRA1 mono, 10 ms, like the page
        let mut b = vec![0u8; 16 + 480 * 2];
        b[..4].copy_from_slice(b"HRA1");
        b[4..8].copy_from_slice(&48_000u32.to_le_bytes());
        b[8..10].copy_from_slice(&1u16.to_le_bytes());
        b[12..16].copy_from_slice(&480u32.to_le_bytes());
        for k in 0..480 {
            let v = (8000.0 * (0.058 * (i * 480 + k) as f64).sin()) as i16;
            b[16 + 2 * k..18 + 2 * k].copy_from_slice(&v.to_le_bytes());
        }
        c.send(&b);
        let mut closed = false;
        while let Some(m) = c.receive(1, &mut closed) {
            if m.starts_with(b"HRA2") {
                audio += 1;
            } else if m.starts_with(b"{\"v\"") {
                hello = String::from_utf8_lossy(&m).contains("\"name\": \"Mint\"");
            }
        }
        std::thread::sleep(Duration::from_millis(10));
    }
    assert!(hello, "the Hub says who the friend is");
    assert!(audio > 20, "the mix comes back ({audio})");
    let code = hub.join().unwrap();
    let text = out.text();
    let lines: Vec<&str> = text.lines().collect();
    assert!(
        lines[0].starts_with("FRIEND_URL=http://127.0.0.1:"),
        "{text}"
    );
    let result = lines
        .iter()
        .find(|l| l.starts_with("RESULT "))
        .expect("a RESULT line");
    for k in [
        "live", "delay", "peak", "playing", "writePos", "rtt", "up", "down", "micGap", "lag",
        "skips",
    ] {
        assert!(result.contains(&format!("{k}=")), "{k} in {result}");
    }
    assert!(result.starts_with("RESULT live=1 "), "{result}");
    let write_pos: u64 = result
        .split("writePos=")
        .nth(1)
        .unwrap()
        .split(' ')
        .next()
        .unwrap()
        .parse()
        .unwrap();
    assert!(
        write_pos >= 150 * 480 * 8 / 10,
        "the microphone arrived ({write_pos})"
    );
    assert_eq!(code, 0, "the friend was live: exit 0");
}

#[test]
fn serve_friend_without_a_friend_exits_1_but_still_prints_a_result() {
    let dir = Temp::new("serve-none");
    let mut out = Vec::new();
    let code = serve::serve_friend(&opts("serve-none", &dir), 1, &mut out);
    let text = String::from_utf8(out).unwrap();
    assert_eq!(code, 1, "{text}");
    assert!(text.contains("RESULT live=0 delay=-1.0 "), "{text}");
}

#[test]
fn the_friend_ring_peak_catches_a_beep_between_two_looks() {
    use hs_bus::ring::ring_write;
    let dir = Temp::new("ringpeak");
    let mut hub = hs_testkit::hub::TestHub::new(&HubOptions {
        demo_tracks: false,
        ..opts("ringpeak", &dir)
    });
    hub.pump(100);
    let bus = hub.bus().expect("a bus");
    let (ring, pos) = (&bus.friendAudio.0[2], &bus.friends.0[2].writePos.0);
    let mut cursor = 0u64;
    assert_eq!(hub.friend_ring_peak(2, &mut cursor), 0.0, "nothing arrived");
    // a short beep, then 90 ms of silence: the header would show the silence
    let beep: Vec<f32> = (0..480).map(|k| 0.8 * (k as f32 * 0.2).sin()).collect();
    ring_write(ring, pos, &[&beep, &beep], 480);
    let silence = vec![0.0f32; 4320];
    ring_write(ring, pos, &[&silence, &silence], 4320);
    let p = hub.friend_ring_peak(2, &mut cursor);
    assert!(p > 0.7 && p <= 0.8, "{p}");
    assert_eq!(cursor, 4800);
    assert_eq!(
        hub.friend_ring_peak(2, &mut cursor),
        0.0,
        "only what is new"
    );
    // a friend who comes back starts the ring again: the cursor follows
    cursor = 10_000_000;
    ring_write(ring, pos, &[&beep, &beep], 480);
    assert!(hub.friend_ring_peak(2, &mut cursor) > 0.7);
    assert_eq!(hub.friend_ring_peak(9, &mut cursor), 0.0, "no such slot");
}

#[test]
fn demo_and_friends_keep_to_their_own_settings() {
    // the demo stores its share base in a settings file of its own; the friends check switches permanent links on and off
    // in a temp file and leaves the snapshot settings alone
    let dir = Temp::new("settings");
    let o = opts("settings", &dir);
    let mut out = Vec::new();
    let base = "https://demo-share.example";
    let before = std::fs::read(hs_testkit::hub::snapshot_settings_path()).ok();
    let code = serve::demo(&o, 1, Some(base), None, &mut out);
    assert_eq!(code, 0);
    let after = std::fs::read(hs_testkit::hub::snapshot_settings_path()).ok();
    assert_eq!(before, after, "the shared snapshot settings were changed");
    let text = String::from_utf8(out).unwrap();
    assert!(
        text.contains("permanent listen link https://demo-share.example/l/"),
        "{text}"
    );
    let mine = std::fs::read_to_string(dir.0.join("settings.xml")).unwrap();
    assert!(mine.contains("demo-share.example"), "{mine}");
}
