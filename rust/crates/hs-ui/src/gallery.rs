//! The sample page (`ui/gallery.slint`) and the Rust glue every window needs: the backdrop image, the strings, the open menu
//! (rows laid out and placed by `overlay`, keyboard stepping, switches that toggle) and the theme / motion switches.
//! R5.5 windows do the same with their own generated types; this is the reference.
use crate::headless::{Frame, Headless};
use crate::overlay::{self, Kind};
use crate::qr;
use crate::scale::Look;
use crate::theme;
use crate::ui::{
    Anchor, CardRect, DotKind, Gallery, GalleryTexts, IconId, MenuKind, MenuRow, Motion, Palette,
};
use crate::{Rect, backdrop};
use hs_app::{Language, Str, tr_in};
use slint::{ComponentHandle, Model, ModelRc, SharedString, VecModel};
use std::cell::RefCell;
use std::rc::Rc;

/// Logical size of the page.
pub const WIDTH: f32 = 1240.0;
pub const HEIGHT: f32 = 720.0;

/// The three glass cards (also the cards of the backdrop image).
pub const CARDS: [Rect; 3] = [
    Rect {
        x: 16.0,
        y: 16.0,
        w: 380.0,
        h: 688.0,
    },
    Rect {
        x: 412.0,
        y: 16.0,
        w: 400.0,
        h: 688.0,
    },
    Rect {
        x: 828.0,
        y: 16.0,
        w: 396.0,
        h: 688.0,
    },
];

/// A link to put in the QR code.
pub const QR_TEXT: &str = "https://hearaside.example/listen/0123456789abcdef";

#[derive(Clone, Copy, Debug)]
pub struct Options {
    pub language: Language,
    pub look: Look,
    /// open the "More" menu next to its button
    pub menu: bool,
    /// open the dropdown list under its button
    pub dropdown: bool,
    /// show the "Copied" toast
    pub toast: bool,
    /// let every text turn transparent: the second render of the contrast check
    pub hide_text: bool,
    /// the page of the contrast check: nothing disabled or faded (disabled text is exempt from the contrast rules)
    pub contrast: bool,
}

impl Options {
    pub fn new(language: Language, dark: bool) -> Self {
        Self {
            language,
            look: Look {
                dark,
                ui_scale: 1.0,
                reduce_motion: false,
            },
            menu: false,
            dropdown: false,
            toast: false,
            hide_text: false,
            contrast: false,
        }
    }
}

/// What the menu is made of: the geometry rows and the text for each (the Slint `MenuRow` without y / h).
struct MenuItem {
    row: overlay::Row,
    text: String,
    value: String,
    sub: String,
    arrow: bool,
    danger: bool,
    dot: DotKind,
    icon: Option<IconId>,
}

fn menu_items(lang: Language, contrast: bool) -> Vec<MenuItem> {
    let item = |kind: Kind, text: &str| MenuItem {
        row: overlay::Row::new(kind),
        text: text.into(),
        value: String::new(),
        sub: String::new(),
        arrow: false,
        danger: false,
        dot: DotKind::None,
        icon: None,
    };
    let mut copy = item(Kind::Action, tr_in(Str::Copy, lang));
    copy.icon = Some(IconId::Link);
    copy.value = "Ctrl+C".into();
    let mut link = item(Kind::Action, tr_in(Str::CopyLink, lang));
    link.arrow = true;
    let mut mute = item(Kind::Switch, tr_in(Str::MuteOff, lang));
    mute.row.on = true;
    let mut daw = item(Kind::Check, tr_in(Str::ColoursFromDaw, lang));
    daw.row.on = true;
    daw.row.has_sub = true;
    daw.sub = trf_in(Str::NameInDaw, lang, "Audio 7");
    let mono = item(Kind::Check, tr_in(Str::ColoursMono, lang));
    let mut close = item(Kind::Action, tr_in(Str::Close, lang));
    close.danger = true;
    close.dot = DotKind::Rec;
    let mut off = item(Kind::Action, tr_in(Str::Cancel, lang));
    off.row.enabled = contrast;
    vec![
        item(Kind::Header, tr_in(Str::More, lang)),
        copy,
        link,
        item(Kind::Separator, ""),
        mute,
        daw,
        mono,
        close,
        off,
    ]
}

