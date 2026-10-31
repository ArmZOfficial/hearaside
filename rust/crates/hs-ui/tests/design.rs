//! `design_tokens_in_sync` and `design_contrast` (R5.1): the C++ side runs `design/gen_theme.py --check` / `--contrast` as CTest
//! tests; these are the Rust twins. They read `design/tokens.json` directly, so a `theme.rs` that drifted from it fails here even
//! where Python is missing.
use hs_ui::theme::{self, Palette, Rgba};
use serde_json::Value;
use std::path::PathBuf;
use std::process::Command;

fn repo() -> PathBuf {
    PathBuf::from(env!("CARGO_MANIFEST_DIR")).join("../../..")
}

fn tokens() -> Value {
    let text =
        std::fs::read_to_string(repo().join("design/tokens.json")).expect("design/tokens.json");
    serde_json::from_str(&text).expect("tokens.json is JSON")
}

/// `gen_theme.py argb()`: `#RRGGBB` or `rgba(r,g,b,a)` with the alpha rounded half to even like Python's `round`.
fn parse_colour(value: &str) -> Rgba {
    let v = value.trim();
    if let Some(hex) = v.strip_prefix('#') {
        let n = u32::from_str_radix(hex, 16).expect("hex colour");
        return Rgba {
            r: (n >> 16) as u8,
            g: (n >> 8) as u8,
            b: n as u8,
            a: 255,
        };
    }
    let inner = v
        .strip_prefix("rgba(")
        .and_then(|r| r.strip_suffix(')'))
        .unwrap_or_else(|| panic!("unsupported colour {v}"));
    let p: Vec<&str> = inner.split(',').map(str::trim).collect();
    Rgba {
        r: p[0].parse().unwrap(),
        g: p[1].parse().unwrap(),
        b: p[2].parse().unwrap(),
        a: (p[3].parse::<f64>().unwrap() * 255.0).round_ties_even() as u8,
    }
}

fn palette_of(mode: &str) -> &'static Palette {
    theme::palette(mode == "dark")
}

#[test]
fn design_tokens_in_sync() {
    let t = tokens();
    for mode in ["light", "dark"] {
        let json = t["color"][mode].as_object().unwrap();
        let fields = palette_of(mode).fields();
        assert_eq!(fields.len(), json.len(), "{mode}: token count");
        for (name, got) in fields {
            let want = json
                .get(name)
                .unwrap_or_else(|| panic!("{mode}.{name} is not in tokens.json"));
            assert_eq!(got, parse_colour(want.as_str().unwrap()), "{mode}.{name}");
        }
    }
    let mut seen = 0;
    for group in ["radius", "space", "type", "glass", "motion", "layout"] {
        for (name, value) in t[group]
            .as_object()
            .unwrap()
            .iter()
            .filter(|(k, _)| !k.starts_with('$'))
        {
            let got = theme::NUMBERS
                .iter()
                .find(|(g, n, _)| *g == group && n == name)
                .unwrap_or_else(|| panic!("{group}.{name} is missing from theme.rs"));
            assert_eq!(got.2, value.as_f64().unwrap() as f32, "{group}.{name}");
            seen += 1;
        }
    }
    assert_eq!(
        seen,
        theme::NUMBERS.len(),
        "theme.rs has a number tokens.json does not"
    );
    // the consts are the table
    assert_eq!(theme::radius::CARD, 22.0);
    assert_eq!(theme::typography::CAPTION, 12.0);
    assert_eq!(theme::glass::ALPHA_DEFAULT, 0.62);
    assert_eq!(theme::layout::MIN_TARGET, 32.0);
}

/// The generator itself: `gen_theme.py --check` also covers Theme.h, tokens.css, theme.rs and theme.slint byte for byte.
/// Skipped (and said so) where there is no Python.
#[test]
fn design_tokens_in_sync_with_the_generator() {
    let script = repo().join("design/gen_theme.py");
    for python in ["python", "python3", "py"] {
        if let Ok(out) = Command::new(python).arg(&script).arg("--check").output() {
            let text = String::from_utf8_lossy(&out.stdout);
            assert!(
                out.status.success(),
                "gen_theme.py --check failed:\n{text}{}",
                String::from_utf8_lossy(&out.stderr)
            );
            return;
        }
    }
    eprintln!("no Python found: gen_theme.py --check not run");
}

