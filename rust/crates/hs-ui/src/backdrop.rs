//! Backdrop (plugins/common/ui/Backdrop.cpp): everything behind the cards is ours (paper, a 40 px grid, soft blobs), so
//! it is rendered once per size and scale, blurred once, and each glass card is the blurred copy clipped to a rounded
//! rectangle plus a veil, two soft shadows and a 1 px edge. The result is one image for `Backdrop` in `qr.slint`.
//! Ported from the R0.3 spike (docs/spikes/R0.3-ui.md), with the blur limited to the cards.
use crate::Rect;
use crate::theme::{Palette, Rgba};

/// A soft coloured ellipse (`blob_warm` or `blob_cool` token).
#[derive(Clone, Copy, Debug)]
pub struct Blob {
    pub bounds: Rect,
    pub warm: bool,
}

/// A frosted-glass card (default radius `theme::radius::CARD`).
#[derive(Clone, Copy, Debug)]
pub struct Card {
    pub bounds: Rect,
    pub radius: f32,
}

/// Straight-alpha RGBA8, `width * height * 4` bytes.
pub struct Image {
    pub width: usize,
    pub height: usize,
    pub rgba: Vec<u8>,
}

/// Premultiplied float pixels while painting.
struct Canvas {
    w: usize,
    px: Vec<[f32; 4]>,
}

fn straight(c: Rgba) -> [f32; 4] {
    [
        f32::from(c.r) / 255.0,
        f32::from(c.g) / 255.0,
        f32::from(c.b) / 255.0,
        f32::from(c.a) / 255.0,
    ]
}

impl Canvas {
    fn new(w: usize, h: usize) -> Self {
        Self {
            w,
            px: vec![[0.0; 4]; w * h],
        }
    }

    /// Source-over of a straight colour at `cover` (0..1).
    fn blend(&mut self, i: usize, c: [f32; 4], cover: f32) {
        let a = c[3] * cover;
        let d = &mut self.px[i];
        for k in 0..3 {
            d[k] = c[k] * a + d[k] * (1.0 - a);
        }
        d[3] = a + d[3] * (1.0 - a);
    }
}

/// Signed distance to a rounded rectangle (negative inside).
fn rounded_sdf(x: f32, y: f32, r: &Rect, radius: f32) -> f32 {
    let (cx, cy) = (r.x + r.w / 2.0, r.y + r.h / 2.0);
    let (qx, qy) = (
        (x - cx).abs() - r.w / 2.0 + radius,
        (y - cy).abs() - r.h / 2.0 + radius,
    );
    let outside = (qx.max(0.0).powi(2) + qy.max(0.0).powi(2)).sqrt();
    outside + qx.max(qy).min(0.0) - radius
}

/// Three box-blur passes of +-r (`boxBlur`: r = radius / 2, at least 1; sigma about radius / 1.6) on `src` limited to the
/// pixel box `[x0, x1) x [y0, y1)`, edges clamped like the C++. Returns that box.
fn box_blur_region(
    src: &Canvas,
    x0: usize,
    y0: usize,
    x1: usize,
    y1: usize,
    radius: usize,
) -> Canvas {
    let (w, h) = (x1 - x0, y1 - y0);
    let mut out = Canvas::new(w, h);
    for y in 0..h {
        out.px[y * w..(y + 1) * w]
            .copy_from_slice(&src.px[(y0 + y) * src.w + x0..(y0 + y) * src.w + x1]);
    }
    let r = (radius / 2).max(1) as isize;
    let mut line = vec![[0.0f32; 4]; w.max(h)];
    for _ in 0..3 {
        for horizontal in [true, false] {
            let (count, stride, lines) = if horizontal {
                (w as isize, 1isize, h)
            } else {
                (h as isize, w as isize, w)
            };
            for l in 0..lines {
                let start = if horizontal { l * w } else { l } as isize;
                let at = |i: isize| (start + i.clamp(0, count - 1) * stride) as usize;
                let mut sum = [0.0f32; 4];
                for i in -r..=r {
                    for (k, s) in sum.iter_mut().enumerate() {
                        *s += out.px[at(i)][k];
                    }
                }
                for i in 0..count {
                    line[i as usize] = sum.map(|s| s / (2 * r + 1) as f32);
                    let (add, sub) = (out.px[at(i + r + 1)], out.px[at(i - r)]);
                    for k in 0..4 {
                        sum[k] += add[k] - sub[k];
                    }
                }
                for i in 0..count {
                    out.px[at(i)] = line[i as usize];
                }
            }
        }
    }
    out
}

