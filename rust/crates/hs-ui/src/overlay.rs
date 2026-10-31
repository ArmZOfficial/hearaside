//! Geometry of the Overlay (plugins/common/ui/Overlay.cpp): row heights of a menu, which row is under the pointer, the
//! keyboard step, and where a menu or popover card goes next to its anchor. `overlay.slint` draws; this decides.
use crate::Rect;

/// Padding of a menu card.
pub const PAD: f32 = 6.0;
/// Standard row, row with a second line, section header, separator.
pub const ROW_H: f32 = 36.0;
pub const ROW_SUB_H: f32 = 46.0;
pub const HEADER_H: f32 = 25.0;
pub const SEP_H: f32 = 11.0;
/// Padding of a popover card around its content.
pub const POPOVER_PAD: f32 = 16.0;
/// Gap kept between a card and the edge of the editor, and between a card and its anchor.
pub const EDGE: f32 = 12.0;
pub const ANCHOR_GAP: f32 = 6.0;

/// `MenuKind` of `overlay.slint`.
#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub enum Kind {
    Action,
    Switch,
    Check,
    Header,
    Separator,
}

/// What the geometry needs to know about a row.
#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub struct Row {
    pub kind: Kind,
    pub enabled: bool,
    /// a second (grey) line
    pub has_sub: bool,
    /// Switch / Check state
    pub on: bool,
}

impl Row {
    pub fn new(kind: Kind) -> Self {
        Self {
            kind,
            enabled: true,
            has_sub: false,
            on: false,
        }
    }

    pub fn height(&self) -> f32 {
        match self.kind {
            Kind::Header => HEADER_H,
            Kind::Separator => SEP_H,
            _ if self.has_sub => ROW_SUB_H,
            _ => ROW_H,
        }
    }

    pub fn selectable(&self) -> bool {
        self.enabled && !matches!(self.kind, Kind::Header | Kind::Separator)
    }
}

/// Top and height of every row, and the height of the card.
#[derive(Clone, Debug, PartialEq)]
pub struct Layout {
    pub y: Vec<f32>,
    pub h: Vec<f32>,
    pub total: f32,
}

pub fn layout(rows: &[Row]) -> Layout {
    let mut y = PAD;
    let mut ys = Vec::with_capacity(rows.len());
    let mut hs = Vec::with_capacity(rows.len());
    for r in rows {
        ys.push(y);
        hs.push(r.height());
        y += r.height();
    }
    Layout {
        y: ys,
        h: hs,
        total: y + PAD,
    }
}

impl Layout {
    /// The row under `y` (card coordinates), `None` in the padding.
    pub fn row_at(&self, y: f32) -> Option<usize> {
        (0..self.y.len()).find(|&i| y >= self.y[i] && y < self.y[i] + self.h[i])
    }
}

/// A dropdown opens on its selected entry: the last ticked Check row.
pub fn initial_hot(rows: &[Row]) -> Option<usize> {
    rows.iter().rposition(|r| r.kind == Kind::Check && r.on)
}

/// Down / Tab (`dir` = 1) and Up / Shift+Tab (`dir` = -1) move to the next selectable row, wrapping around. With no row
/// hot yet, down goes to the first and up to the last. (The C++ went to the second-to-last: `hot_` = -1 was stepped as an index.)
pub fn step(rows: &[Row], hot: Option<usize>, dir: i32) -> Option<usize> {
    let n = rows.len() as i32;
    let mut i = match hot {
        Some(h) => h as i32,
        None if dir > 0 => -1,
        None => n,
    };
    for _ in 0..n {
        i = (i + dir).rem_euclid(n);
        if rows[i as usize].selectable() {
            return Some(i as usize);
        }
    }
    hot
}

/// Size of a popover card for content of `w` x `h`.
pub fn popover_size(w: f32, h: f32) -> (f32, f32) {
    (w + 2.0 * POPOVER_PAD, h + 2.0 * POPOVER_PAD)
}

/// Where a card of `w` x `h` goes in an editor of `bounds_w` x `bounds_h`: below `anchor` (above when there is no room),
/// right-aligned with it unless `align_left`, kept 12 from the edges. No anchor = the top third, centred horizontally
/// (`Overlay::place`). A card taller than the editor is cut to fit (it scrolls inside).
pub fn place(
    anchor: Option<Rect>,
    w: f32,
    h: f32,
    bounds_w: f32,
    bounds_h: f32,
    align_left: bool,
) -> Rect {
    let a = anchor.unwrap_or(Rect::new(
        (bounds_w / 2.0).floor(),
        (bounds_h / 3.0).floor(),
        0.0,
        0.0,
    ));
    let h = h.min(bounds_h - 2.0 * EDGE);
    let left = if align_left { a.x } else { a.right() - w };
    let left = left.clamp(EDGE, EDGE.max(bounds_w - EDGE - w));
    let mut top = a.bottom() + ANCHOR_GAP;
    if top + h > bounds_h - EDGE {
        top = EDGE.max(a.y - ANCHOR_GAP - h);
    }
    Rect::new(left, top, w, h)
}

