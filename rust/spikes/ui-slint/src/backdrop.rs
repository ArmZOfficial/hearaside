//! Port of plugins/common/ui/Backdrop.cpp: paper, 40 px grid and two soft blobs, then glass cards made from a
//! blurred copy, a veil, two drop shadows and a 1 px edge. Rendered once per size (criterion 4: pre-rendered).

#[derive(Clone, Copy)]
pub struct Rgba(pub [f32; 4]); // straight alpha, 0..1

pub struct Colors {
    pub paper: Rgba,
    pub grid: Rgba,
    pub blob_warm: Rgba,
    pub blob_cool: Rgba,
    pub glass: Rgba,
    pub glass_edge: Rgba,
    pub shadow: Rgba,
}

pub struct Rect {
    pub x: f32,
    pub y: f32,
    pub w: f32,
    pub h: f32,
    pub radius: f32,
}

pub struct Image {
    pub w: usize,
    pub h: usize,
    pub px: Vec<[f32; 4]>, // premultiplied
}

impl Image {
    fn new(w: usize, h: usize) -> Self {
        Self { w, h, px: vec![[0.0; 4]; w * h] }
    }
    fn blend(&mut self, i: usize, c: Rgba, cover: f32) {
        let a = c.0[3] * cover;
        let d = &mut self.px[i];
        for k in 0..3 {
            d[k] = c.0[k] * a + d[k] * (1.0 - a);
        }
        d[3] = a + d[3] * (1.0 - a);
    }
}

/// Signed distance to a rounded rectangle (negative inside).
fn rounded_sdf(x: f32, y: f32, r: &Rect) -> f32 {
    let (cx, cy) = (r.x + r.w / 2.0, r.y + r.h / 2.0);
    let (qx, qy) = ((x - cx).abs() - r.w / 2.0 + r.radius, (y - cy).abs() - r.h / 2.0 + r.radius);
    let outside = (qx.max(0.0).powi(2) + qy.max(0.0).powi(2)).sqrt();
    outside + qx.max(qy).min(0.0) - r.radius
}

fn box_blur(img: &mut Image, radius: usize) {
    let r = (radius / 2).max(1) as isize; // three passes of +-r, as boxBlur() in Backdrop.cpp
    let (w, h) = (img.w as isize, img.h as isize);
    let mut line = vec![[0.0f32; 4]; img.w.max(img.h)];
    for _ in 0..3 {
        for (count, stride, starts) in [(w, 1isize, (0..h).map(|y| y * w).collect::<Vec<_>>()), (h, w, (0..w).collect())] {
            for start in starts {
                let at = |i: isize| (start + i.clamp(0, count - 1) * stride) as usize;
                let mut sum = [0.0f32; 4];
                for i in -r..=r {
                    for k in 0..4 {
                        sum[k] += img.px[at(i)][k];
                    }
                }
                for i in 0..count {
                    line[i as usize] = sum.map(|s| s / (2 * r + 1) as f32);
                    let (add, sub) = (img.px[at(i + r + 1)], img.px[at(i - r)]);
                    for k in 0..4 {
                        sum[k] += add[k] - sub[k];
                    }
                }
                for i in 0..count {
                    img.px[at(i)] = line[i as usize];
                }
            }
        }
    }
}

/// Pixel rows/columns a rectangle grown by `margin` covers.
fn span(card: &Rect, margin: f32, dy: f32, scale: f32, w: usize, h: usize) -> (std::ops::Range<usize>, std::ops::Range<usize>) {
    let clamp = |v: f32, max: usize| ((v * scale).floor().max(0.0) as usize).min(max);
    (
        clamp(card.y + dy - margin, h)..clamp(card.y + dy + card.h + margin + 1.0, h),
        clamp(card.x - margin, w)..clamp(card.x + card.w + margin + 1.0, w),
    )
}

