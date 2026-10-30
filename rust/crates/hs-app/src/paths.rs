//! Where the C++ plug-ins keep their files, per operating system, as pure functions of the environment so each OS can be
//! tested on any host. Two different JUCE rules are involved and they do not agree on macOS and Linux:
//! * `juce::File::userApplicationDataDirectory` (what `AccountManager` uses for `account.dat`): `%APPDATA%` on Windows,
//!   `~/Library` on macOS, `$XDG_CONFIG_HOME` or `~/.config` on Linux;
//! * `juce::PropertiesFile::Options::getDefaultFile` with a `folderName` (what `Settings` uses): `%APPDATA%\HEARASIDE` on
//!   Windows, `~/Library/Application Support/HEARASIDE` on macOS, `~/HEARASIDE` on Linux.
use std::path::PathBuf;

#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub enum Os {
    Windows,
    Mac,
    Linux,
}

impl Os {
    pub fn current() -> Os {
        if cfg!(windows) {
            Os::Windows
        } else if cfg!(target_os = "macos") {
            Os::Mac
        } else {
            Os::Linux
        }
    }
}

/// The environment the rules read; every value empty or missing counts as not set.
#[derive(Clone, Debug, Default)]
pub struct PathEnv {
    /// `$HOME`, or `%USERPROFILE%` on Windows
    pub home: Option<String>,
    /// `%APPDATA%`
    pub appdata: Option<String>,
    /// `$XDG_CONFIG_HOME`
    pub xdg_config_home: Option<String>,
}

impl PathEnv {
    pub fn from_process() -> PathEnv {
        let get = |n: &str| std::env::var(n).ok().filter(|v| !v.is_empty());
        PathEnv {
            home: get("HOME").or_else(|| get("USERPROFILE")),
            appdata: get("APPDATA"),
            xdg_config_home: get("XDG_CONFIG_HOME"),
        }
    }
}

fn some(v: &Option<String>) -> Option<&str> {
    v.as_deref().filter(|s| !s.is_empty())
}

/// `juce::File::userApplicationDataDirectory`.
pub fn user_app_data_dir_for(os: Os, env: &PathEnv) -> Option<PathBuf> {
    match os {
        Os::Windows => some(&env.appdata)
            .map(PathBuf::from)
            .or_else(|| some(&env.home).map(|h| PathBuf::from(h).join("AppData").join("Roaming"))),
        Os::Mac => some(&env.home).map(|h| PathBuf::from(h).join("Library")),
        Os::Linux => some(&env.xdg_config_home)
            .map(PathBuf::from)
            .or_else(|| some(&env.home).map(|h| PathBuf::from(h).join(".config"))),
    }
}

/// `HEARASIDE/<name>.dat` in the user application data directory (`AccountManager::storageFile`).
pub fn account_file_for(os: Os, env: &PathEnv, name: &str) -> Option<PathBuf> {
    Some(
        user_app_data_dir_for(os, env)?
            .join("HEARASIDE")
            .join(format!("{name}.dat")),
    )
}

/// `HEARASIDE/<name>.xml` where `juce::PropertiesFile` puts the settings (`Settings.cpp` `fileOptions`).
pub fn settings_file_for(os: Os, env: &PathEnv, name: &str) -> Option<PathBuf> {
    let base = match os {
        Os::Windows => user_app_data_dir_for(os, env)?,
        Os::Mac => PathBuf::from(some(&env.home)?)
            .join("Library")
            .join("Application Support"),
        Os::Linux => PathBuf::from(some(&env.home)?),
    };
    Some(base.join("HEARASIDE").join(format!("{name}.xml")))
}

/// `juce::File::userDocumentsDirectory`: the real Documents folder (Windows: the known folder, which OneDrive may have
/// moved; elsewhere `~/Documents`).
pub fn documents_dir() -> Option<PathBuf> {
    #[cfg(windows)]
    {
        win::documents_dir()
    }
    #[cfg(not(windows))]
    {
        PathEnv::from_process()
            .home
            .filter(|h| !h.is_empty())
            .map(|h| PathBuf::from(h).join("Documents"))
    }
}

/// A calendar date and time of day.
#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub struct DateTime {
    pub year: i32,
    pub month: u32,
    pub day: u32,
    pub hour: u32,
    pub minute: u32,
    pub second: u32,
}

impl DateTime {
    /// `2026-10-31`
    pub fn date_text(&self) -> String {
        format!("{:04}-{:02}-{:02}", self.year, self.month, self.day)
    }

    /// `12:00:00`
    pub fn time_text(&self) -> String {
        format!("{:02}:{:02}:{:02}", self.hour, self.minute, self.second)
    }
}

