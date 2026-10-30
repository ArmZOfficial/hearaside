//! The lines a check prints: `ok   <what>` / `FAIL <what>` (the format the C++ `expect` used), plus free-text lines.
//! Checks run client threads beside the message thread, so a `Report` is shared (`&Report`, interior mutability).
use std::io::Write;
use std::sync::Mutex;
use std::sync::atomic::{AtomicUsize, Ordering};

pub struct Report {
    echo: bool,
    lines: Mutex<Vec<String>>,
    failures: AtomicUsize,
}

impl Report {
    /// `echo`: also print every line to stdout now (the command line); tests keep them quiet.
    pub fn new(echo: bool) -> Report {
        Report {
            echo,
            lines: Mutex::new(Vec::new()),
            failures: AtomicUsize::new(0),
        }
    }

    /// One line of output as it is.
    pub fn line(&self, text: impl Into<String>) {
        let text = text.into();
        if self.echo {
            let mut out = std::io::stdout().lock();
            let _ = writeln!(out, "{text}");
            let _ = out.flush();
        }
        self.lines
            .lock()
            .unwrap_or_else(|e| e.into_inner())
            .push(text);
    }

    /// A check: `ok   what` or `FAIL what`. Returns `ok`.
    pub fn expect(&self, what: &str, ok: bool) -> bool {
        if !ok {
            self.failures.fetch_add(1, Ordering::SeqCst);
        }
        self.line(format!("{} {what}", if ok { "ok  " } else { "FAIL" }));
        ok
    }

    pub fn failures(&self) -> usize {
        self.failures.load(Ordering::SeqCst)
    }

    pub fn lines(&self) -> Vec<String> {
        self.lines.lock().unwrap_or_else(|e| e.into_inner()).clone()
    }

    /// The summary line (`<name>: all passed` / `<name>: FAILED`), the last line of a check; returns the exit code.
    pub fn finish(&self, name: &str) -> i32 {
        let ok = self.failures() == 0;
        self.line(format!(
            "{name}: {}",
            if ok { "all passed" } else { "FAILED" }
        ));
        i32::from(!ok)
    }
}
