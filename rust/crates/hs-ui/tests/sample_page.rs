//! The sample page (`ui/gallery.slint`) rendered headless: it renders, it passes the contrast rules on real pixels in both
//! themes and both languages (`design_contrast`, R5.1's exit test), reduced motion skips the fade, and the menu works.
use hs_app::Language;
use hs_ui::gallery::{self, Options, Page};
use hs_ui::headless::{Frame, Headless};
use slint::LogicalPosition;
use slint::platform::{PointerEventButton, WindowEvent};

const MIN_RATIO: f64 = 4.5;

fn lin(c: u8) -> f64 {
    let x = f64::from(c) / 255.0;
    if x <= 0.03928 {
        x / 12.92
    } else {
        ((x + 0.055) / 1.055).powf(2.4)
    }
}

fn luminance(p: [u8; 4]) -> f64 {
    0.2126 * lin(p[0]) + 0.7152 * lin(p[1]) + 0.0722 * lin(p[2])
}

fn contrast(a: [u8; 4], b: [u8; 4]) -> f64 {
    let (la, lb) = (luminance(a), luminance(b));
    (la.max(lb) + 0.05) / (la.min(lb) + 0.05)
}

struct Cluster {
    pixels: usize,
    best: f64,
    at: (u32, u32),
    text: [u8; 4],
    back: [u8; 4],
}

/// Every connected group of pixels that changed when the text was hidden is one glyph (or a mark of one). The best contrast
/// between its pixel with the text and the same pixel without it is the contrast of that text colour on what is behind it.
fn clusters(shown: &Frame, hidden: &Frame) -> Vec<Cluster> {
    let (w, h) = (shown.width as i64, shown.height as i64);
    let changed = |x: i64, y: i64| {
        let (a, b) = (
            shown.pixel(x as u32, y as u32),
            hidden.pixel(x as u32, y as u32),
        );
        (0..3)
            .map(|k| (i32::from(a[k]) - i32::from(b[k])).abs())
            .sum::<i32>()
            > 12
    };
    let mut seen = vec![false; (w * h) as usize];
    let mut out = Vec::new();
    for y0 in 0..h {
        for x0 in 0..w {
            if seen[(y0 * w + x0) as usize] || !changed(x0, y0) {
                continue;
            }
            let mut stack = vec![(x0, y0)];
            seen[(y0 * w + x0) as usize] = true;
            let mut c = Cluster {
                pixels: 0,
                best: 0.0,
                at: (x0 as u32, y0 as u32),
                text: [0; 4],
                back: [0; 4],
            };
            while let Some((x, y)) = stack.pop() {
                c.pixels += 1;
                let (t, b) = (
                    shown.pixel(x as u32, y as u32),
                    hidden.pixel(x as u32, y as u32),
                );
                let r = contrast(t, b);
                if r > c.best {
                    (c.best, c.at, c.text, c.back) = (r, (x as u32, y as u32), t, b);
                }
                for (dx, dy) in [
                    (-1, -1),
                    (0, -1),
                    (1, -1),
                    (-1, 0),
                    (1, 0),
                    (-1, 1),
                    (0, 1),
                    (1, 1),
                ] {
                    let (nx, ny) = (x + dx, y + dy);
                    if nx >= 0
                        && ny >= 0
                        && nx < w
                        && ny < h
                        && !seen[(ny * w + nx) as usize]
                        && changed(nx, ny)
                    {
                        seen[(ny * w + nx) as usize] = true;
                        stack.push((nx, ny));
                    }
                }
            }
            out.push(c);
        }
    }
    out
}

fn contrast_options(language: Language, dark: bool) -> Options {
    let mut o = Options::new(language, dark);
    o.contrast = true;
    o.menu = true;
    o.toast = true;
    o
}

/// The page twice (text shown, text hidden) at 3x so thin strokes cover whole pixels.
fn page_pair(language: Language, dark: bool) -> (Frame, Frame) {
    let h = Headless::install();
    let scale = 3.0;
    let mut o = contrast_options(language, dark);
    let shown = Page::new(&o, scale).render(&h, scale);
    o.hide_text = true;
    let hidden = Page::new(&o, scale).render(&h, scale);
    (shown, hidden)
}

/// The glyphs under the minimum ratio, and how many glyphs and the lowest ratio there were.
fn weak_glyphs(shown: &Frame, hidden: &Frame) -> (usize, f64, Vec<String>) {
    let all = clusters(shown, hidden);
    let strong: Vec<&Cluster> = all.iter().filter(|c| c.pixels >= 4).collect();
    let lowest = strong.iter().map(|c| c.best).fold(f64::INFINITY, f64::min);
    let weak = strong
        .iter()
        .filter(|c| c.best < MIN_RATIO)
        .map(|c| {
            format!(
                "{:.2}:1 at {:?} ({} px) text {:?} on {:?}",
                c.best, c.at, c.pixels, c.text, c.back
            )
        })
        .collect();
    (strong.len(), lowest, weak)
}

/// R5.1 "done when": the sample page passes the design contrast rules. Not the tokens (`design_contrast` in design.rs) but the
/// rendered pixels: every glyph of every text on the page, over the real glass, blobs, cards and menu behind it.
#[test]
fn design_contrast_sample_page() {
    for language in [Language::Thai, Language::English] {
        for dark in [false, true] {
            let (shown, hidden) = page_pair(language, dark);
            let (glyphs, lowest, problems) = weak_glyphs(&shown, &hidden);
            eprintln!("{language:?} dark={dark}: {glyphs} glyphs, lowest contrast {lowest:.2}:1");
            assert!(
                glyphs > 300,
                "{language:?} dark={dark}: only {glyphs} glyphs found, the page lost its text"
            );
            assert!(
                problems.is_empty(),
                "{language:?} dark={dark}: {} of {glyphs} glyphs under {MIN_RATIO}:1\n{}",
                problems.len(),
                problems[..problems.len().min(12)].join("\n")
            );
        }
    }
}

