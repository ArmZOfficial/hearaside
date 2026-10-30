//! The self-tests behind the `ui-snapshot` flags: each `run` prints the same `ok` / `FAIL` lines and a summary line, and
//! returns the exit code (0 = all passed).
pub mod directory;
pub mod friends;
pub mod rest;
pub mod serve;
pub mod ui;

use std::time::{Duration, Instant};

/// Polls `f` every 20 ms until it is true or `ms` pass.
pub fn wait_for(ms: u64, mut f: impl FnMut() -> bool) -> bool {
    let end = Instant::now() + Duration::from_millis(ms);
    loop {
        if f() {
            return true;
        }
        if Instant::now() >= end {
            return false;
        }
        std::thread::sleep(Duration::from_millis(20));
    }
}
