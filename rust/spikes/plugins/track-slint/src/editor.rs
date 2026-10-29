//! nice-plug `Editor` for the Slint Track page, on slint-baseview (FemtoVG / OpenGL).
//! The footer shows how long the editor took to open (criterion 3).
use crate::{backdrop, tokens, track};
use nice_plug::prelude::*;
use slint::{ComponentHandle, Image, Rgba8Pixel, SharedPixelBuffer};
use slint_baseview::{SlintWindow, WindowHandle, WindowOpenOptions, WindowScalePolicy, default_gl_config};
use std::any::Any;
use std::sync::atomic::{AtomicU32, Ordering};
use std::sync::{Arc, Mutex};
use std::time::Instant;

slint::include_modules!();

const W: f32 = 380.0;
const H: f32 = 640.0;

pub struct SlintEditor {
    params: Arc<track::TrackParams>,
}

impl SlintEditor {
    pub fn new(params: Arc<track::TrackParams>) -> Self {
        Self { params }
    }
}

/// baseview's handle closes the window on drop; nice-plug drops it on the GUI thread.
struct Handle {
    _window: WindowHandle,
}
// SAFETY: nice-plug only moves the handle between threads and drops it on the host's GUI thread,
// which is the thread baseview created the window on (same as nice-plug-egui's handle).
unsafe impl Send for Handle {}

fn backdrop_image(scale: f32, dark: bool) -> Image {
    let cards = [
        backdrop::Rect { x: 12.0, y: 12.0, w: W - 24.0, h: 54.0, radius: 18.0 },
        backdrop::Rect { x: 12.0, y: 76.0, w: W - 24.0, h: H - 88.0, radius: 20.0 },
    ];
    let img = backdrop::render(W, H, scale, &tokens::colors(dark), &cards, 18.0);
    let mut buffer = SharedPixelBuffer::<Rgba8Pixel>::new(img.w as u32, img.h as u32);
    for (d, s) in buffer.make_mut_slice().iter_mut().zip(&img.px) {
        let a = s[3].max(1e-6);
        let c = |v: f32| ((v / a).clamp(0.0, 1.0) * 255.0).round() as u8;
        *d = Rgba8Pixel { r: c(s[0]), g: c(s[1]), b: c(s[2]), a: (s[3] * 255.0).round() as u8 };
    }
    Image::from_rgba8(buffer)
}

/// What the UI last showed, to tell a UI click (push to the host) from automation (pull into the UI).
#[derive(Default)]
struct Shown {
    you_hear: Option<bool>,
    viewers_hear: Option<bool>,
    dark: Option<bool>,
    scale: Option<u32>,
}

impl Editor for SlintEditor {
    fn spawn(&self, parent: ParentWindowHandle, context: Arc<dyn GuiContext>) -> Box<dyn Any + Send> {
        let started = Instant::now();
        let opened_ms = Arc::new(AtomicU32::new(0));
        let options = WindowOpenOptions {
            title: "HEARASIDE Spike Slint".into(),
            size: slint_baseview::Size::new(W as f64, H as f64),
            scale: WindowScalePolicy::SystemScaleFactor,
            // Xvfb (cloud tests) has no sRGB framebuffer; without one baseview panics and takes the host down
            gl_config: Some(slint_baseview::GlConfig {
                srgb: std::env::var_os("HS_SPIKE_NO_SRGB").is_none(),
                ..default_gl_config()
            }),
        };
        let shown = Arc::new(Mutex::new(Shown::default()));
        let (params, setup_opened) = (self.params.clone(), opened_ms.clone());
        let window = SlintWindow::open_parented(
            &parent,
            options,
            TrackSpike::new,
            move |_handler, _window| {
                setup_opened.store(started.elapsed().as_millis() as u32, Ordering::Relaxed);
            },
            move |handler, _window| {
                let ui = handler.component();
                let mut shown = shown.lock().unwrap();
                let setter = ParamSetter::new(&*context);
                let sync = |param: &BoolParam, ui_value: bool, last: &mut Option<bool>, set_ui: &dyn Fn(bool)| {
                    if last.is_some_and(|l| l != ui_value) {
                        setter.begin_set_parameter(param);
                        setter.set_parameter(param, ui_value);
                        setter.end_set_parameter(param);
                        *last = Some(ui_value);
                    } else if *last != Some(param.value()) {
                        set_ui(param.value());
                        *last = Some(param.value());
                    }
                };
                sync(&params.you_hear, ui.get_you_hear(), &mut shown.you_hear, &|v| ui.set_you_hear(v));
                sync(&params.viewers_hear, ui.get_viewers_hear(), &mut shown.viewers_hear, &|v| ui.set_viewers_hear(v));
                let dark = params.dark.value();
                let scale = handler.scale_factor();
                let scale_key = (scale * 100.0).round() as u32;
                if shown.dark != Some(dark) || shown.scale != Some(scale_key) {
                    ui.global::<Palette>().set_dark(dark);
                    ui.set_backdrop(backdrop_image(scale, dark));
                    shown.dark = Some(dark);
                    shown.scale = Some(scale_key);
                }
                ui.set_footer(
                    format!("Slint · เปิดใน {} ms · สเกล {:.2}", opened_ms.load(Ordering::Relaxed), scale).into(),
                );
            },
            |_w, _h| {},
        );
        Box::new(Handle { _window: window })
    }

    fn size(&self) -> (u32, u32) {
        (W as u32, H as u32)
    }

    fn set_scale_factor(&self, _factor: f32) -> bool {
        false // follow the system scale (WindowScalePolicy::SystemScaleFactor)
    }

    fn param_value_changed(&self, _id: &str, _normalized_value: f32) {}

    fn param_modulation_changed(&self, _id: &str, _modulation_offset: f32) {}

    fn param_values_changed(&self) {}
}
