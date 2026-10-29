//! The Track page in Vizia, shared by the standalone spike and the plug-in spike.
use crate::{backdrop, tokens};
use crate::vizia::prelude::*;
use std::rc::Rc;

pub const W: f32 = 380.0;
pub const H: f32 = 640.0;

/// design/tokens.json, light/dark subset (same keys as the Slint spike).
fn palette(dark: bool) -> String {
    let (l, d): (&[(&str, &str)], &[(&str, &str)]) = (
        &[
            ("paper", "#F3F3F1"), ("ink", "#181818"), ("on-ink", "#FFFFFF"), ("ink2", "#3E3E3A"), ("graphite", "#5A5A56"),
            ("muted", "#8A8A84"), ("glass", "rgba(255,255,255,0.62)"), ("glass-edge", "rgba(255,255,255,0.85)"),
            ("inset", "rgba(240,240,237,0.90)"), ("off-bg", "rgba(255,255,255,0.80)"), ("chip-bg", "rgba(255,255,255,0.72)"),
            ("hairline2", "rgba(24,24,24,0.08)"), ("hairline3", "rgba(24,24,24,0.12)"), ("switch-off", "rgba(24,24,24,0.18)"),
            ("slider-rail", "rgba(24,24,24,0.14)"), ("shadow", "rgba(24,24,24,0.06)"), ("track-shade3", "#8C8C86"),
            ("thumb", "#FFFFFF"), ("ok", "#0F7553"),
        ],
        &[
            ("paper", "#151514"), ("ink", "#EDEDEA"), ("on-ink", "#151514"), ("ink2", "#CFCFCA"), ("graphite", "#A3A39D"),
            ("muted", "#6E6E69"), ("glass", "rgba(38,38,36,0.62)"), ("glass-edge", "rgba(255,255,255,0.10)"),
            ("inset", "rgba(255,255,255,0.055)"), ("off-bg", "rgba(255,255,255,0.05)"), ("chip-bg", "rgba(255,255,255,0.06)"),
            ("hairline2", "rgba(255,255,255,0.10)"), ("hairline3", "rgba(255,255,255,0.16)"), ("switch-off", "rgba(255,255,255,0.20)"),
            ("slider-rail", "rgba(255,255,255,0.16)"), ("shadow", "rgba(0,0,0,0.30)"), ("track-shade3", "#8C8C86"),
            ("thumb", "#EDEDEA"), ("ok", "#3CCB7F"),
        ],
    );
    (if dark { d } else { l }).iter().map(|(k, v)| format!("    --{k}: {v};\n")).collect()
}

