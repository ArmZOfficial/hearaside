//! The cloudflared quick tunnel: finding the program, starting it so that it dies with the DAW, reading its output.
//! Port of `ShareServer::Tunnelled`, `findCloudflared` and `tunnelLoop`; the arguments are fixed by
//! `docs/cloudflared-protocol.md` (`tunnel --no-autoupdate --url http://127.0.0.1:<port>`, protocol auto).
use std::io;
use std::path::{Path, PathBuf};
use std::process::{Child, Command, Stdio};
use std::sync::Mutex;

#[cfg(windows)]
const EXE_NAME: &str = "cloudflared.exe";
#[cfg(not(windows))]
const EXE_NAME: &str = "cloudflared";

/// Where `cloudflared` is searched, in this order: the copy the installer puts next to the plug-ins (links work with
/// nothing else installed), the PATH, then the usual install places.
pub(crate) fn default_search_dirs() -> Vec<PathBuf> {
    let env = |name: &str| std::env::var_os(name).filter(|v| !v.is_empty());
    let mut dirs = Vec::new();
    #[cfg(windows)]
    {
        for v in ["ProgramFiles", "ProgramW6432"] {
            if let Some(p) = env(v) {
                dirs.push(PathBuf::from(p).join("HEARASIDE").join("cloudflared"));
            }
        }
    }
    if let Some(path) = env("PATH") {
        dirs.extend(std::env::split_paths(&path));
    }
    #[cfg(windows)]
    {
        for v in ["ProgramFiles", "ProgramFiles(x86)"] {
            if let Some(p) = env(v) {
                dirs.push(PathBuf::from(p).join("cloudflared"));
            }
        }
        if let Some(local) = env("LOCALAPPDATA") {
            let local = PathBuf::from(local);
            dirs.push(local.join("Microsoft").join("WinGet").join("Links"));
            dirs.push(local.join("HEARASIDE"));
        }
    }
    dirs
}

/// The first `cloudflared` file in `dirs`. Empty and relative entries (".") are skipped: they would run whatever
/// sits in the DAW's current folder.
pub(crate) fn find_in(dirs: &[PathBuf]) -> Option<PathBuf> {
    dirs.iter()
        .filter(|d| d.is_absolute())
        .map(|d| d.join(EXE_NAME))
        .find(|f| f.is_file())
}

/// The cloudflared process. On Windows it is in a job object that is closed (and the process killed) when the DAW's
/// process ends for any reason, so a crashed DAW never leaves a tunnel running.
pub(crate) struct Tunnelled {
    child: Mutex<Child>,
    #[cfg(windows)]
    _job: job::Job,
}

impl Tunnelled {
    /// Starts it with stdout and stderr on one pipe (the URL is on stderr); returns the process and the pipe.
    pub(crate) fn start(exe: &Path, port: u16) -> io::Result<(Self, io::PipeReader)> {
        #[cfg(windows)]
        let job = job::Job::new()?;
        let (reader, writer) = io::pipe()?;
        let mut cmd = Command::new(exe);
        cmd.args(["tunnel", "--no-autoupdate", "--url"])
            .arg(format!("http://127.0.0.1:{port}"))
            .stdin(Stdio::null())
            .stdout(writer.try_clone()?)
            .stderr(writer);
        #[cfg(windows)]
        {
            use std::os::windows::process::CommandExt;
            cmd.creation_flags(0x0800_0000); // CREATE_NO_WINDOW
        }
        let child = cmd.spawn()?;
        drop(cmd); // our copies of the write end: the pipe ends when the process does
        #[cfg(windows)]
        job.assign(&child);
        Ok((
            Tunnelled {
                child: Mutex::new(child),
                #[cfg(windows)]
                _job: job,
            },
            reader,
        ))
    }

    pub(crate) fn kill(&self) {
        let mut c = self
            .child
            .lock()
            .unwrap_or_else(std::sync::PoisonError::into_inner);
        let _ = c.kill();
    }
}