/// The pixel rows and columns a card grown by `margin` (and moved down by `dy`) covers.
fn span(
    r: &Rect,
    margin: f32,
    dy: f32,
    scale: f32,
    w: usize,
    h: usize,
) -> (usize, usize, usize, usize) {
    let clamp = |v: f32, max: usize| ((v * scale).floor().max(0.0) as usize).min(max);
    (
        clamp(r.x - margin, w),
        clamp(r.y + dy - margin, h),
        clamp(r.x + r.w + margin + 1.0, w),
        clamp(r.y + r.h + dy + margin + 1.0, h),
    )
}

/// Abramowitz-Stegun 7.1.26 (the published constants)
#[allow(clippy::excessive_precision)]
fn erf(x: f32) -> f32 {
    let t = 1.0 / (1.0 + 0.3275911 * x.abs());
    let y = 1.0
        - (((((1.061405429 * t - 1.453152027) * t) + 1.421413741) * t - 0.284496736) * t
            + 0.254829592)
            * t
            * (-x * x).exp();
    y.copysign(x)
}

/// Renders the backdrop of a `width` x `height` editor (logical px) at `scale` physical px per logical px.
/// `glass_alpha` is the glass opacity setting (`theme::glass::ALPHA_*`); the veil keeps the glass token's colour.
pub fn render(
    width: f32,
    height: f32,
    scale: f32,
    p: &Palette,
    blobs: &[Blob],
    cards: &[Card],
    glass_alpha: f32,
) -> Image {
    let (w, h) = (
        ((width * scale).round() as usize).max(1),
        ((height * scale).round() as usize).max(1),
    );
    let (paper, grid, edge, shadow) = (
        straight(p.paper),
        straight(p.grid),
        straight(p.glass_edge),
        straight(p.shadow),
    );
    let mut veil = straight(p.glass);
    veil[3] = glass_alpha;
    let mut base = Canvas::new(w, h);
    for py in 0..h {
        for px in 0..w {
            let i = py * w + px;
            let (x, y) = ((px as f32 + 0.5) / scale, (py as f32 + 0.5) / scale);
            base.blend(i, paper, 1.0);
            if x % 40.0 < 1.0 || y % 40.0 < 1.0 {
                base.blend(i, grid, 1.0);
            }
            for b in blobs {
                // CSS radial-gradient(closest-side) on the ellipse: 1 at the centre, 0.55 at 55 %, 0 at the edge
                let (dx, dy) = (
                    (x - b.bounds.x - b.bounds.w / 2.0) / (b.bounds.w / 2.0),
                    (y - b.bounds.y - b.bounds.h / 2.0) / (b.bounds.h / 2.0),
                );
                let t = (dx * dx + dy * dy).sqrt();
                let k = if t < 0.55 {
                    1.0 - t / 0.55 * 0.45
                } else if t < 1.0 {
                    0.55 * (1.0 - (t - 0.55) / 0.45)
                } else {
                    0.0
                };
                base.blend(
                    i,
                    straight(if b.warm { p.blob_warm } else { p.blob_cool }),
                    k,
                );
            }
        }
    }

    // each card shows the blurred base under it; three passes of +-r reach 3r + 1 pixels
    let blur = (crate::theme::glass::BLUR * scale).round() as usize;
    let reach = 3 * (blur / 2).max(1) + 2;
    let blurred: Vec<(usize, usize, Canvas)> = cards
        .iter()
        .map(|c| {
            let (x0, y0, x1, y1) = span(&c.bounds, reach as f32 / scale, 0.0, scale, w, h);
            (x0, y0, box_blur_region(&base, x0, y0, x1, y1, blur))
        })
        .collect();

    let mut out = base;
    for c in cards {
        // DropShadow(shadow, 32, {0, 12}) and DropShadow(shadow * 0.7, 2, {0, 1})
        for (blur_px, dy, alpha) in [(32.0f32, 12.0f32, 1.0f32), (2.0, 1.0, 0.7)] {
            let sigma = blur_px / 2.5;
            let (x0, y0, x1, y1) = span(&c.bounds, blur_px, dy, scale, w, h);
            for py in y0..y1 {
                for px in x0..x1 {
                    let (x, y) = ((px as f32 + 0.5) / scale, (py as f32 + 0.5) / scale - dy);
                    let d = rounded_sdf(x, y, &c.bounds, c.radius);
                    if d > blur_px {
                        continue;
                    }
                    let cover = 0.5 * (1.0 - erf(d / (sigma * std::f32::consts::SQRT_2)));
                    let mut s = shadow;
                    s[3] *= alpha;
                    out.blend(py * w + px, s, cover);
                }
            }
        }
    }
    for (c, (bx, by, blur_img)) in cards.iter().zip(&blurred) {
        let (x0, y0, x1, y1) = span(&c.bounds, 2.0, 0.0, scale, w, h);
        for py in y0..y1 {
            for px in x0..x1 {
                let (x, y) = ((px as f32 + 0.5) / scale, (py as f32 + 0.5) / scale);
                let d = rounded_sdf(x, y, &c.bounds, c.radius);
                let inside = (0.5 - d * scale).clamp(0.0, 1.0);
                let i = py * w + px;
                if inside > 0.0 {
                    let b = blur_img.px[(py - by) * blur_img.w + (px - bx)];
                    for (o, b) in out.px[i].iter_mut().zip(b) {
                        *o = b * inside + *o * (1.0 - inside);
                    }
                    out.blend(i, veil, inside);
                }
                // the 1 px stroke is centred on the card's edge: half in, half out
                let stroke = ((0.5 - d.abs()) * scale + 0.5).clamp(0.0, 1.0);
                if stroke > 0.0 {
                    out.blend(i, edge, stroke);
                }
            }
        }
    }

    let mut rgba = Vec::with_capacity(w * h * 4);
    for p in &out.px {
        let a = p[3].max(1e-6);
        let c = |v: f32| ((v / a).clamp(0.0, 1.0) * 255.0).round() as u8;
        rgba.extend_from_slice(&[c(p[0]), c(p[1]), c(p[2]), (p[3] * 255.0).round() as u8]);
    }
    Image {
        width: w,
        height: h,
        rgba,
    }
}