/// UTC calendar time of `secs` since 1970 (days-to-civil, H. Hinnant).
pub fn utc_from_unix(secs: i64) -> DateTime {
    let (days, rem) = (secs.div_euclid(86_400), secs.rem_euclid(86_400));
    let z = days + 719_468;
    let era = z.div_euclid(146_097);
    let doe = z.rem_euclid(146_097);
    let yoe = (doe - doe / 1460 + doe / 36_524 - doe / 146_096) / 365;
    let doy = doe - (365 * yoe + yoe / 4 - yoe / 100);
    let mp = (5 * doy + 2) / 153;
    let month = if mp < 10 { mp + 3 } else { mp - 9 };
    DateTime {
        year: (yoe + era * 400 + i64::from(month <= 2)) as i32,
        month: month as u32,
        day: (doy - (153 * mp + 2) / 5 + 1) as u32,
        hour: (rem / 3600) as u32,
        minute: (rem % 3600 / 60) as u32,
        second: (rem % 60) as u32,
    }
}

/// The time on this computer's clocks (its time zone and daylight saving) at `secs` since 1970, like
/// `juce::Time::formatted`. `None` when the system cannot say.
pub fn local_time(secs: i64) -> Option<DateTime> {
    #[cfg(windows)]
    {
        win::local_time(secs)
    }
    #[cfg(unix)]
    {
        let t = libc::time_t::try_from(secs).ok()?;
        // SAFETY: `tm` is plain data that `localtime_r` fills; `t` is a valid time_t on our stack.
        let tm = unsafe {
            let mut tm: libc::tm = std::mem::zeroed();
            if libc::localtime_r(&t, &mut tm).is_null() {
                return None;
            }
            tm
        };
        Some(DateTime {
            year: tm.tm_year + 1900,
            month: u32::try_from(tm.tm_mon + 1).ok()?,
            day: u32::try_from(tm.tm_mday).ok()?,
            hour: u32::try_from(tm.tm_hour).ok()?,
            minute: u32::try_from(tm.tm_min).ok()?,
            second: u32::try_from(tm.tm_sec).ok()?,
        })
    }
}

#[cfg(windows)]
mod win {
    use super::{DateTime, utc_from_unix};
    use std::os::windows::ffi::OsStringExt;
    use std::path::PathBuf;
    use windows_sys::Win32::Foundation::SYSTEMTIME;
    use windows_sys::Win32::System::Com::CoTaskMemFree;
    use windows_sys::Win32::System::Time::SystemTimeToTzSpecificLocalTime;
    use windows_sys::Win32::UI::Shell::{FOLDERID_Documents, SHGetKnownFolderPath};

    pub(super) fn documents_dir() -> Option<PathBuf> {
        let mut raw: *mut u16 = std::ptr::null_mut();
        // SAFETY: `FOLDERID_Documents` is a valid KNOWNFOLDERID, the token is null (this user) and `raw` is a valid output.
        let hr =
            unsafe { SHGetKnownFolderPath(&FOLDERID_Documents, 0, std::ptr::null_mut(), &mut raw) };
        if raw.is_null() {
            return None;
        }
        let path = (hr >= 0).then(|| {
            // SAFETY: on success `raw` is a NUL-terminated UTF-16 string, valid until `CoTaskMemFree`.
            let wide = unsafe {
                let mut len = 0;
                while *raw.add(len) != 0 {
                    len += 1;
                }
                std::slice::from_raw_parts(raw, len)
            };
            PathBuf::from(std::ffi::OsString::from_wide(wide))
        });
        // SAFETY: `raw` was allocated by the shell with CoTaskMemAlloc (the API sets it even when it fails) and is not used again.
        unsafe { CoTaskMemFree(raw.cast()) };
        path
    }

    pub(super) fn local_time(secs: i64) -> Option<DateTime> {
        let u = utc_from_unix(secs);
        let utc = SYSTEMTIME {
            wYear: u16::try_from(u.year).ok()?,
            wMonth: u.month as u16,
            wDayOfWeek: 0,
            wDay: u.day as u16,
            wHour: u.hour as u16,
            wMinute: u.minute as u16,
            wSecond: u.second as u16,
            wMilliseconds: 0,
        };
        let mut local = utc;
        // SAFETY: both pointers refer to valid SYSTEMTIMEs on our stack; a null zone means the current time zone.
        let ok = unsafe { SystemTimeToTzSpecificLocalTime(std::ptr::null(), &utc, &mut local) };
        (ok != 0).then(|| DateTime {
            year: i32::from(local.wYear),
            month: u32::from(local.wMonth),
            day: u32::from(local.wDay),
            hour: u32::from(local.wHour),
            minute: u32::from(local.wMinute),
            second: u32::from(local.wSecond),
        })
    }
}

#[cfg(test)]
mod tests {
    use super::*;
    use std::path::Path;

    fn env(home: Option<&str>, appdata: Option<&str>, xdg: Option<&str>) -> PathEnv {
        PathEnv {
            home: home.map(str::to_owned),
            appdata: appdata.map(str::to_owned),
            xdg_config_home: xdg.map(str::to_owned),
        }
    }

