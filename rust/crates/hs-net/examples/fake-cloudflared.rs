//! Test double for `cloudflared` (used by `tests/share.rs`): records its arguments and pid next to itself, then acts as
//! the file `cloudflared.mode` next to it says: `ready` (prints a trycloudflare URL like the real one, on stderr),
//! `split` (the URL arrives in two writes), `api-error` (only an error line naming api.trycloudflare.com), `silent`,
//! `exit` (prints an error and exits).
use std::io::Write;
use std::time::Duration;

fn main() {
    let exe = std::env::current_exe().expect("exe path");
    let dir = exe.parent().expect("exe dir");
    let args: Vec<String> = std::env::args().skip(1).collect();
    let _ = std::fs::write(dir.join("cloudflared.args"), args.join(" "));
    let _ = std::fs::write(dir.join("cloudflared.pid"), std::process::id().to_string());
    let mode = std::fs::read_to_string(dir.join("cloudflared.mode")).unwrap_or_default();
    let mut err = std::io::stderr();
    let mut say = |text: &str| {
        let _ = err.write_all(text.as_bytes());
        let _ = err.flush();
    };
    say("2026-10-30T10:00:00Z INF Thank you for trying Cloudflare Tunnel.\n");
    match mode.trim() {
        "ready" => {
            say("2026-10-30T10:00:01Z INF Requesting new quick Tunnel on trycloudflare.com...\n");
            say("2026-10-30T10:00:02Z INF |  https://fake-test-tunnel.trycloudflare.com  |\n");
        }
        "split" => {
            say("INF |  https://split-te");
            std::thread::sleep(Duration::from_millis(300));
            say("st.trycloudflare.com  |\n");
        }
        "api-error" => {
            say(
                "2026-10-30T10:00:01Z ERR failed to request quick Tunnel: Post \"https://api.trycloudflare.com/tunnel\": dial tcp: lookup api.trycloudflare.com: no such host\n",
            );
        }
        "exit" => {
            say("2026-10-30T10:00:01Z ERR something went wrong\n");
            std::process::exit(1);
        }
        _ => {}
    }
    loop {
        std::thread::sleep(Duration::from_millis(200));
    }
}
