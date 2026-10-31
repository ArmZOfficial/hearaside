//! `hs-ui-gallery <output directory>`: renders the sample page of hs-ui without a window, to PNG.
//! Light and dark, Thai and English, 100 % and 200 % UI size (the menu and the toast are open in the first two).
use hs_app::Language;
use hs_ui::gallery::{self, Options, Page};
use hs_ui::headless::Headless;
use std::path::Path;

fn main() -> Result<(), Box<dyn std::error::Error>> {
    let out = std::env::args()
        .nth(1)
        .ok_or("usage: hs-ui-gallery <output directory>")?;
    std::fs::create_dir_all(&out)?;
    let headless = Headless::install();
    let shots = [
        ("gallery-light-th-100.png", Language::Thai, false, 1.0, true),
        ("gallery-dark-th-100.png", Language::Thai, true, 1.0, true),
        (
            "gallery-light-en-100.png",
            Language::English,
            false,
            1.0,
            false,
        ),
        (
            "gallery-light-en-dropdown.png",
            Language::English,
            false,
            1.0,
            false,
        ),
        ("gallery-dark-th-200.png", Language::Thai, true, 2.0, false),
    ];
    for (name, language, dark, scale, overlay) in shots {
        let mut options = Options::new(language, dark);
        options.look.ui_scale = scale;
        options.menu = overlay;
        options.toast = overlay;
        options.dropdown = name.contains("dropdown");
        let started = std::time::Instant::now();
        let frame = Page::new(&options, scale).render(&headless, scale);
        frame.write_png(&Path::new(&out).join(name))?;
        println!(
            "{name}: {}x{} in {:.0} ms",
            frame.width,
            frame.height,
            started.elapsed().as_secs_f64() * 1000.0
        );
    }
    println!("page: {}x{} units", gallery::WIDTH, gallery::HEIGHT);
    Ok(())
}