impl Drop for Tunnelled {
    fn drop(&mut self) {
        let c = self
            .child
            .get_mut()
            .unwrap_or_else(std::sync::PoisonError::into_inner);
        let _ = c.kill();
        let _ = c.wait(); // no zombie
    }
}

#[cfg(windows)]
mod job {
    use std::io;
    use std::os::windows::io::AsRawHandle;
    use std::process::Child;
    use windows_sys::Win32::Foundation::{CloseHandle, HANDLE};
    use windows_sys::Win32::System::JobObjects::{
        AssignProcessToJobObject, CreateJobObjectW, JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE,
        JOBOBJECT_EXTENDED_LIMIT_INFORMATION, JobObjectExtendedLimitInformation,
        SetInformationJobObject,
    };

    pub(super) struct Job(HANDLE);

    // SAFETY: a job object handle is a plain kernel handle usable from any thread.
    unsafe impl Send for Job {}
    // SAFETY: the handle is only passed to thread-safe kernel calls and closed once on drop.
    unsafe impl Sync for Job {}

    impl Job {
        pub(super) fn new() -> io::Result<Self> {
            // SAFETY: null security attributes and name are allowed; the result is checked.
            let h = unsafe { CreateJobObjectW(std::ptr::null(), std::ptr::null()) };
            if h.is_null() {
                return Err(io::Error::last_os_error());
            }
            let job = Job(h);
            // SAFETY: an all-zero JOBOBJECT_EXTENDED_LIMIT_INFORMATION is the documented "no limits" value.
            let mut lim: JOBOBJECT_EXTENDED_LIMIT_INFORMATION = unsafe { std::mem::zeroed() };
            lim.BasicLimitInformation.LimitFlags = JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE;
            // SAFETY: `h` is the live job; the pointer and size describe `lim`.
            let ok = unsafe {
                SetInformationJobObject(
                    h,
                    JobObjectExtendedLimitInformation,
                    (&raw const lim).cast(),
                    size_of::<JOBOBJECT_EXTENDED_LIMIT_INFORMATION>() as u32,
                )
            };
            if ok == 0 {
                return Err(io::Error::last_os_error());
            }
            Ok(job)
        }

        /// A failure is not fatal (the DAW may itself sit in a job that forbids it): the process is still killed by
        /// `stop()`.
        pub(super) fn assign(&self, child: &Child) {
            // SAFETY: both handles are live: the job is ours, the process handle belongs to `child`.
            unsafe { AssignProcessToJobObject(self.0, child.as_raw_handle()) };
        }
    }

    impl Drop for Job {
        fn drop(&mut self) {
            // SAFETY: the handle came from CreateJobObjectW and is closed once; closing kills the job's processes.
            unsafe { CloseHandle(self.0) };
        }
    }
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn search_takes_the_first_folder_with_the_program_and_skips_relative_ones() {
        let dir = std::env::temp_dir().join(format!("hs-find-cf-{}", std::process::id()));
        let other = dir.join("other");
        std::fs::create_dir_all(&other).unwrap();
        std::fs::write(dir.join(EXE_NAME), b"x").unwrap();
        std::fs::write(other.join(EXE_NAME), b"y").unwrap();
        let found = find_in(&[
            PathBuf::new(),
            PathBuf::from("."),
            dir.join("missing"),
            dir.clone(),
            other.clone(),
        ]);
        assert_eq!(found, Some(dir.join(EXE_NAME)));
        assert_eq!(find_in(&[PathBuf::from("."), PathBuf::new()]), None);
        // a folder named like the program is not the program
        std::fs::create_dir_all(other.join("sub").join(EXE_NAME)).unwrap();
        assert_eq!(find_in(&[other.join("sub")]), None);
        std::fs::remove_dir_all(&dir).ok();
    }

    #[cfg(windows)]
    #[test]
    fn default_search_order_starts_with_the_installer_copy() {
        let dirs = default_search_dirs();
        if std::env::var_os("ProgramFiles").is_some() {
            assert!(dirs[0].ends_with(Path::new("HEARASIDE").join("cloudflared")));
        }
        assert!(dirs.iter().all(|d| !d.as_os_str().is_empty()));
    }
}
