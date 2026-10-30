//! Per-user UI preferences (`plugins/common/Settings.{h,cpp}`): language, theme, size, motion, glass opacity, share base,
//! REST API switch, window sizes and the other one-off facts. Same file as the C++ plug-ins read and write:
//! `HEARASIDE/settings.xml` in the user's app-data folder (a `juce::PropertiesFile`, see `properties.rs`), same keys,
//! same defaults and the same clamps, so both versions can run side by side.
//!
//! No UI here: the C++ `ChangeBroadcaster` is `Settings::set_on_change`, the `Settings::appearanceChanged` hook is
//! `set_appearance_hook`. Both run on the thread that called the setter.
//!
//! Saving: a change is kept in memory and written later, like the C++ (`millisecondsBeforeSaving` 500). The owner calls
//! `tick()` from a timer of its own (a few times a second); `flush()` writes at once and runs when the `Settings` is dropped.
//! A hundred changes in a burst are one write. A setter writes only the keys it owns, onto the file as it is by then, so
//! two processes do not undo each other's settings; the values read back after a write include the other process's.
use crate::paths::{Os, PathEnv, settings_file_for, user_app_data_dir_for};
use crate::properties::PropertiesFile;
use crate::strings::{Language, set_current_language};
use std::path::{Path, PathBuf};

#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub enum ThemeMode {
    Auto,
    Light,
    Dark,
}

/// The look the account syncs (`web/site/shared/schemas.ts` `syncedSettings.appearance`). A field that is `None` is not
/// part of it (the website may have only some).
#[derive(Clone, Debug, Default, PartialEq)]
pub struct Appearance {
    pub language: Option<Language>,
    pub theme: Option<ThemeMode>,
    pub ui_scale: Option<f64>,
    pub glass_alpha: Option<f64>,
    pub host_colours: Option<bool>,
    pub reduce_motion: Option<bool>,
}

type Hook = Box<dyn Fn(&Appearance) + Send>;

/// The values are read from the file's key/value set each time (never cached), so a write that merged another process's
/// changes is visible at once.
pub struct Settings {
    file: PropertiesFile,
    default_share_base: String,
    on_change: Option<Box<dyn Fn() + Send>>,
    appearance_hook: Option<Hook>,
    silent: bool,
}

/// `juce::File::userApplicationDataDirectory` of this machine: `%APPDATA%`, `~/Library`, `$XDG_CONFIG_HOME` or `~/.config`.
pub fn user_app_data_dir() -> Option<PathBuf> {
    user_app_data_dir_for(Os::current(), &PathEnv::from_process())
}

/// `$name` trimmed, or `default` (the C++ `getEnv`).
pub(crate) fn env_or(name: &str, default: &str) -> String {
    match std::env::var(name) {
        Ok(v) if !v.trim().is_empty() => v.trim().to_owned(),
        _ => default.to_owned(),
    }
}

/// Where `juce::PropertiesFile` puts the settings (see `paths.rs`). `name` is `settings` unless `$HEARASIDE_SETTINGS_NAME`
/// is set (tools keep their own file, never the user's).
pub fn default_settings_path() -> Option<PathBuf> {
    settings_file_for(
        Os::current(),
        &PathEnv::from_process(),
        &env_or("HEARASIDE_SETTINGS_NAME", "settings"),
    )
}

/// Bus used by new instances: "Main", or `$HEARASIDE_DEFAULT_BUS` (tests, a second DAW session).
pub fn default_bus_name() -> String {
    env_or("HEARASIDE_DEFAULT_BUS", "Main")
}

fn clamp_f32(v: f32, lo: f32, hi: f32, nan_default: f32) -> f32 {
    if v.is_nan() {
        nan_default
    } else {
        v.clamp(lo, hi)
    }
}

impl Settings {
    /// The user's settings file (`default_settings_path`).
    pub fn open_default() -> Self {
        Self::open(default_settings_path().unwrap_or_default())
    }

    /// Settings in `path` (tests and tools). Like the C++ constructor it also makes the stored language the process's.
    pub fn open(path: impl Into<PathBuf>) -> Self {
        let s = Self {
            file: PropertiesFile::open(path),
            default_share_base: String::new(),
            on_change: None,
            appearance_hook: None,
            silent: false,
        };
        set_current_language(s.language());
        s
    }

    pub fn path(&self) -> &Path {
        self.file.path()
    }

    /// Writes now what is not on disk yet (also done when the `Settings` is dropped). `false` = could not (the file is
    /// busy or unreadable at this moment); the changes stay pending.
    pub fn flush(&mut self) -> bool {
        self.file.flush()
    }

    /// Call from a timer: writes once the oldest change is half a second old. Cheap when nothing changed.
    pub fn tick(&mut self) {
        self.file.tick();
    }

    /// One value, in memory until the next `tick` / `flush`.
    fn put(&mut self, key: &str, value: &str) {
        self.file.set(key, value);
    }

    /// `HEARASIDE_SHARE_BASE` of the C++ build: the share base used while the file has none.
    pub fn set_default_share_base(&mut self, base: &str) {
        self.default_share_base = base.to_owned();
    }

