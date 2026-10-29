// Generated from design/tokens.json (straight alpha).
pub fn colors(dark: bool) -> crate::backdrop::Colors {
    use crate::backdrop::Rgba;
    let pick = |l: Rgba, d: Rgba| if dark { d } else { l };
    crate::backdrop::Colors {
        paper: pick(Rgba([0.9529, 0.9529, 0.9451, 1.0]), Rgba([0.0824, 0.0824, 0.0784, 1.0])),
        grid: pick(Rgba([0.0941, 0.0941, 0.0941, 0.0500]), Rgba([1.0000, 1.0000, 1.0000, 0.0450])),
        blob_warm: pick(Rgba([0.4392, 0.4157, 0.3765, 0.4200]), Rgba([0.5882, 0.5412, 0.4627, 0.3000])),
        blob_cool: pick(Rgba([0.1569, 0.1569, 0.1490, 0.2000]), Rgba([0.3529, 0.3529, 0.3373, 0.2200])),
        glass: pick(Rgba([1.0000, 1.0000, 1.0000, 0.6200]), Rgba([0.1490, 0.1490, 0.1412, 0.6200])),
        glass_edge: pick(Rgba([1.0000, 1.0000, 1.0000, 0.8500]), Rgba([1.0000, 1.0000, 1.0000, 0.1000])),
        shadow: pick(Rgba([0.0941, 0.0941, 0.0941, 0.0600]), Rgba([0.0000, 0.0000, 0.0000, 0.3000])),
    }
}
