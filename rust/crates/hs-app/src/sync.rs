//! A panic on one of this crate's worker threads must not turn into panics in the host's threads that call into it
//! afterwards: locks are taken even when a thread panicked while holding one, and workers restart their loop.
use std::sync::{Mutex, MutexGuard, PoisonError};
use std::time::Duration;

pub(crate) trait Relock<T> {
    fn relock(&self) -> MutexGuard<'_, T>;
}

impl<T> Relock<T> for Mutex<T> {
    fn relock(&self) -> MutexGuard<'_, T> {
        self.lock().unwrap_or_else(PoisonError::into_inner)
    }
}

/// Runs `body` until it returns normally; after a panic it waits a moment and starts it again, unless `stopped()`.
pub(crate) fn keep_running(stopped: impl Fn() -> bool, body: impl Fn()) {
    while !stopped() {
        if std::panic::catch_unwind(std::panic::AssertUnwindSafe(&body)).is_ok() {
            break;
        }
        std::thread::sleep(Duration::from_millis(200));
    }
}

#[cfg(test)]
mod tests {
    use super::*;
    use std::sync::Arc;
    use std::sync::atomic::{AtomicU32, Ordering};

    #[test]
    fn a_poisoned_lock_still_opens_and_a_panicking_worker_restarts() {
        let m = Arc::new(Mutex::new(5));
        let m2 = Arc::clone(&m);
        let _ = std::thread::spawn(move || {
            let _g = m2.lock().unwrap();
            panic!("worker died holding the lock");
        })
        .join();
        assert!(m.is_poisoned());
        assert_eq!(*m.relock(), 5);

        let runs = AtomicU32::new(0);
        keep_running(
            || false,
            || {
                if runs.fetch_add(1, Ordering::SeqCst) == 0 {
                    panic!("first run fails");
                }
            },
        );
        assert_eq!(runs.load(Ordering::SeqCst), 2);
    }
}
