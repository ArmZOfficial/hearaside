//! R0.3 spike: renders the Slint Track page to PNG without a window (criteria 1, 4 and 5).
//!   cargo run --release -- <output directory>
//! Scale 2.5 matches tools/ui-snapshot (UI size 125 % at 2x); 1, 1.5 and 2 are the display scales to check.
mod backdrop;
mod tokens;

use slint::platform::software_renderer::{MinimalSoftwareWindow, PremultipliedRgbaColor, RepaintBufferType};
use slint::platform::{Platform, WindowAdapter};
use slint::{ComponentHandle, Image, PhysicalSize, Rgba8Pixel, SharedPixelBuffer};
use std::path::Path;
use std::rc::Rc;
use std::time::Instant;

slint::include_modules!();

struct Headless(Rc<MinimalSoftwareWindow>);

impl Platform for Headless {
    fn create_window_adapter(&self) -> Result<Rc<dyn WindowAdapter>, slint::PlatformError> {
        Ok(self.0.clone())
    }
}

const W: f32 = 380.0;
const H: f32 = 640.0;

/// Header and card as TrackEditor::layout() places them (kPad 12, kHeaderH 54, kGap 10; radii 18 / 20).
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

fn render(window: &MinimalSoftwareWindow, ui: &TrackSpike, scale: f32, dark: bool, file: &Path) -> Result<(f64, f64), Box<dyn std::error::Error>> {
    let (w, h) = ((W * scale).round() as u32, (H * scale).round() as u32);
    window.dispatch_event(slint::platform::WindowEvent::ScaleFactorChanged { scale_factor: scale });
    window.set_size(PhysicalSize::new(w, h));
    ui.global::<Palette>().set_dark(dark);
    let started = Instant::now();
    ui.set_backdrop(backdrop_image(scale, dark));
    let backdrop_ms = started.elapsed().as_secs_f64() * 1000.0;
    std::thread::sleep(std::time::Duration::from_millis(250)); // let the 160 ms animations finish
    slint::platform::update_timers_and_animations();
    let start = Instant::now();
    let mut pixels = vec![PremultipliedRgbaColor::default(); (w * h) as usize];
    window.request_redraw();
    window.draw_if_needed(|renderer| {
        renderer.render(&mut pixels, w as usize);
    });
    let render_ms = start.elapsed().as_secs_f64() * 1000.0;
    let mut rgba = Vec::with_capacity(pixels.len() * 4);
    for p in pixels {
        let a = p.alpha.max(1) as u32;
        let un = |c: u8| ((c as u32 * 255 + a / 2) / a).min(255) as u8;
        rgba.extend_from_slice(&[un(p.red), un(p.green), un(p.blue), p.alpha]);
    }
    let mut encoder = png::Encoder::new(std::io::BufWriter::new(std::fs::File::create(file)?), w, h);
    encoder.set_color(png::ColorType::Rgba);
    encoder.set_depth(png::BitDepth::Eight);
    encoder.write_header()?.write_image_data(&rgba)?;
    Ok((backdrop_ms, render_ms))
}

fn main() -> Result<(), Box<dyn std::error::Error>> {
    let out = std::env::args().nth(1).ok_or("usage: spike-ui-slint <output directory>")?;
    std::fs::create_dir_all(&out)?;
    let window = MinimalSoftwareWindow::new(RepaintBufferType::NewBuffer);
    slint::platform::set_platform(Box::new(Headless(window.clone())))?;
    let created = Instant::now();
    let ui = TrackSpike::new()?;
    ui.show()?;
    println!("create: {:.1} ms", created.elapsed().as_secs_f64() * 1000.0);
    for dark in [false, true] {
        for scale in [1.0f32, 1.5, 2.0, 2.5] {
            let name = format!("slint-{}-{}.png", if dark { "dark" } else { "light" }, (scale * 100.0) as u32);
            let (backdrop_ms, render_ms) = render(&window, &ui, scale, dark, &Path::new(&out).join(&name))?;
            println!("{name}: backdrop {backdrop_ms:.1} ms, render {render_ms:.1} ms");
        }
    }
    Ok(())
}
