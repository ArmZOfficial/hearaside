//! R0.3 spike: the Track editor main page (plugins/track/TrackEditor.cpp layout) in Vizia.
//! Vizia needs a real window, so `capture.sh` runs it under Xvfb.
//!   HS_THEME=dark WINIT_X11_SCALE_FACTOR=2.5 cargo run --release
//! The paper/grid/blob layer comes from the same code as the Slint spike; the glass is Vizia's own
//! `backdrop-filter: blur()` instead of a pre-rendered copy.
#[path = "../../ui-slint/src/backdrop.rs"]
mod backdrop;
#[path = "../../ui-slint/src/tokens.rs"]
mod tokens;

mod page;
use vizia; // page.rs names it crate::vizia, as the plug-in spike re-exports it

use std::rc::Rc;
use vizia::prelude::*;

fn main() -> Result<(), ApplicationError> {
    let dark = std::env::var("HS_THEME").as_deref() == Ok("dark");
    let scale: f32 = std::env::var("WINIT_X11_SCALE_FACTOR").ok().and_then(|s| s.parse().ok()).unwrap_or(1.0);
    let started = std::time::Instant::now();
    Application::new(move |cx| {
        page::install(cx, scale, dark);
        let footer = Signal::new(String::from("บัฟเฟอร์ DAW 256 sample = 5.3 ms · 48.0 kHz"));
        page::page(cx, dark, Signal::new(false), Signal::new(true), footer, Rc::new(|_, _| {}), Rc::new(|_, _| {}));
        eprintln!("built in {:.1} ms", started.elapsed().as_secs_f64() * 1000.0);
    })
    .title("HEARASIDE R0.3 Vizia")
    .inner_size((page::W as u32, page::H as u32))
    .run()
}