pub fn stylesheet(dark: bool) -> String {
    format!(
        r#"
:root {{
{vars}    font-family: "Anuphan";
    background-color: var(--paper);
    background-image: url("backdrop");
    background-size: 100% 100%;
    color: var(--ink);
}}
.page {{ padding: 12px; gap: 10px; }}
.glass {{
    background-color: var(--glass); border-width: 1px; border-color: var(--glass-edge);
    backdrop-filter: blur(18px); shadow: 0px 12px 32px var(--shadow);
}}
.header {{ height: 54px; corner-radius: 18px; padding-left: 16px; padding-right: 9px; alignment: left; gap: 8px; }}
.wordmark {{ width: 1s; height: auto; alignment: left; gap: -2px; }}
.wordmark .big {{ font-size: 12.5px; font-weight: 600; }}
.wordmark .small {{ font-size: 8px; font-weight: 500; color: var(--graphite); }}
.chip {{ height: 30px; width: auto; corner-radius: 15px; background-color: var(--chip-bg); border-width: 1px; border-color: var(--hairline2);
         padding-left: 12px; padding-right: 14px; gap: 8px; alignment: left; }}
.dot {{ width: 7px; height: 7px; corner-radius: 50%; background-color: var(--ok); }}
.chip label {{ font-size: 12.5px; }}
.square {{ width: 36px; height: 36px; corner-radius: 11px; background-color: var(--chip-bg); border-width: 1px; border-color: var(--hairline2); alignment: center; }}
.square svg {{ width: 18px; height: 18px; }}
.card {{ height: 1s; corner-radius: 20px; padding-left: 18px; padding-right: 18px; padding-top: 16px; padding-bottom: 12px; gap: 12px; }}
.name-row {{ height: 26px; gap: 10px; alignment: left; }}
.track-dot {{ width: 10px; height: 10px; corner-radius: 50%; background-color: var(--track-shade3); }}
.name {{ font-size: 16.5px; font-weight: 600; width: 1s; }}
.input-label {{ font-size: 11px; color: var(--graphite); }}
.meter {{ width: 80px; height: 4px; corner-radius: 2px; background-color: var(--slider-rail); }}
.meter-fill {{ width: 85%; height: 4px; corner-radius: 2px; background-color: var(--ink); }}
.rows {{ height: auto; gap: 8px; }}
.row {{ height: 60px; corner-radius: 14px; background-color: var(--inset); border-width: 1px; border-color: var(--hairline2);
        padding-left: 10px; padding-right: 12px; gap: 13px; alignment: left; }}
.tile {{ width: 38px; height: 38px; corner-radius: 11px; background-color: var(--off-bg); border-width: 1px; border-color: var(--hairline2); alignment: center; }}
.tile svg {{ width: 20px; height: 20px; position-type: absolute; left: 1s; right: 1s; top: 1s; bottom: 1s; fill: var(--graphite); }}
.tile .on-icon {{ fill: var(--on-ink); }}
.square svg {{ fill: var(--ink); }}
.tile.on {{ background-color: var(--ink); border-width: 0px; }}
.row .text {{ width: 1s; height: auto; alignment: left; }}
.title {{ font-size: 14.5px; font-weight: 600; }}
.caption {{ font-size: 12.5px; color: var(--graphite); }}
switch {{ width: 46px; height: 26px; background-color: var(--switch-off); }}
switch .thumb {{ width: 22px; height: 22px; top: 2px; left: 2px; background-color: #FFFFFF; transition: left 160ms; }}
switch:checked {{ background-color: var(--ink); }}
switch:checked .thumb {{ left: 22px; background-color: var(--on-ink); }}
.summary {{ height: 36px; corner-radius: 12px; border-width: 1px; border-style: dashed; border-color: var(--hairline3); padding-left: 12px; alignment: left; }}
.summary label {{ font-size: 12.5px; color: var(--ink2); }}
.field {{ height: auto; gap: 14px; padding-bottom: 4px; }}
.field-head {{ height: auto; }}
.field-head .label {{ font-size: 13.5px; font-weight: 600; width: 1s; }}
.field-head .value {{ font-size: 12.5px; color: var(--graphite); padding-right: 7px; }}
.field .caption {{ font-size: 12px; }}
slider {{ height: 18px; }}
slider .track {{ background-color: var(--slider-rail); height: 3.5px; }}
slider .range {{ background-color: var(--ink); }}
slider .thumb {{ width: 17px; height: 17px; corner-radius: 50%; background-color: var(--thumb); border-width: 1px; border-color: var(--hairline3);
                 shadow: 0px 1px 3px var(--shadow); }}
.dim slider .range {{ background-color: var(--muted); }}
.dim slider .thumb {{ border-color: var(--hairline2); opacity: 0.75; }}
.spacer {{ height: 1s; }}
.footer-line {{ height: 1px; background-color: var(--hairline2); }}
.footer {{ font-size: 11px; color: var(--graphite); }}
"#,
        vars = palette(dark)
    )
}

/// The 24-unit icons of plugins/common/ui/Icons.cpp as SVG (Vizia's SVG has no currentColor: the colour is baked in).
fn icon(paths: &str, colour: &str) -> Vec<u8> {
    format!(
        r#"<svg xmlns="http://www.w3.org/2000/svg" viewBox="0 0 24 24" fill="none" stroke="{colour}" stroke-width="1.7" stroke-linecap="round" stroke-linejoin="round"><path d="{paths}"/></svg>"#
    )
    .into_bytes()
}

fn ink(dark: bool) -> (&'static str, &'static str, &'static str) {
    // (ink, onInk, graphite)
    if dark { ("#EDEDEA", "#151514", "#A3A39D") } else { ("#181818", "#FFFFFF", "#5A5A56") }
}
const HEADPHONES: &str = "M4 17v-5a8 8 0 0 1 16 0v5 M4.8 14h1a1.8 1.8 0 0 1 1.8 1.8v3.4a1.8 1.8 0 0 1-1.8 1.8h-1a1.8 1.8 0 0 1-1.8-1.8v-3.4a1.8 1.8 0 0 1 1.8-1.8z M18.3 14h1a1.8 1.8 0 0 1 1.8 1.8v3.4a1.8 1.8 0 0 1-1.8 1.8h-1a1.8 1.8 0 0 1-1.8-1.8v-3.4a1.8 1.8 0 0 1 1.8-1.8z";
const BROADCAST: &str = "M15.5 8.5a5 5 0 0 1 0 7 M8.5 15.5a5 5 0 0 1 0-7 M18.4 5.6a9 9 0 0 1 0 12.8 M5.6 18.4a9 9 0 0 1 0-12.8 M14 12a2 2 0 1 1-4 0a2 2 0 1 1 4 0";
const SLIDERS: &str = "M4 7h10 M18 7h2 M4 17h4 M12 17h8 M18 7a2 2 0 1 1-4 0a2 2 0 1 1 4 0 M12 17a2 2 0 1 1-4 0a2 2 0 1 1 4 0";

fn toggle_row(cx: &mut Context, dark: bool, title: &'static str, caption: &'static str, broadcast: bool, on: Signal<bool>, changed: Rc<dyn Fn(&mut EventContext, bool)>) {
    HStack::new(cx, move |cx| {
        HStack::new(cx, move |cx| {
            let (_, on_ink, graphite) = ink(dark);
            let (on_paths, off_paths) = if broadcast {
                (BROADCAST.to_string(), BROADCAST.to_string())
            } else {
                (HEADPHONES.to_string(), format!("{HEADPHONES} M3 3L21 21"))
            };
            let off = Memo::new(move |_| !on.get());
            // Vizia recolours an SVG with CSS `fill`; `.tile.on svg` did not restyle the child, so two icons are swapped
            Svg::new(cx, icon(&on_paths, on_ink)).class("on-icon").display(on);
            Svg::new(cx, icon(&off_paths, graphite)).display(off);
        })
        .class("tile")
        .toggle_class("on", on);
        VStack::new(cx, |cx| {
            Label::new(cx, title).class("title");
            Label::new(cx, caption).class("caption");
        })
        .class("text");
        Switch::new(cx, on).on_toggle(move |cx| {
            on.set(!on.get());
            changed(cx, on.get());
        });
    })
    .class("row");
}

fn field(cx: &mut Context, label: &'static str, value: &'static str, position: f32, dim: Memo<bool>, caption: Option<&'static str>) {
    VStack::new(cx, move |cx| {
        HStack::new(cx, move |cx| {
            Label::new(cx, label).class("label");
            Label::new(cx, value).class("value");
        })
        .class("field-head");
        let v = Signal::new(position);
        Slider::new(cx, v).on_change(move |_, x| v.set(x));
        if let Some(c) = caption {
            Label::new(cx, c).class("caption");
        }
    })
    .class("field")
    .toggle_class("dim", dim);
}

pub fn backdrop_png(scale: f32, dark: bool) -> &'static [u8] {
    let img = backdrop::render(W, H, scale, &tokens::colors(dark), &[], 18.0);
    let rgba: Vec<u8> = img
        .px
        .iter()
        .flat_map(|p| {
            let a = p[3].max(1e-6);
            [p[0] / a, p[1] / a, p[2] / a, p[3]].map(|v| (v.clamp(0.0, 1.0) * 255.0).round() as u8)
        })
        .collect();
    let mut out = Vec::new();
    {
        let mut enc = png::Encoder::new(&mut out, img.w as u32, img.h as u32);
        enc.set_color(png::ColorType::Rgba);
        enc.write_header().unwrap().write_image_data(&rgba).unwrap();
    }
    Box::leak(out.into_boxed_slice())
}

/// Fonts, backdrop and stylesheet for one window; call once per window before `page`.
pub fn install(cx: &mut Context, scale: f32, dark: bool) {
    cx.add_font_mem(include_bytes!("../../../../external/fonts/Anuphan-400.ttf"));
    cx.add_font_mem(include_bytes!("../../../../external/fonts/Anuphan-500.ttf"));
    cx.add_font_mem(include_bytes!("../../../../external/fonts/Anuphan-600.ttf"));
    cx.load_image("backdrop", backdrop_png(scale, dark), ImageRetentionPolicy::Forever);
    cx.add_stylesheet(CSS::from_string(&stylesheet(dark))).expect("stylesheet");
}

/// The Track page. `on_you` / `on_viewers` run when the user flips a switch.
pub fn page(
    cx: &mut Context,
    dark: bool,
    you_hear: Signal<bool>,
    viewers_hear: Signal<bool>,
    footer: Signal<String>,
    on_you: Rc<dyn Fn(&mut EventContext, bool)>,
    on_viewers: Rc<dyn Fn(&mut EventContext, bool)>,
) {
        let summary = Memo::new(move |_| {
            match (you_hear.get(), viewers_hear.get()) {
                (true, true) => "ทั้งคุณและคนดูได้ยินแทร็กนี้",
                (false, true) => "คุณจะไม่ได้ยินแทร็กนี้ในหูฟัง แต่คนดูได้ยินตามปกติ",
                _ => "คุณได้ยินคนเดียว คนดูจะไม่ได้ยินแทร็กนี้",
            }
            .to_string()
        });
        let mon_dim = Memo::new(move |_| !you_hear.get());
        let str_dim = Memo::new(move |_| !viewers_hear.get());
        VStack::new(cx, move |cx| {
            HStack::new(cx, |cx| {
                VStack::new(cx, |cx| {
                    Label::new(cx, "H E A R A S I D E").class("big");
                    Label::new(cx, "T R A C K").class("small");
                })
                .class("wordmark");
                HStack::new(cx, |cx| {
                    Element::new(cx).class("dot");
                    Label::new(cx, "เชื่อมกับ Hub แล้ว");
                })
                .class("chip");
                HStack::new(cx, |cx| {
                    Svg::new(cx, icon(SLIDERS, ink(dark).0));
                })
                .class("square");
            })
            .class("header")
            .class("glass");
            VStack::new(cx, move |cx| {
                HStack::new(cx, |cx| {
                    Element::new(cx).class("track-dot");
                    Label::new(cx, "Vocal").class("name");
                    Label::new(cx, "สัญญาณเข้า").class("input-label");
                    HStack::new(cx, |cx| {
                        Element::new(cx).class("meter-fill");
                    })
                    .class("meter");
                })
                .class("name-row");
                VStack::new(cx, move |cx| {
                    toggle_row(cx, dark, "คุณได้ยินแทร็กนี้", "ในหูฟังของคุณ", false, you_hear, on_you.clone());
                    toggle_row(cx, dark, "คนดูได้ยินแทร็กนี้", "ส่งไปไลฟ์ผ่าน OBS", true, viewers_hear, on_viewers.clone());
                })
                .class("rows");
                HStack::new(cx, move |cx| {
                    Label::new(cx, summary);
                })
                .class("summary");
                field(cx, "ระดับในหูฟัง", "0.0 dB", 0.83, mon_dim, None);
                field(cx, "ระดับเสียงฝั่งคนดู", "0.0 dB", 0.83, str_dim, None);
                field(cx, "หน่วงเวลาฝั่งคนดู", "0 ms", 0.0, str_dim, Some("เหมือน Sync Offset ใน OBS เฉพาะแทร็กนี้ หูฟังของคุณไม่ถูกหน่วง"));
                Element::new(cx).class("spacer");
                Element::new(cx).class("footer-line");
                Label::new(cx, footer).class("footer");
            })
            .class("card")
            .class("glass");
        })
        .class("page");
}