impl Image {
    /// The pixel at (`x`, `y`) as `[r, g, b, a]`.
    pub fn pixel(&self, x: usize, y: usize) -> [u8; 4] {
        let i = (y * self.width + x) * 4;
        [
            self.rgba[i],
            self.rgba[i + 1],
            self.rgba[i + 2],
            self.rgba[i + 3],
        ]
    }

    /// For an `Image` element: `Backdrop { source: image.to_slint() }`.
    pub fn to_slint(&self) -> slint::Image {
        let mut buffer = slint::SharedPixelBuffer::<slint::Rgba8Pixel>::new(
            self.width as u32,
            self.height as u32,
        );
        for (d, s) in buffer
            .make_mut_slice()
            .iter_mut()
            .zip(self.rgba.as_chunks::<4>().0)
        {
            *d = slint::Rgba8Pixel {
                r: s[0],
                g: s[1],
                b: s[2],
                a: s[3],
            };
        }
        slint::Image::from_rgba8(buffer)
    }
}

#[cfg(test)]
mod tests {
    use super::*;
    use crate::theme::{DARK, LIGHT, glass, radius};

    fn card(x: f32, y: f32, w: f32, h: f32) -> Card {
        Card {
            bounds: Rect::new(x, y, w, h),
            radius: radius::CARD,
        }
    }

    fn over(top: Rgba, bottom: [u8; 3]) -> [u8; 3] {
        let a = f32::from(top.a) / 255.0;
        let mix = |t: u8, b: u8| (f32::from(t) * a + f32::from(b) * (1.0 - a)).round() as u8;
        [
            mix(top.r, bottom[0]),
            mix(top.g, bottom[1]),
            mix(top.b, bottom[2]),
        ]
    }