    #[test]
    fn utc_dates() {
        let d = utc_from_unix(1_793_448_000);
        assert_eq!(
            (d.year, d.month, d.day, d.hour, d.minute, d.second),
            (2026, 10, 31, 12, 0, 0)
        );
        assert_eq!(d.date_text(), "2026-10-31");
        assert_eq!(d.time_text(), "12:00:00");
        assert_eq!(utc_from_unix(0).date_text(), "1970-01-01");
        assert_eq!(utc_from_unix(-1).time_text(), "23:59:59");
        assert_eq!(utc_from_unix(951_782_400).date_text(), "2000-02-29");
    }

    #[test]
    fn local_time_is_the_utc_time_moved_by_the_zone() {
        // whatever zone this computer is in, local = utc within the range of real offsets
        let secs = 1_793_448_000;
        let (l, u) = (local_time(secs).unwrap(), utc_from_unix(secs));
        let as_secs = |d: DateTime| {
            i64::from(d.day) * 86_400
                + i64::from(d.hour) * 3600
                + i64::from(d.minute) * 60
                + i64::from(d.second)
        };
        let diff = as_secs(l) - as_secs(u);
        let diff = if diff.abs() > 20 * 86_400 { 0 } else { diff }; // a month border
        assert!((-14 * 3600..=14 * 3600).contains(&diff), "{l:?} {u:?}");
        assert_eq!(l.year, 2026);
    }

    #[test]
    fn documents_folder_exists() {
        let d = documents_dir().expect("a Documents folder");
        assert!(d.is_absolute());
    }

    #[test]
    fn windows_paths() {
        let e = env(
            Some("C:/Users/mint"),
            Some("C:/Users/mint/AppData/Roaming"),
            None,
        );
        assert_eq!(
            account_file_for(Os::Windows, &e, "account").unwrap(),
            Path::new("C:/Users/mint/AppData/Roaming/HEARASIDE/account.dat")
        );
        assert_eq!(
            settings_file_for(Os::Windows, &e, "settings").unwrap(),
            Path::new("C:/Users/mint/AppData/Roaming/HEARASIDE/settings.xml")
        );
        let no_appdata = env(Some("C:/Users/mint"), Some(""), None);
        assert_eq!(
            user_app_data_dir_for(Os::Windows, &no_appdata).unwrap(),
            Path::new("C:/Users/mint/AppData/Roaming")
        );
        assert!(user_app_data_dir_for(Os::Windows, &env(None, None, None)).is_none());
    }

    #[test]
    fn mac_account_is_in_library_but_settings_in_application_support() {
        // juce_Files_mac.mm: userApplicationDataDirectory = "~/Library"; PropertiesFile: ~/Library/Application Support/<folder>
        let e = env(Some("/Users/mint"), None, Some("/ignored/on/mac"));
        assert_eq!(
            account_file_for(Os::Mac, &e, "account").unwrap(),
            Path::new("/Users/mint/Library/HEARASIDE/account.dat")
        );
        assert_eq!(
            settings_file_for(Os::Mac, &e, "settings").unwrap(),
            Path::new("/Users/mint/Library/Application Support/HEARASIDE/settings.xml")
        );
    }

    #[test]
    fn linux_account_honours_xdg_config_home_but_settings_do_not() {
        // juce_Files_linux.cpp: resolveXDGFolder("XDG_CONFIG_HOME", "~/.config"); PropertiesFile: ~/<folder>
        let plain = env(Some("/home/mint"), None, None);
        assert_eq!(
            account_file_for(Os::Linux, &plain, "account").unwrap(),
            Path::new("/home/mint/.config/HEARASIDE/account.dat")
        );
        assert_eq!(
            settings_file_for(Os::Linux, &plain, "settings").unwrap(),
            Path::new("/home/mint/HEARASIDE/settings.xml")
        );
        let xdg = env(Some("/home/mint"), None, Some("/data/cfg"));
        assert_eq!(
            account_file_for(Os::Linux, &xdg, "account").unwrap(),
            Path::new("/data/cfg/HEARASIDE/account.dat")
        );
        assert_eq!(
            settings_file_for(Os::Linux, &xdg, "settings").unwrap(),
            Path::new("/home/mint/HEARASIDE/settings.xml")
        );
        let empty_xdg = env(Some("/home/mint"), None, Some(""));
        assert_eq!(
            account_file_for(Os::Linux, &empty_xdg, "x").unwrap(),
            Path::new("/home/mint/.config/HEARASIDE/x.dat")
        );
        assert!(account_file_for(Os::Linux, &env(None, None, None), "account").is_none());
    }

    #[test]
    fn this_host_follows_its_own_rule() {
        let e = PathEnv::from_process();
        let acc = account_file_for(Os::current(), &e, "account");
        if cfg!(target_os = "macos") {
            assert!(acc.unwrap().ends_with("Library/HEARASIDE/account.dat"));
        } else if let Some(p) = acc {
            assert!(p.ends_with("HEARASIDE/account.dat"));
        }
    }
}