/// The list of the sample dropdown: its entries as Check rows, the first one selected.
fn dropdown_items(lang: Language) -> Vec<MenuItem> {
    [(Str::MonLabel, true), (Str::StrLabel, false)]
        .into_iter()
        .map(|(key, on)| {
            let mut row = overlay::Row::new(Kind::Check);
            row.on = on;
            MenuItem {
                row,
                text: tr_in(key, lang).into(),
                value: String::new(),
                sub: String::new(),
                arrow: false,
                danger: false,
                dot: DotKind::None,
                icon: None,
            }
        })
        .collect()
}

/// `trf` in a given language (`trf` itself uses the process-wide one).
fn trf_in(key: Str, lang: Language, arg: &str) -> String {
    tr_in(key, lang).replacen("%s", arg, 1)
}

fn texts(lang: Language) -> GalleryTexts {
    let t = |k: Str| SharedString::from(tr_in(k, lang));
    GalleryTexts {
        heading: t(Str::Settings),
        more: t(Str::More),
        you_hear: t(Str::MonLabel),
        viewers_hear: t(Str::StrLabel),
        theme_auto: t(Str::ThemeAuto),
        theme_light: t(Str::ThemeLight),
        theme_dark: t(Str::ThemeDark),
        ok: t(Str::Done),
        cancel: t(Str::Cancel),
        copy: t(Str::Copy),
        mute: t(Str::MuteOff),
        placeholder: t(Str::LanguageWord),
        on: t(Str::PowerOn),
        off: t(Str::PowerOff),
        value_db: crate::controls::ValueField::new(crate::controls::ValueKind::Db, -30.0, 6.0)
            .text(-3.0)
            .into(),
        value_ms: crate::controls::ValueField::new(crate::controls::ValueKind::Ms, 0.0, 500.0)
            .text(40.0)
            .into(),
        value_pan: crate::controls::ValueField::new(crate::controls::ValueKind::Pan, -100.0, 100.0)
            .text(-40.0)
            .into(),
        track_name: if lang == Language::Thai {
            "ไมค์ร้อง".into()
        } else {
            "Vocal".into()
        },
        tracks_tab: SharedString::from(tr_in(Str::TabTracks, lang).replacen("%d", "7", 1)),
        other_daws: t(Str::StepsOtherDaws),
        program: if lang == Language::Thai {
            "เบราว์เซอร์".into()
        } else {
            "Browser".into()
        },
        line_ok: t(Str::SetupSignalOk),
        line_warn: t(Str::SetupRateBad),
        line_danger: t(Str::SetupTracksBad),
        line_muted: t(Str::SetupNothing),
        copied: t(Str::Copied),
        icons_heading: t(Str::More),
        steps_heading: t(Str::SetupTitle),
        scroll_heading: t(Str::SetupTitle),
        qr_heading: t(Str::CopyLink),
    }
}

fn strings(keys: &[Str], lang: Language) -> ModelRc<SharedString> {
    // "%n tracks": the sample has 3
    ModelRc::new(VecModel::from(
        keys.iter()
            .map(|k| SharedString::from(tr_in(*k, lang).replace("%n", "3")))
            .collect::<Vec<_>>(),
    ))
}

struct MenuState {
    lang: Language,
    contrast: bool,
    items: Vec<MenuItem>,
    rows: Vec<overlay::Row>,
    layout: overlay::Layout,
    hot: Option<usize>,
}

/// The sample page: the window component plus the state of its menu.
pub struct Page {
    pub ui: Gallery,
    menu: Rc<RefCell<MenuState>>,
}

fn anchor_rect(a: &Anchor) -> Rect {
    Rect::new(a.x, a.y, a.width, a.height)
}