// ---- WCAG contrast of the tokens (gen_theme.py contrast_problems) ------------------------------------------------------------------

fn rgba(c: Rgba) -> [f64; 4] {
    [
        f64::from(c.r) / 255.0,
        f64::from(c.g) / 255.0,
        f64::from(c.b) / 255.0,
        f64::from(c.a) / 255.0,
    ]
}

fn over(top: [f64; 4], bottom: [f64; 4]) -> [f64; 4] {
    let a = top[3];
    [
        top[0] * a + bottom[0] * (1.0 - a),
        top[1] * a + bottom[1] * (1.0 - a),
        top[2] * a + bottom[2] * (1.0 - a),
        1.0,
    ]
}

fn luminance(c: [f64; 4]) -> f64 {
    let f = |x: f64| {
        if x <= 0.03928 {
            x / 12.92
        } else {
            ((x + 0.055) / 1.055).powf(2.4)
        }
    };
    0.2126 * f(c[0]) + 0.7152 * f(c[1]) + 0.0722 * f(c[2])
}

fn ratio(a: [f64; 4], b: [f64; 4]) -> f64 {
    let (la, lb) = (luminance(a), luminance(b));
    (la.max(lb) + 0.05) / (la.min(lb) + 0.05)
}

fn token(p: &Palette, name: &str) -> [f64; 4] {
    rgba(
        p.fields()
            .iter()
            .find(|(n, _)| *n == name)
            .unwrap_or_else(|| panic!("no token {name}"))
            .1,
    )
}

/// "inset>glass>paper": each token composited over the ones after it.
fn layer(p: &Palette, stack: &str) -> [f64; 4] {
    let names: Vec<&str> = stack.split('>').collect();
    let mut colour = token(p, names[names.len() - 1]);
    for n in names[..names.len() - 1].iter().rev() {
        colour = over(token(p, n), colour);
    }
    colour
}

fn contrast_problems(t: &Value) -> Vec<String> {
    let rules = &t["contrast"];
    let min = rules["minRatio"].as_f64().unwrap();
    let strings = |key: &str| -> Vec<String> {
        rules[key]
            .as_array()
            .unwrap()
            .iter()
            .map(|v| v.as_str().unwrap().to_string())
            .collect()
    };
    let (text, backgrounds) = (strings("text"), strings("backgrounds"));
    let mut pairs: Vec<(String, String)> = text
        .iter()
        .flat_map(|fg| backgrounds.iter().map(move |bg| (fg.clone(), bg.clone())))
        .collect();
    pairs.extend(rules["pairs"].as_array().unwrap().iter().map(|p| {
        (
            p[0].as_str().unwrap().to_string(),
            p[1].as_str().unwrap().to_string(),
        )
    }));
    let mut problems = Vec::new();
    for mode in ["light", "dark"] {
        let p = palette_of(mode);
        for (fg, bg) in &pairs {
            let back = layer(p, bg);
            let r = ratio(over(token(p, fg), back), back);
            if r < min {
                problems.push(format!("{mode}: {fg} on {bg} = {r:.2}:1 (needs {min}:1)"));
            }
        }
    }
    for name in strings("disabledOnly") {
        if text.contains(&name) {
            problems.push(format!("{name} is listed as disabled-only and as text"));
        }
    }
    problems
}

#[test]
fn design_contrast() {
    let t = tokens();
    let problems = contrast_problems(&t);
    assert!(problems.is_empty(), "{}", problems.join("\n"));
}

#[test]
fn design_contrast_checker_fails_on_a_bad_pair() {
    // muted is for disabled controls only because it does not reach 4.5:1 on paper
    let p = &theme::LIGHT;
    let r = ratio(
        over(token(p, "muted"), token(p, "paper")),
        token(p, "paper"),
    );
    assert!(r < 4.5, "muted on paper = {r:.2}");
    let mut t = tokens();
    t["contrast"]["text"]
        .as_array_mut()
        .unwrap()
        .push(Value::String("muted".into()));
    t["contrast"]["disabledOnly"] = serde_json::json!(["muted"]);
    let problems = contrast_problems(&t);
    assert!(
        problems
            .iter()
            .any(|p| p.starts_with("light: muted on paper")),
        "{problems:?}"
    );
    assert!(
        problems
            .iter()
            .any(|p| p.contains("disabled-only and as text")),
        "{problems:?}"
    );
}
