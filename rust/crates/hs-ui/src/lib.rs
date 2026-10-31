//! `hs-ui`: the HEARASIDE look in Slint (R5.1). Port of `plugins/common/ui` of the C++ plug-ins: theme tokens, Anuphan, the
//! 100-200 % UI size, reduced motion, controls, overlay (menus, popovers, toasts), scroll area, icons, backdrop and QR view.
//!
//! * `ui/*.slint`: the components. `theme.slint` and `src/theme.rs` are generated from `design/tokens.json` by
//!   `design/gen_theme.py`. A window imports them with a library path to [`UI_DIR`].
//! * Rust: what is not drawing: geometry ([`overlay`]), value parsing ([`controls`]), the painted [`backdrop`], the [`qr`]
//!   matrix, the UI size ([`scale`]), and the software [`headless`] renderer (ui-snapshot, tests).
//!
//! UI thread only. Nothing here runs on an audio thread; no `unsafe`.

pub mod backdrop;
pub mod controls;
pub mod gallery;
pub mod headless;
pub mod overlay;
pub mod qr;
pub mod scale;
pub mod theme;

/// The `.slint` sources of this crate (import `index.slint`-style files from here with a Slint library path).
pub const UI_DIR: &str = concat!(env!("CARGO_MANIFEST_DIR"), "/ui");

/// A rectangle in logical pixels.
#[derive(Clone, Copy, Debug, PartialEq)]
pub struct Rect {
    pub x: f32,
    pub y: f32,
    pub w: f32,
    pub h: f32,
}

impl Rect {
    pub const fn new(x: f32, y: f32, w: f32, h: f32) -> Self {
        Self { x, y, w, h }
    }
    pub fn right(&self) -> f32 {
        self.x + self.w
    }
    pub fn bottom(&self) -> f32 {
        self.y + self.h
    }
}

/// The types Slint generated from `ui/gallery.slint` (the sample page and the components it exports).
#[allow(clippy::all, clippy::pedantic, missing_docs)]
pub mod ui {
    slint::include_modules!();
}