    fn close(a: [u8; 4], b: [u8; 3], tolerance: i32) -> bool {
        (0..3).all(|k| (i32::from(a[k]) - i32::from(b[k])).abs() <= tolerance)
    }

    #[test]
    fn size_follows_scale() {
        let img = render(100.0, 60.0, 1.5, &LIGHT, &[], &[], glass::ALPHA_DEFAULT);
        assert_eq!(
            (img.width, img.height, img.rgba.len()),
            (150, 90, 150 * 90 * 4)
        );
    }

    #[test]
    fn plain_pixel_is_paper_and_grid_line_is_paper_under_grid() {
        for p in [&LIGHT, &DARK] {
            let img = render(120.0, 120.0, 1.0, p, &[], &[], glass::ALPHA_DEFAULT);
            assert!(
                close(img.pixel(20, 20), [p.paper.r, p.paper.g, p.paper.b], 0),
                "paper"
            );
            // x = 40 is a grid line: paper with the grid token over it
            assert!(
                close(
                    img.pixel(40, 20),
                    over(p.grid, [p.paper.r, p.paper.g, p.paper.b]),
                    1
                ),
                "grid"
            );
            assert_eq!(img.pixel(20, 20)[3], 255);
        }
    }

    #[test]
    fn glass_card_is_the_blurred_backdrop_under_the_veil() {
        for p in [&LIGHT, &DARK] {
            let img = render(
                200.0,
                200.0,
                1.0,
                p,
                &[],
                &[card(40.0, 40.0, 120.0, 120.0)],
                0.62,
            );
            // deep inside, away from grid lines: paper (blur of a flat area) with the glass veil at 0.62
            let veil = Rgba {
                a: (0.62f32 * 255.0).round() as u8,
                ..p.glass
            };
            assert!(close(
                img.pixel(100, 100),
                over(veil, [p.paper.r, p.paper.g, p.paper.b]),
                3
            ));
            // outside the card and its shadow: untouched paper
            assert!(close(img.pixel(5, 5), [p.paper.r, p.paper.g, p.paper.b], 0));
        }
    }

    #[test]
    fn card_has_an_edge_and_a_shadow_below_it() {
        let img = render(
            200.0,
            200.0,
            1.0,
            &LIGHT,
            &[],
            &[card(40.0, 40.0, 120.0, 120.0)],
            0.62,
        );
        // the left edge column is lighter than the paper (glassEdge is white at 0.85)
        assert!(img.pixel(40, 100)[0] > img.pixel(30, 100)[0]);
        // 10 px under the card the shadow darkens the paper
        assert!(img.pixel(100, 170)[0] < img.pixel(5, 170)[0]);
    }

    #[test]
    fn blob_tints_its_centre() {
        let blob = Blob {
            bounds: Rect::new(20.0, 20.0, 100.0, 100.0),
            warm: true,
        };
        let img = render(
            200.0,
            200.0,
            1.0,
            &LIGHT,
            &[blob],
            &[],
            glass::ALPHA_DEFAULT,
        );
        assert!(
            img.pixel(70, 70)[0] < img.pixel(180, 180)[0],
            "warm blob darkens the light paper at its centre"
        );
        assert!(close(
            img.pixel(180, 180),
            [LIGHT.paper.r, LIGHT.paper.g, LIGHT.paper.b],
            0
        ));
    }

    #[test]
    fn scaled_render_matches_the_unscaled_one_at_the_same_point() {
        let cards = [card(20.0, 20.0, 100.0, 80.0)];
        let a = render(160.0, 120.0, 1.0, &DARK, &[], &cards, 0.62);
        let b = render(160.0, 120.0, 2.0, &DARK, &[], &cards, 0.62);
        for (x, y) in [(70usize, 60usize), (10, 10), (140, 100)] {
            assert!(
                close(
                    b.pixel(x * 2, y * 2),
                    [a.pixel(x, y)[0], a.pixel(x, y)[1], a.pixel(x, y)[2]],
                    3
                ),
                "{x},{y}"
            );
        }
    }
}