impl Page {
    /// Builds the page for a render at `scale` physical px per logical px (the backdrop image has that many pixels).
    pub fn new(o: &Options, scale: f32) -> Page {
        let ui = Gallery::new().expect("create the gallery window");
        let lang = o.language;
        ui.global::<Palette>().set_dark(o.look.dark);
        ui.global::<Motion>().set_reduce(o.look.reduce_motion);
        ui.global::<crate::ui::TextProbe>().set_hide(o.hide_text);
        ui.set_contrast(o.contrast);
        ui.set_t(texts(lang));
        ui.set_steps(strings(
            &[Str::SetupHubFix, Str::SetupTracksBad, Str::SetupRateFix],
            lang,
        ));
        ui.set_lines(strings(
            &[
                Str::SetupHubOk,
                Str::SetupTracksOk,
                Str::SetupSignalOk,
                Str::SetupRateOk,
                Str::SetupBypassOk,
                Str::SetupAheadOk,
                Str::SetupAppOk,
                Str::SetupShareOk,
            ],
            lang,
        ));
        let qr = qr::encode(QR_TEXT).expect("the sample link fits a QR code");
        ui.set_qr_commands(qr.path().into());
        ui.set_qr_modules(qr.total());
        let cards: Vec<CardRect> = CARDS
            .iter()
            .map(|c| CardRect {
                x: c.x,
                y: c.y,
                width: c.w,
                height: c.h,
            })
            .collect();
        ui.set_cards(ModelRc::new(VecModel::from(cards)));
        let glass = backdrop::Card {
            bounds: Rect::new(0.0, 0.0, 0.0, 0.0),
            radius: theme::radius::CARD,
        };
        let glass_cards: Vec<backdrop::Card> = CARDS
            .iter()
            .map(|b| backdrop::Card {
                bounds: *b,
                ..glass
            })
            .collect();
        let blobs = [
            backdrop::Blob {
                bounds: Rect::new(-60.0, -110.0, 480.0, 300.0),
                warm: true,
            },
            backdrop::Blob {
                bounds: Rect::new(WIDTH - 360.0, HEIGHT - 260.0, 420.0, 400.0),
                warm: false,
            },
        ];
        let image = backdrop::render(
            WIDTH,
            HEIGHT,
            scale,
            theme::palette(o.look.dark),
            &blobs,
            &glass_cards,
            theme::glass::ALPHA_DEFAULT,
        );
        ui.set_backdrop(image.to_slint());

        let state = Rc::new(RefCell::new(MenuState {
            lang,
            contrast: o.contrast,
            items: Vec::new(),
            rows: Vec::new(),
            layout: overlay::layout(&[]),
            hot: None,
        }));
        let page = Page { ui, menu: state };
        page.wire();
        if o.menu {
            page.open_menu();
        }
        if o.dropdown {
            page.open_dropdown();
        }
        if o.toast {
            page.ui.set_toast_text(tr_in(Str::Copied, lang).into());
            page.ui.set_toast_shown(true);
        }
        page
    }

    fn wire(&self) {
        let ui = self.ui.as_weak();
        let state = self.menu.clone();
        self.ui.on_menu_requested({
            let page = (ui.clone(), state.clone());
            move |a| {
                if let Some(ui) = page.0.upgrade() {
                    show_menu(
                        &ui,
                        &page.1,
                        menu_items(page.1.borrow().lang, page.1.borrow().contrast),
                        236.0,
                        anchor_rect(&a),
                    );
                }
            }
        });
        self.ui.on_menu_hovered({
            let (ui, state) = (ui.clone(), state.clone());
            move |i| {
                state.borrow_mut().hot = usize::try_from(i).ok();
                if let Some(ui) = ui.upgrade() {
                    ui.set_menu_hot(i);
                    ui.set_menu_keyboard(false);
                }
            }
        });
        self.ui.on_menu_step({
            let (ui, state) = (ui.clone(), state.clone());
            move |dir| {
                let hot = {
                    let mut s = state.borrow_mut();
                    s.hot = overlay::step(&s.rows, s.hot, dir);
                    s.hot
                };
                if let Some(ui) = ui.upgrade() {
                    ui.set_menu_hot(hot.map_or(-1, |h| h as i32));
                    ui.set_menu_keyboard(true);
                }
            }
        });
        self.ui.on_menu_activated({
            let (ui, state) = (ui.clone(), state.clone());
            move |i| {
                let Some(ui) = ui.upgrade() else { return };
                let mut s = state.borrow_mut();
                let Some(item) = usize::try_from(i).ok().filter(|i| *i < s.rows.len()) else {
                    return;
                };
                if !s.rows[item].selectable() {
                    return;
                }
                if s.rows[item].kind == Kind::Switch {
                    // a switch toggles and the menu stays open, like the mock
                    s.rows[item].on = !s.rows[item].on;
                    drop(s);
                    refresh_rows(&ui, &state);
                    return;
                }
                drop(s);
                ui.set_menu_open(false);
                ui.set_dropdown_open(false);
            }
        });
        self.ui.on_menu_dismissed({
            let ui = ui.clone();
            move || {
                if let Some(ui) = ui.upgrade() {
                    ui.set_menu_open(false);
                    ui.set_dropdown_open(false);
                }
            }
        });
        self.ui.on_dropdown_requested({
            let (ui, state) = (ui.clone(), state.clone());
            move |a| {
                if let Some(ui) = ui.upgrade() {
                    ui.set_dropdown_open(true);
                    let items = dropdown_items(state.borrow().lang);
                    show_menu(&ui, &state, items, a.width.max(160.0), anchor_rect(&a));
                }
            }
        });
    }

