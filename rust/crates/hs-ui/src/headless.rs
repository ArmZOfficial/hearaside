//! Slint's software renderer without a window: the renderer behind `hs-ui-gallery` and the contrast check, and the
//! one PLAN D2 wants for ui-snapshot. The clock is ours, so animations are finished by moving it, not by sleeping.
use slint::platform::software_renderer::{
    MinimalSoftwareWindow, PremultipliedRgbaColor, RepaintBufferType,
};
use slint::platform::{Platform, WindowAdapter};
use slint::{PhysicalSize, PlatformError};
use std::cell::{Cell, RefCell};
use std::rc::Rc;
use std::time::Duration;

/// A rendered frame, straight-alpha RGBA8.
pub struct Frame {
    pub width: u32,
    pub height: u32,
    pub rgba: Vec<u8>,
}

impl Frame {
    pub fn pixel(&self, x: u32, y: u32) -> [u8; 4] {
        let i = ((y * self.width + x) * 4) as usize;
        [
            self.rgba[i],
            self.rgba[i + 1],
            self.rgba[i + 2],
            self.rgba[i + 3],
        ]
    }

    /// Writes the frame as an 8-bit RGBA PNG.
    #[cfg(feature = "png")]
    pub fn write_png(&self, path: &std::path::Path) -> Result<(), Box<dyn std::error::Error>> {
        let mut encoder = png::Encoder::new(
            std::io::BufWriter::new(std::fs::File::create(path)?),
            self.width,
            self.height,
        );
        encoder.set_color(png::ColorType::Rgba);
        encoder.set_depth(png::BitDepth::Eight);
        encoder.write_header()?.write_image_data(&self.rgba)?;
        Ok(())
    }
}

struct HeadlessPlatform {
    window: Rc<MinimalSoftwareWindow>,
    clock: Rc<Cell<Duration>>,
}

impl Platform for HeadlessPlatform {
    fn create_window_adapter(&self) -> Result<Rc<dyn WindowAdapter>, PlatformError> {
        Ok(self.window.clone())
    }

    fn duration_since_start(&self) -> Duration {
        self.clock.get()
    }
}

/// The headless platform of the current thread (Slint keeps one per thread): create components after `Headless::install()`,
/// one at a time per thread.
#[derive(Clone)]
pub struct Headless {
    window: Rc<MinimalSoftwareWindow>,
    clock: Rc<Cell<Duration>>,
}

thread_local! {
    static INSTALLED: RefCell<Option<Headless>> = const { RefCell::new(None) };
}

impl Headless {
    /// Installs the platform on this thread the first time, and returns the handle every time.
    pub fn install() -> Headless {
        INSTALLED.with(|slot| {
            slot.borrow_mut()
                .get_or_insert_with(|| {
                    let window = MinimalSoftwareWindow::new(RepaintBufferType::NewBuffer);
                    let clock = Rc::new(Cell::new(Duration::ZERO));
                    let platform = HeadlessPlatform {
                        window: window.clone(),
                        clock: clock.clone(),
                    };
                    slint::platform::set_platform(Box::new(platform))
                        .expect("another Slint platform was installed on this thread");
                    Headless { window, clock }
                })
                .clone()
        })
    }

    /// Lets 1 s pass for animations and timers (switches take 160 ms, menus fade in 120 ms).
    pub fn settle(&self) {
        self.clock.set(self.clock.get() + Duration::from_secs(1));
        slint::platform::update_timers_and_animations();
    }

    /// Lets `ms` pass without finishing long animations.
    pub fn advance(&self, ms: u64) {
        self.clock.set(self.clock.get() + Duration::from_millis(ms));
        slint::platform::update_timers_and_animations();
    }

    /// Renders the window at `scale` (physical px per logical px) with a logical size of `width` x `height`.
    pub fn render(&self, width: f32, height: f32, scale: f32) -> Frame {
        let (w, h) = (
            (width * scale).round() as u32,
            (height * scale).round() as u32,
        );
        self.window
            .dispatch_event(slint::platform::WindowEvent::ScaleFactorChanged {
                scale_factor: scale,
            });
        self.window.set_size(PhysicalSize::new(w, h));
        let mut pixels = vec![PremultipliedRgbaColor::default(); (w * h) as usize];
        self.window.request_redraw();
        let drawn = self.window.draw_if_needed(|renderer| {
            renderer.render(&mut pixels, w as usize);
        });
        assert!(
            drawn,
            "the window had nothing to draw: show() the component first"
        );
        let mut rgba = Vec::with_capacity(pixels.len() * 4);
        for p in pixels {
            let a = u32::from(p.alpha.max(1));
            let un = |c: u8| ((u32::from(c) * 255 + a / 2) / a).min(255) as u8;
            rgba.extend_from_slice(&[un(p.red), un(p.green), un(p.blue), p.alpha]);
        }
        Frame {
            width: w,
            height: h,
            rgba,
        }
    }

    /// The Slint window of the platform, for events (`dispatch_event`).
    pub fn window(&self) -> &Rc<MinimalSoftwareWindow> {
        &self.window
    }
}