/// The check can fail: the same page with every text blended 70 % into what is behind it is full of weak glyphs.
#[test]
fn design_contrast_sample_page_checker_sees_faint_text() {
    let (shown, hidden) = page_pair(Language::English, false);
    let faint = Frame {
        width: shown.width,
        height: shown.height,
        rgba: shown
            .rgba
            .iter()
            .zip(&hidden.rgba)
            .map(|(t, b)| ((f32::from(*t) * 0.3) + (f32::from(*b) * 0.7)).round() as u8)
            .collect(),
    };
    assert!(weak_glyphs(&shown, &hidden).2.is_empty());
    let (glyphs, _, weak) = weak_glyphs(&faint, &hidden);
    assert!(
        weak.len() * 2 > glyphs,
        "{} of {glyphs} faint glyphs were flagged",
        weak.len()
    );
}

#[test]
fn sample_page_renders_in_both_themes_and_scales() {
    let h = Headless::install();
    for scale in [1.0f32, 2.0] {
        let light = Page::new(&Options::new(Language::Thai, false), scale).render(&h, scale);
        let dark = Page::new(&Options::new(Language::Thai, true), scale).render(&h, scale);
        assert_eq!(
            (light.width, light.height),
            (
                (gallery::WIDTH * scale) as u32,
                (gallery::HEIGHT * scale) as u32
            )
        );
        // top-left corner, outside every card: the paper
        let (l, d) = (light.pixel(2, 2), dark.pixel(2, 2));
        assert!(l[0] > 200 && d[0] < 60, "paper {l:?} / {d:?}");
        assert!(light.rgba.as_chunks::<4>().0.iter().all(|p| p[3] == 255));
    }
}

#[test]
fn hiding_the_text_changes_only_text() {
    let h = Headless::install();
    let mut o = Options::new(Language::English, false);
    let shown = Page::new(&o, 1.0).render(&h, 1.0);
    o.hide_text = true;
    let hidden = Page::new(&o, 1.0).render(&h, 1.0);
    let changed = shown
        .rgba
        .as_chunks::<4>()
        .0
        .iter()
        .zip(hidden.rgba.as_chunks::<4>().0.iter())
        .filter(|(a, b)| a != b)
        .count();
    assert!(
        changed > 2000 && changed < shown.rgba.len() / 4 / 5,
        "{changed} pixels differ"
    );
    // the QR code (icons, backdrop) did not move
    assert_eq!(shown.pixel(870, 340), hidden.pixel(870, 340));
}

#[test]
fn reduced_motion_opens_the_menu_at_once() {
    let h = Headless::install();
    let mut o = Options::new(Language::English, false);
    // (the frame 30 ms after opening, the frame once everything settled): the same with reduced motion, not without
    let mut frames = |reduce: bool| {
        o.look.reduce_motion = reduce;
        let page = Page::new(&o, 1.0);
        page.render(&h, 1.0);
        page.open_menu();
        h.advance(30);
        let early = h.render(gallery::WIDTH, gallery::HEIGHT, 1.0);
        h.settle();
        (early, h.render(gallery::WIDTH, gallery::HEIGHT, 1.0))
    };
    let (early, settled) = frames(true);
    assert!(
        early.rgba == settled.rgba,
        "reduce motion: the menu is complete in the first frame"
    );
    let (early, settled) = frames(false);
    assert!(
        early.rgba != settled.rgba,
        "without it the menu is still fading in after 30 ms"
    );
}

fn click(h: &Headless, x: f32, y: f32) {
    let w = h.window();
    let position = LogicalPosition::new(x, y);
    w.dispatch_event(WindowEvent::PointerMoved { position });
    w.dispatch_event(WindowEvent::PointerPressed {
        position,
        button: PointerEventButton::Left,
    });
    w.dispatch_event(WindowEvent::PointerReleased {
        position,
        button: PointerEventButton::Left,
    });
}

#[test]
fn menu_rows_click_and_keyboard_follow_the_cpp_overlay() {
    let h = Headless::install();
    let mut o = Options::new(Language::English, false);
    o.menu = true;
    let page = Page::new(&o, 1.0);
    page.render(&h, 1.0);
    assert!(page.ui.get_menu_open());
    assert_eq!(page.menu_len(), 9);

    // the menu opens on its ticked entry ("From DAW", row 5); Down / Up skip what cannot be selected and wrap around
    assert_eq!(page.ui.get_menu_hot(), 5);
    assert!(!page.ui.get_menu_keyboard());
    let hot_after = |dir: i32| {
        page.ui.invoke_menu_step(dir);
        page.ui.get_menu_hot()
    };
    assert_eq!([hot_after(1), hot_after(1)], [6, 7]);
    assert_eq!(
        hot_after(1),
        1,
        "the last row is disabled and the header is not a row: wraps to Copy"
    );
    assert_eq!(hot_after(-1), 7);
    assert!(page.ui.get_menu_keyboard());

    // a switch row toggles and the menu stays open
    let switch = page.menu_row_rect(4);
    assert!(page.menu_row_on(4));
    click(&h, switch.x + 40.0, switch.y + switch.h / 2.0);
    h.settle();
    assert!(!page.menu_row_on(4));
    assert!(page.ui.get_menu_open());

    // an action row closes it
    let copy = page.menu_row_rect(1);
    click(&h, copy.x + 40.0, copy.y + copy.h / 2.0);
    h.settle();
    assert!(!page.ui.get_menu_open());
}
