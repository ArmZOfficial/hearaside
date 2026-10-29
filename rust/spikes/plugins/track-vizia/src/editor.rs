//! vizia-plug editor for the Vizia Track page. The footer shows how long the page took to build
//! (criterion 3). Switches push parameter changes; host automation is not pulled back into the UI.
use crate::{page, track};
use nice_plug::prelude::*;
use std::rc::Rc;
use std::sync::Arc;
use std::time::Instant;
use vizia_plug::vizia::prelude::*;
use vizia_plug::widgets::ParamEvent;
use vizia_plug::{ViziaState, ViziaTheming, create_vizia_editor};

fn set(cx: &mut EventContext, param: &BoolParam, value: bool) {
    cx.emit(ParamEvent::BeginSetParameter(param).upcast());
    cx.emit(ParamEvent::SetParameter(param, value).upcast());
    cx.emit(ParamEvent::EndSetParameter(param).upcast());
}

pub fn create(params: Arc<track::TrackParams>, state: Arc<ViziaState>) -> Option<Box<dyn Editor>> {
    create_vizia_editor(state, ViziaTheming::Builtin, move |cx, _gui| {
        let started = Instant::now();
        let dark = params.dark.value();
        page::install(cx, 2.0, dark); // backdrop drawn at 2x and stretched; good enough for a spike
        let you_hear = Signal::new(params.you_hear.value());
        let viewers_hear = Signal::new(params.viewers_hear.value());
        let footer = Signal::new(String::new());
        let (p1, p2) = (params.clone(), params.clone());
        page::page(
            cx,
            dark,
            you_hear,
            viewers_hear,
            footer,
            Rc::new(move |cx, v| set(cx, &p1.you_hear, v)),
            Rc::new(move |cx, v| set(cx, &p2.viewers_hear, v)),
        );
        footer.set(format!("Vizia · สร้างหน้าใน {} ms", started.elapsed().as_millis()));
    })
}