    /// Opens the "More" menu below its button.
    pub fn open_menu(&self) {
        let items = menu_items(self.menu.borrow().lang, self.menu.borrow().contrast);
        show_menu(
            &self.ui,
            &self.menu,
            items,
            236.0,
            anchor_rect(&self.ui.get_menu_anchor()),
        );
    }

    /// Opens the dropdown's list under its button, on the selected entry.
    pub fn open_dropdown(&self) {
        let a = self.ui.get_dropdown_anchor();
        self.ui.set_dropdown_open(true);
        let items = dropdown_items(self.menu.borrow().lang);
        show_menu(
            &self.ui,
            &self.menu,
            items,
            a.width.max(160.0),
            anchor_rect(&a),
        );
    }

    /// Row `i` of the open menu: its top-left corner in window coordinates and its size.
    pub fn menu_row_rect(&self, i: usize) -> Rect {
        let s = self.menu.borrow();
        Rect::new(
            self.ui.get_menu_x(),
            self.ui.get_menu_y() + s.layout.y[i],
            self.ui.get_menu_w(),
            s.layout.h[i],
        )
    }

    /// `true` while the switch row `i` of the menu is on.
    pub fn menu_row_on(&self, i: usize) -> bool {
        self.menu.borrow().rows[i].on
    }

    pub fn menu_len(&self) -> usize {
        self.menu.borrow().rows.len()
    }

    /// Shows the window, lets animations finish and renders it at `scale`.
    pub fn render(&self, h: &Headless, scale: f32) -> Frame {
        self.ui.show().expect("show the gallery window");
        h.settle();
        h.render(WIDTH, HEIGHT, scale)
    }
}

fn refresh_rows(ui: &Gallery, state: &Rc<RefCell<MenuState>>) {
    let s = state.borrow();
    let rows: Vec<MenuRow> = s
        .items
        .iter()
        .zip(&s.rows)
        .enumerate()
        .map(|(i, (it, row))| MenuRow {
            kind: match row.kind {
                Kind::Action => MenuKind::Action,
                Kind::Switch => MenuKind::Switch,
                Kind::Check => MenuKind::Check,
                Kind::Header => MenuKind::Header,
                Kind::Separator => MenuKind::Separator,
            },
            text: it.text.as_str().into(),
            value: it.value.as_str().into(),
            sub: it.sub.as_str().into(),
            arrow: it.arrow,
            on: row.on,
            danger: it.danger,
            enabled: row.enabled,
            dot: it.dot,
            has_icon: it.icon.is_some(),
            icon: it.icon.unwrap_or(IconId::Check),
            y: s.layout.y[i],
            h: s.layout.h[i],
        })
        .collect();
    ui.set_menu_rows(ModelRc::new(VecModel::from(rows)));
}

fn show_menu(
    ui: &Gallery,
    state: &Rc<RefCell<MenuState>>,
    items: Vec<MenuItem>,
    w: f32,
    anchor: Rect,
) {
    let (total, hot) = {
        let mut s = state.borrow_mut();
        s.rows = items.iter().map(|i| i.row).collect();
        s.items = items;
        s.layout = overlay::layout(&s.rows);
        s.hot = overlay::initial_hot(&s.rows);
        (s.layout.total, s.hot)
    };
    let card = overlay::place(Some(anchor), w, total, WIDTH, HEIGHT, true);
    ui.set_menu_w(w);
    ui.set_menu_x(card.x);
    ui.set_menu_y(card.y);
    ui.set_menu_h(card.h);
    ui.set_menu_hot(hot.map_or(-1, |h| h as i32));
    ui.set_menu_keyboard(false);
    refresh_rows(ui, state);
    ui.set_menu_open(true);
}

/// Number of rows currently in the menu model of `ui` (for tests).
pub fn menu_rows_in(ui: &Gallery) -> usize {
    ui.get_menu_rows().row_count()
}