    /// Called after Language, Theme, UI size, Glass, Track colours or Reduce motion changed
    /// (`Settings::appearanceChanged`): the account syncs them when "Sync appearance" is on. Not called by
    /// `apply_appearance`, which is the account's own copy coming down.
    pub fn set_appearance_hook(&mut self, hook: Option<Hook>) {
        self.appearance_hook = hook;
    }

    /// Called after any of the six appearance values changed (the `ChangeBroadcaster` message).
    pub fn set_on_change(&mut self, f: Option<Box<dyn Fn() + Send>>) {
        self.on_change = f;
    }

    pub fn language(&self) -> Language {
        // English for new users (S6)
        if self.file.get_or("language", "en") == "th" {
            Language::Thai
        } else {
            Language::English
        }
    }
    pub fn theme_mode(&self) -> ThemeMode {
        match self.file.get_or("theme", "auto").as_str() {
            "light" => ThemeMode::Light,
            "dark" => ThemeMode::Dark,
            _ => ThemeMode::Auto,
        }
    }
    /// 1.0 ..= 2.0
    pub fn ui_scale(&self) -> f32 {
        clamp_f32(self.file.get_f64("uiScale", 1.0) as f32, 1.0, 2.0, 1.0)
    }
    pub fn reduce_motion(&self) -> bool {
        self.file.get_bool("reduceMotion", false)
    }
    /// 0.35 ..= 0.95
    pub fn glass_alpha(&self) -> f32 {
        clamp_f32(
            self.file.get_f64("glassAlpha", 0.62) as f32,
            0.35,
            0.95,
            0.62,
        )
    }
    /// track colours from the DAW
    pub fn host_colours(&self) -> bool {
        self.file.get_bool("hostColours", true)
    }
    /// UI size picked by the user (false on a fresh install: the first editor fits it to the screen).
    pub fn ui_scale_chosen(&self) -> bool {
        self.file.contains_key("uiScale") && !self.file.get_bool("uiScaleAuto", false)
    }

    /// An appearance value changed: tell the listeners (`Settings::save`).
    fn changed(&mut self) {
        if let Some(c) = &self.on_change {
            c();
        }
        if !self.silent
            && let Some(h) = &self.appearance_hook
        {
            h(&self.appearance());
        }
    }

    pub fn set_language(&mut self, l: Language) {
        if l == self.language() {
            return;
        }
        self.put("language", if l == Language::English { "en" } else { "th" });
        set_current_language(l);
        self.changed();
    }
    pub fn set_theme_mode(&mut self, m: ThemeMode) {
        if m == self.theme_mode() {
            return;
        }
        self.put(
            "theme",
            match m {
                ThemeMode::Light => "light",
                ThemeMode::Dark => "dark",
                ThemeMode::Auto => "auto",
            },
        );
        self.changed();
    }
    pub fn set_ui_scale(&mut self, s: f32) {
        let s = clamp_f32(s, 1.0, 2.0, self.ui_scale());
        let differs = s != self.ui_scale();
        self.file.set_bool("uiScaleAuto", false);
        // C++ bug: choosing the size that is already shown wrote only uiScaleAuto, so without a stored uiScale the choice
        // was forgotten at the next start
        if differs || !self.file.contains_key("uiScale") {
            self.file.set_f64("uiScale", f64::from(s));
        }
        if differs {
            self.changed();
        }
    }
    /// Like `set_ui_scale`, but still counts as not chosen.
    pub fn set_ui_scale_auto(&mut self, s: f32) {
        let s = clamp_f32(s, 1.0, 2.0, self.ui_scale());
        if s != self.ui_scale() {
            let chosen = self.ui_scale_chosen();
            self.file.set_f64("uiScale", f64::from(s));
            self.file.set_bool("uiScaleAuto", !chosen);
            self.changed();
        }
    }
    pub fn set_reduce_motion(&mut self, b: bool) {
        if b != self.reduce_motion() {
            self.file.set_bool("reduceMotion", b);
            self.changed();
        }
    }
    pub fn set_glass_alpha(&mut self, a: f32) {
        let a = clamp_f32(a, 0.35, 0.95, self.glass_alpha());
        if a != self.glass_alpha() {
            self.file.set_f64("glassAlpha", f64::from(a));
            self.changed();
        }
    }
    pub fn set_host_colours(&mut self, b: bool) {
        if b != self.host_colours() {
            self.file.set_bool("hostColours", b);
            self.changed();
        }
    }

    /// What the account syncs, from these settings (`appearanceOf`).
    pub fn appearance(&self) -> Appearance {
        Appearance {
            language: Some(self.language()),
            theme: Some(self.theme_mode()),
            ui_scale: Some(f64::from(self.ui_scale())),
            glass_alpha: Some(f64::from(self.glass_alpha())),
            host_colours: Some(self.host_colours()),
            reduce_motion: Some(self.reduce_motion()),
        }
    }

