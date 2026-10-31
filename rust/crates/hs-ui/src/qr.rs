//! QrView (plugins/common/ui/QrView.h): the QR matrix of a link and the SVG path of its dark modules for `qr.slint`.
//! `qrcodegen` is the Rust build of the library `qrcodegen.cpp` came from, so the matrix is the same (see the C++ golden test).
use qrcodegen::{QrCode, QrCodeEcc};

/// Quiet zone around the code, in modules.
pub const BORDER: i32 = 2;

pub struct Qr(QrCode);

/// The QR code of `text` (error correction Medium, boosted when it fits). `None` for an empty text or one too long for a QR
/// code, in which case QrView draws the white card only.
pub fn encode(text: &str) -> Option<Qr> {
    if text.is_empty() {
        return None;
    }
    QrCode::encode_text(text, QrCodeEcc::Medium).ok().map(Qr)
}

impl Qr {
    /// Modules per side, without the quiet zone.
    pub fn size(&self) -> i32 {
        self.0.size()
    }

    /// The module at column `x`, row `y`; false outside the matrix.
    pub fn module(&self, x: i32, y: i32) -> bool {
        self.0.get_module(x, y)
    }

    /// Modules per side with the quiet zone: the `viewbox` of the path.
    pub fn total(&self) -> i32 {
        self.size() + 2 * BORDER
    }

    /// The dark modules as one SVG path in module units, quiet zone included. Neighbours in a row are one rectangle.
    pub fn path(&self) -> String {
        let mut d = String::new();
        for y in 0..self.size() {
            let mut x = 0;
            while x < self.size() {
                if !self.module(x, y) {
                    x += 1;
                    continue;
                }
                let start = x;
                while x < self.size() && self.module(x, y) {
                    x += 1;
                }
                d.push_str(&format!(
                    "M{} {}h{}v1h-{}z",
                    start + BORDER,
                    y + BORDER,
                    x - start,
                    x - start
                ));
            }
        }
        d
    }
}

#[cfg(test)]
mod tests {
    use super::*;

    /// Parses `tests/fixtures/qr-cpp.txt`: "text", "size mask", then `size` rows of '#' / '.', per sample.
    fn cpp_matrices() -> Vec<(String, Vec<String>)> {
        let text = include_str!("../tests/fixtures/qr-cpp.txt");
        let mut lines = text.lines();
        let mut out = Vec::new();
        while let Some(t) = lines.next() {
            let size: usize = lines
                .next()
                .unwrap()
                .split(' ')
                .next()
                .unwrap()
                .parse()
                .unwrap();
            out.push((
                t.to_string(),
                (0..size)
                    .map(|_| lines.next().unwrap().to_string())
                    .collect(),
            ));
        }
        out
    }

    #[test]
    fn matrix_matches_the_cpp_qrcodegen_bit_for_bit() {
        let samples = cpp_matrices();
        assert_eq!(samples.len(), 3);
        for (text, rows) in samples {
            let qr = encode(&text).unwrap();
            assert_eq!(qr.size() as usize, rows.len(), "{text}");
            for (y, row) in rows.iter().enumerate() {
                for (x, ch) in row.chars().enumerate() {
                    assert_eq!(
                        qr.module(x as i32, y as i32),
                        ch == '#',
                        "{text} at {x},{y}"
                    );
                }
            }
        }
    }

    #[test]
    fn empty_and_oversized_text_have_no_code() {
        assert!(encode("").is_none());
        assert!(encode(&"x".repeat(5000)).is_none());
    }

    #[test]
    fn path_covers_exactly_the_dark_modules() {
        let qr = encode("https://hearaside.example/listen/abc123").unwrap();
        let mut grid = vec![vec![false; qr.total() as usize]; qr.total() as usize];
        for part in qr.path().split('M').skip(1) {
            let (xy, rest) = part.split_once('h').unwrap();
            let (x, y) = xy.split_once(' ').unwrap();
            let run: usize = rest.split_once('v').unwrap().0.parse().unwrap();
            for i in 0..run {
                grid[y.parse::<usize>().unwrap()][x.parse::<usize>().unwrap() + i] = true;
            }
        }
        for y in 0..qr.total() {
            for x in 0..qr.total() {
                assert_eq!(
                    grid[y as usize][x as usize],
                    qr.module(x - BORDER, y - BORDER),
                    "{x},{y}"
                );
            }
        }
    }
}
