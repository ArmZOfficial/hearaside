//! UI size and look (plugins/common/ui/EditorShell.h, Settings): the 100-200 % scale, the first-run pick that fits the
//! screen, and the resolved theme / reduce-motion switches that go into `Palette.dark` and `Motion.reduce`.
use crate::theme::layout::SCREEN_FIT;
use hs_app::{Settings, ThemeMode};

pub const MIN: f32 = 1.0;
pub const MAX: f32 = 2.0;

/// The UI size setting limited to 100-200 % (`jlimit(1, 2)`); a NaN is 100 %.
pub fn clamp(ui_scale: f32) -> f32 {
    if ui_scale.is_nan() {
        MIN
    } else {
        ui_scale.clamp(MIN, MAX)
    }
}

/// Window scale factor for Slint: the display's scale times the UI size.
pub fn window_scale(display_scale: f32, ui_scale: f32) -> f32 {
    display_scale * clamp(ui_scale)
}

/// First run, nothing chosen yet: the biggest UI size whose default window still fits the screen.
pub fn auto_pick(base_w: f32, base_h: f32, screen_w: f32, screen_h: f32) -> f32 {
    [2.0, 1.75, 1.5, 1.25]
        .into_iter()
        .find(|s| base_w * s <= screen_w * SCREEN_FIT && base_h * s <= screen_h * SCREEN_FIT)
        .unwrap_or(MIN)
}

/// Window size in pixels for content of `w` x `h` units (`setSize(round(size * s))`).
pub fn window_size(w: i32, h: i32, ui_scale: f32) -> (i32, i32) {
    let s = clamp(ui_scale);
    ((w as f32 * s).round() as i32, (h as f32 * s).round() as i32)
}

/// Content size in units for a window of `w` x `h` pixels (`contentSize`).
pub fn content_size(w: i32, h: i32, ui_scale: f32) -> (i32, i32) {
    let s = clamp(ui_scale);
    ((w as f32 / s).round() as i32, (h as f32 / s).round() as i32)
}

/// What the Settings say about the look, resolved: set `Palette.dark`, `Motion.reduce` and the window scale from it.
#[derive(Clone, Copy, Debug, PartialEq)]
pub struct Look {
    pub dark: bool,
    pub ui_scale: f32,
    pub reduce_motion: bool,
}

impl Look {
    /// `os_dark`: whether the system is in dark mode, used when the theme is Auto (`Settings::isDark`).
    pub fn from_settings(s: &Settings, os_dark: bool) -> Self {
        let dark = match s.theme_mode() {
            ThemeMode::Light => false,
            ThemeMode::Dark => true,
            ThemeMode::Auto => os_dark,
        };
        Self {
            dark,
            ui_scale: s.ui_scale(),
            reduce_motion: s.reduce_motion(),
        }
    }
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn scale_is_limited_to_100_200_percent() {
        assert_eq!(clamp(0.5), 1.0);
        assert_eq!(clamp(1.5), 1.5);
        assert_eq!(clamp(3.0), 2.0);
        assert_eq!(clamp(f32::NAN), 1.0);
        assert_eq!(window_scale(1.25, 1.5), 1.875);
        assert_eq!(window_scale(2.0, 9.0), 4.0);
    }

    #[test]
    fn first_run_picks_the_biggest_size_that_fits() {
        // a 380 x 640 editor on 1920 x 1080: 0.85 * 1080 = 918, 640 * 1.25 = 800 fits, 640 * 1.5 = 960 does not
        assert_eq!(auto_pick(380.0, 640.0, 1920.0, 1080.0), 1.25);
        assert_eq!(auto_pick(380.0, 640.0, 3840.0, 2160.0), 2.0);
        assert_eq!(auto_pick(380.0, 640.0, 1366.0, 728.0), 1.0);
        assert_eq!(auto_pick(380.0, 600.0, 1920.0, 1080.0), 1.5);
    }

    #[test]
    fn window_and_content_sizes_round_trip() {
        assert_eq!(window_size(380, 640, 1.25), (475, 800));
        assert_eq!(content_size(475, 800, 1.25), (380, 640));
        assert_eq!(content_size(477, 801, 1.5), (318, 534));
    }

    #[test]
    fn look_follows_the_settings_and_the_system_when_auto() {
        let dir = std::env::temp_dir().join(format!("hs-ui-look-{}", std::process::id()));
        std::fs::create_dir_all(&dir).unwrap();
        let mut s = Settings::open(dir.join("settings.xml"));
        let auto = Look::from_settings(&s, true);
        assert!(auto.dark && !Look::from_settings(&s, false).dark);
        s.set_theme_mode(ThemeMode::Light);
        s.set_ui_scale(1.75);
        s.set_reduce_motion(true);
        let l = Look::from_settings(&s, true);
        assert_eq!(
            l,
            Look {
                dark: false,
                ui_scale: 1.75,
                reduce_motion: true
            }
        );
        s.set_theme_mode(ThemeMode::Dark);
        assert!(Look::from_settings(&s, false).dark);
        let _ = std::fs::remove_dir_all(dir);
    }
}
