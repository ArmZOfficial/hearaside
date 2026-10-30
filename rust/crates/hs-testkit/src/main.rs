//! `hs-testkit`: the flags of the C++ `ui-snapshot` tool that the tests use, over the Rust stack.
//!
//! ```text
//! hs-testkit --serve-friend <seconds>      a Hub with one friend; prints FRIEND_URL=..., then RESULT ... (friend_page, friend_wire, friend_opus)
//! hs-testkit --test-rest                   the REST API end to end                                       (ctest rest_api)
//! hs-testkit --test-friends                the friends room over real sockets                            (ctest friends_room)
//! hs-testkit --test-directory              permanent links against a mock share site                     (ctest share_directory)
//! hs-testkit --test-ui                     typed values, names, share links, account basics              (ctest ui_values)
//! hs-testkit --test-account                the account basics alone
//! hs-testkit --demo <seconds> [--share-base <url>] [--state <file>]
//! ```
//!
//! Exit codes: 0 all passed, 1 a check failed, 2 wrong use or the Hub could not start, 3 NOT IMPLEMENTED: the flag needs
//! something the Rust side does not have yet (the pictures need the Rust UI, R5; App Audio needs R7). Such a flag prints
//! one `NOT IMPLEMENTED: ...` line and never reports a pass.
use hs_testkit::checks::{directory, friends, rest, serve, ui};
use hs_testkit::hub::{HubOptions, snapshot_account_path};
use hs_testkit::report::Report;
use std::path::PathBuf;
use std::process::ExitCode;

const NOT_IMPLEMENTED: u8 = 3;

fn not_implemented(flag: &str, why: &str) -> ExitCode {
    println!("NOT IMPLEMENTED: {flag}: {why}");
    ExitCode::from(NOT_IMPLEMENTED)
}

fn usage() -> ExitCode {
    eprintln!(
        "usage: hs-testkit --serve-friend <seconds> | --test-rest | --test-friends | --test-directory | --test-ui | --test-account | --demo <seconds> [--share-base <url>] [--state <file>]"
    );
    ExitCode::from(2)
}

fn code(c: i32) -> ExitCode {
    ExitCode::from(u8::try_from(c).unwrap_or(1))
}

/// `juce::String::getIntValue`: the number at the start, else 0.
fn int_value(s: &str) -> i64 {
    let t = s.trim();
    let end = t
        .char_indices()
        .find(|&(i, c)| !(c.is_ascii_digit() || (i == 0 && (c == '-' || c == '+'))))
        .map_or(t.len(), |(i, _)| i);
    t[..end].parse().unwrap_or(0)
}

fn main() -> ExitCode {
    let args: Vec<String> = std::env::args().skip(1).collect();
    let opts = HubOptions::default();
    let flag = args.first().map(String::as_str);
    match flag {
        Some("--help" | "-h") => {
            println!("{}", include_str!("help.txt"));
            ExitCode::SUCCESS
        }
        Some("--test-directory") => code(directory::run(&Report::new(true))),
        Some("--test-ui") => code(ui::run_ui(&Report::new(true), &snapshot_account_path())),
        Some("--test-account") => code(ui::run_account_only(
            &Report::new(true),
            &snapshot_account_path(),
        )),
        Some("--test-account-flow") => not_implemented(
            "--test-account-flow",
            "its second half checks the signed-in panel's rows (the Rust UI is R5); the sign-in flow itself is tested in hs-app (cargo test -p hs-app --test account)",
        ),
        Some("--test-sync" | "--test-app-audio") => not_implemented(
            flag.unwrap_or_default(),
            "needs the App Audio plug-in with its program capture (R7)",
        ),
        Some("--test-friend-app") => not_implemented(
            "--test-friend-app",
            "needs the App Audio plug-in (friend source, Program only, take position: R7); the Hub side of it (the friend's ring and slot) is covered by --test-friends",
        ),
        Some("--audit") => not_implemented("--audit", "draws the editors (the Rust UI is R5)"),
        Some("--test-rest") => code(rest::run(&Report::new(true), &opts)),
        Some("--test-friends") => {
            // never the real Recordings folder or the shared snapshot settings: the check makes and deletes takes, and
            // switches permanent links on and off
            let dir =
                std::env::temp_dir().join(format!("hs-testkit-friends-{}", std::process::id()));
            let o = HubOptions {
                settings_path: Some(dir.join("settings.xml")),
                recordings_dir: Some(dir.join("takes")),
                ..opts
            };
            let c = friends::run(&Report::new(true), &o);
            let _ = std::fs::remove_dir_all(&dir);
            code(c)
        }
        Some("--serve-friend") if args.len() > 1 => {
            let seconds = u32::try_from(int_value(&args[1]).clamp(0, 86_400)).unwrap_or(0);
            code(serve::serve_friend(&opts, seconds, &mut std::io::stdout()))
        }
        _ => {
            // --demo <seconds> [--share-base <url>] [--state <file>] anywhere in the line
            if let Some(i) = args.iter().position(|a| a == "--demo")
                && let Some(sec) = args.get(i + 1)
            {
                let value_of = |name: &str| {
                    args.iter()
                        .position(|a| a == name)
                        .and_then(|k| args.get(k + 1))
                };
                let seconds = u32::try_from(int_value(sec).clamp(0, 86_400)).unwrap_or(0);
                let state = value_of("--state").map(PathBuf::from);
                // the share base of a demo is the demo's: the shared snapshot settings stay as they are
                let settings = std::env::temp_dir()
                    .join(format!("hs-testkit-demo-{}", std::process::id()))
                    .join("settings.xml");
                let demo_opts = HubOptions {
                    settings_path: Some(settings.clone()),
                    ..opts
                };
                let c = serve::demo(
                    &demo_opts,
                    seconds,
                    value_of("--share-base").map(String::as_str),
                    state.as_deref(),
                    &mut std::io::stdout(),
                );
                if let Some(d) = settings.parent() {
                    let _ = std::fs::remove_dir_all(d);
                }
                return code(c);
            }
            match flag {
                Some(f) if f.starts_with("--") => usage(),
                // `ui-snapshot [folder]`: the PNG pictures of the editors
                _ => not_implemented(
                    "ui-snapshot [folder]",
                    "the pictures of the editors come from the Rust UI (R5); use the C++ ui-snapshot for them",
                ),
            }
        }
    }
}

#[cfg(test)]
mod tests {
    use super::int_value;

    #[test]
    fn numbers_like_juce() {
        assert_eq!(int_value("20"), 20);
        assert_eq!(int_value(" 14 "), 14);
        assert_eq!(int_value("3x"), 3);
        assert_eq!(int_value("x3"), 0);
        assert_eq!(int_value("-5"), -5);
        assert_eq!(int_value(""), 0);
    }
}