    /// The account's copy comes down (`applyAppearance`): the same setters, but the appearance hook stays quiet so it is
    /// not uploaded again as a local change.
    pub fn apply_appearance(&mut self, a: &Appearance) {
        self.silent = true;
        if let Some(l) = a.language {
            self.set_language(l);
        }
        if let Some(t) = a.theme {
            self.set_theme_mode(t);
        }
        if let Some(s) = a.ui_scale {
            self.set_ui_scale(s as f32);
        }
        if let Some(g) = a.glass_alpha {
            self.set_glass_alpha(g as f32);
        }
        if let Some(h) = a.host_colours {
            self.set_host_colours(h);
        }
        if let Some(r) = a.reduce_motion {
            self.set_reduce_motion(r);
        }
        self.silent = false;
    }

    /// The share web site (a Vercel deployment); empty = links change every time sharing starts.
    pub fn share_base(&self) -> String {
        match self.file.get("shareBase") {
            Some(v) => v.to_owned(),
            None => self.default_share_base.clone(),
        }
    }
    pub fn set_share_base(&mut self, url: &str) {
        let mut u = url.trim().trim_end_matches('/').to_owned();
        let lower = u.to_ascii_lowercase();
        if !u.is_empty() && !lower.starts_with("https://") && !lower.starts_with("http://") {
            u = format!("https://{u}");
        }
        self.put("shareBase", &u);
    }

    /// Last window size per plug-in ("hub", "track", "app") in content units; `(0, 0)` = none yet.
    pub fn window_size(&self, key: &str) -> (i32, i32) {
        let v = self.file.get_or(&format!("window.{key}"), "");
        if v.is_empty() {
            return (0, 0);
        }
        let parts: Vec<&str> = v.split('x').collect();
        if parts.len() != 2 {
            return (0, 0);
        }
        (
            crate::properties::juce_int(parts[0]),
            crate::properties::juce_int(parts[1]),
        )
    }
    /// Saved quietly (no change message). A key or size that is not positive is ignored.
    pub fn set_window_size(&mut self, key: &str, w: i32, h: i32) {
        if key.is_empty() || w <= 0 || h <= 0 {
            return;
        }
        self.put(&format!("window.{key}"), &format!("{w}x{h}"));
    }

    pub fn permanent_links(&self) -> bool {
        !self.flag("permanentLinksOff")
    }
    pub fn set_permanent_links(&mut self, on: bool) {
        self.set_flag("permanentLinksOff", !on);
    }

    /// If non-empty, web listeners must give this password.
    pub fn listen_password(&self) -> String {
        self.file.get_or("listenPassword", "")
    }
    pub fn set_listen_password(&mut self, p: &str) {
        self.put("listenPassword", p);
    }

    /// Require host confirmation in the Hub before accepting new friend audio.
    pub fn friend_approval_required(&self) -> bool {
        self.flag("friendApprovalRequired")
    }
    pub fn set_friend_approval_required(&mut self, on: bool) {
        self.set_flag("friendApprovalRequired", on);
    }

    /// REST API on 127.0.0.1 (`docs/rest-api.md`): off until switched on; the key is per user.
    pub fn rest_api(&self) -> bool {
        self.flag("restApi")
    }
    pub fn set_rest_api(&mut self, on: bool) {
        self.set_flag("restApi", on);
    }
    pub fn rest_api_key(&self) -> String {
        self.file.get_or("restApiKey", "")
    }
    pub fn set_rest_api_key(&mut self, k: &str) {
        self.put("restApiKey", k);
    }

    /// Friends slower than this are not waited for (ms, 100 ..= 1000).
    pub fn line_up_limit_ms(&self) -> i32 {
        self.file.get_int("lineUpLimitMs", 600).clamp(100, 1000)
    }
    pub fn set_line_up_limit_ms(&mut self, ms: i32) {
        self.put("lineUpLimitMs", &ms.clamp(100, 1000).to_string());
    }

    /// The output device the Hub plays the Stream Mix into (VB-Cable); empty = off.
    pub fn virtual_mic_device(&self) -> String {
        self.file.get_or("virtualMicDevice", "")
    }
    pub fn set_virtual_mic_device(&mut self, d: &str) {
        self.put("virtualMicDevice", d);
    }

    /// One-off facts about this user ("triedPreview", "startHidden"...).
    pub fn flag(&self, key: &str) -> bool {
        self.file.get_bool(&format!("flag.{key}"), false)
    }
    pub fn set_flag(&mut self, key: &str, on: bool) {
        if self.flag(key) != on {
            self.put(&format!("flag.{key}"), if on { "1" } else { "0" });
        }
    }

    /// Audio device state of the standalone app (`AudioDeviceManager` XML, kept as the text it is).
    pub fn audio_device_state(&self, key: &str) -> String {
        self.file.get_or(&format!("audioDeviceState.{key}"), "")
    }
    pub fn set_audio_device_state(&mut self, key: &str, state_xml: &str) {
        self.put(&format!("audioDeviceState.{key}"), state_xml);
    }

    /// Any key of the file, for what this crate has no typed getter for (a C++ build may add some).
    pub fn raw(&self) -> &PropertiesFile {
        &self.file
    }
    pub fn raw_mut(&mut self) -> &mut PropertiesFile {
        &mut self.file
    }
}