pub fn render(width: f32, height: f32, scale: f32, c: &Colors, cards: &[Rect], glass_blur: f32) -> Image {
    let (w, h) = ((width * scale).round() as usize, (height * scale).round() as usize);
    let mut base = Image::new(w, h);
    let blobs = [
        (Rect { x: -60.0, y: -110.0, w: 380.0, h: 240.0, radius: 0.0 }, c.blob_warm),
        (Rect { x: width - 200.0, y: height - 240.0, w: 300.0, h: 300.0, radius: 0.0 }, c.blob_cool),
    ];
    for py in 0..h {
        for px in 0..w {
            let i = py * w + px;
            let (x, y) = ((px as f32 + 0.5) / scale, (py as f32 + 0.5) / scale);
            base.blend(i, c.paper, 1.0);
            let on_grid = |v: f32| (v % 40.0) < 1.0;
            if on_grid(x) || on_grid(y) {
                base.blend(i, c.grid, 1.0);
            }
            for (b, col) in &blobs {
                // radial gradient (closest side) on the ellipse: 1 at the centre, 0.55 at 55 %, 0 at the edge
                let (dx, dy) = ((x - b.x - b.w / 2.0) / (b.w / 2.0), (y - b.y - b.h / 2.0) / (b.h / 2.0));
                let t = (dx * dx + dy * dy).sqrt();
                let k = if t < 0.55 { 1.0 - t / 0.55 * 0.45 } else if t < 1.0 { 0.55 * (1.0 - (t - 0.55) / 0.45) } else { 0.0 };
                base.blend(i, *col, k);
            }
        }
    }
    let mut blurred = Image { w, h, px: base.px.clone() };
    if !cards.is_empty() {
        box_blur(&mut blurred, (glass_blur * scale).round() as usize);
    }

    let mut out = base;
    for card in cards {
        // DropShadow(shadow, 32, {0, 12}) and DropShadow(shadow * 0.7, 2, {0, 1})
        for (blur, dy, alpha) in [(32.0f32, 12.0f32, 1.0f32), (2.0, 1.0, 0.7)] {
            let sigma = blur / 2.5;
            let (rows, cols) = span(card, blur, dy, scale, w, h);
            for py in rows {
                for px in cols.clone() {
                    let (x, y) = ((px as f32 + 0.5) / scale, (py as f32 + 0.5) / scale - dy);
                    let d = rounded_sdf(x, y, card);
                    if d > blur {
                        continue;
                    }
                    let cover = 0.5 * (1.0 - erf(d / (sigma * std::f32::consts::SQRT_2)));
                    let mut s = c.shadow;
                    s.0[3] *= alpha;
                    out.blend(py * w + px, s, cover);
                }
            }
        }
    }
    for card in cards {
        let (rows, cols) = span(card, 2.0, 0.0, scale, w, h);
        for py in rows {
            for px in cols.clone() {
                let (x, y) = ((px as f32 + 0.5) / scale, (py as f32 + 0.5) / scale);
                let d = rounded_sdf(x, y, card);
                let inside = (0.5 - d * scale).clamp(0.0, 1.0);
                let i = py * w + px;
                if inside > 0.0 {
                    let b = blurred.px[i];
                    for k in 0..4 {
                        out.px[i][k] = b[k] * inside + out.px[i][k] * (1.0 - inside);
                    }
                    out.blend(i, c.glass, inside);
                }
                let edge = (1.0 - (d.abs() * scale - 0.0).max(0.0)).clamp(0.0, 1.0) * (1.0 / scale).min(1.0) * scale;
                if edge > 0.0 {
                    out.blend(i, c.glass_edge, edge.min(1.0));
                }
            }
        }
    }
    out
}

fn erf(x: f32) -> f32 {
    // Abramowitz-Stegun 7.1.26
    let t = 1.0 / (1.0 + 0.3275911 * x.abs());
    let y = 1.0 - (((((1.061405429 * t - 1.453152027) * t) + 1.421413741) * t - 0.284496736) * t + 0.254829592) * t * (-x * x).exp();
    y.copysign(x)
}