#[cfg(test)]
mod tests {
    use super::*;

    fn menu() -> Vec<Row> {
        let mut disabled = Row::new(Kind::Action);
        disabled.enabled = false;
        let mut sub = Row::new(Kind::Action);
        sub.has_sub = true;
        vec![
            Row::new(Kind::Header),
            Row::new(Kind::Action),
            Row::new(Kind::Separator),
            disabled,
            sub,
            Row::new(Kind::Switch),
        ]
    }

    #[test]
    fn layout_stacks_rows_between_the_padding() {
        let l = layout(&menu());
        assert_eq!(l.y, vec![6.0, 31.0, 67.0, 78.0, 114.0, 160.0]);
        assert_eq!(l.h, vec![25.0, 36.0, 11.0, 36.0, 46.0, 36.0]);
        assert_eq!(l.total, 202.0);
        assert_eq!(layout(&[]).total, 12.0);
    }

    #[test]
    fn row_at_hits_rows_not_the_padding() {
        let l = layout(&menu());
        assert_eq!(l.row_at(3.0), None);
        assert_eq!(l.row_at(6.0), Some(0));
        assert_eq!(l.row_at(66.9), Some(1));
        assert_eq!(l.row_at(67.0), Some(2));
        assert_eq!(l.row_at(l.total - 1.0), None);
    }

    #[test]
    fn step_skips_headers_separators_and_disabled_rows_and_wraps() {
        let rows = menu();
        assert_eq!(step(&rows, None, 1), Some(1));
        assert_eq!(step(&rows, Some(1), 1), Some(4));
        assert_eq!(step(&rows, Some(5), 1), Some(1));
        assert_eq!(step(&rows, Some(1), -1), Some(5));
        assert_eq!(step(&rows, Some(4), -1), Some(1));
        // C++ bug: Up from "nothing hot" landed on the second-to-last row
        assert_eq!(step(&rows, None, -1), Some(5));
        assert_eq!(step(&[Row::new(Kind::Header)], Some(0), 1), Some(0));
        assert_eq!(step(&[], None, 1), None);
    }

    #[test]
    fn a_dropdown_opens_on_its_selected_entry() {
        let mut rows = vec![Row::new(Kind::Check); 3];
        rows[1].on = true;
        assert_eq!(initial_hot(&rows), Some(1));
        assert_eq!(initial_hot(&menu()), None);
    }

    #[test]
    fn card_goes_below_the_anchor_right_aligned() {
        let r = place(
            Some(Rect::new(300.0, 40.0, 32.0, 32.0)),
            236.0,
            200.0,
            760.0,
            600.0,
            false,
        );
        assert_eq!(r, Rect::new(96.0, 78.0, 236.0, 200.0));
    }

    #[test]
    fn card_flips_above_when_there_is_no_room_below() {
        let r = place(
            Some(Rect::new(100.0, 500.0, 120.0, 38.0)),
            160.0,
            150.0,
            760.0,
            600.0,
            true,
        );
        assert_eq!(r, Rect::new(100.0, 344.0, 160.0, 150.0));
    }

    #[test]
    fn card_stays_12_inside_the_edges_and_never_taller_than_the_editor() {
        let left = place(
            Some(Rect::new(0.0, 10.0, 20.0, 20.0)),
            236.0,
            100.0,
            760.0,
            600.0,
            false,
        );
        assert_eq!(left.x, 12.0);
        let right = place(
            Some(Rect::new(700.0, 10.0, 50.0, 20.0)),
            236.0,
            100.0,
            760.0,
            600.0,
            true,
        );
        assert_eq!(right.x, 760.0 - 12.0 - 236.0);
        let tall = place(
            Some(Rect::new(10.0, 10.0, 20.0, 20.0)),
            100.0,
            900.0,
            300.0,
            400.0,
            true,
        );
        assert_eq!(tall.h, 376.0);
        assert_eq!(tall.y, 12.0);
        let none = place(None, 100.0, 80.0, 300.0, 400.0, false);
        assert_eq!((none.x, none.y), (50.0, 139.0));
    }

    #[test]
    fn popover_adds_its_padding() {
        assert_eq!(popover_size(200.0, 90.0), (232.0, 122.0));
    }
}
