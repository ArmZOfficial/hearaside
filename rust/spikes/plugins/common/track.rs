//! Parameters and audio shared by both spike plug-ins: a stereo pass-through whose headphone level
//! follows "You hear" and the level slider. Enough to exercise automation and state, nothing more.
use nice_plug::prelude::*;

#[derive(Params)]
pub struct TrackParams {
    #[id = "mon"]
    pub you_hear: BoolParam,
    #[id = "str"]
    pub viewers_hear: BoolParam,
    #[id = "gain"]
    pub headphone_gain: FloatParam,
    #[id = "vgain"]
    pub viewers_gain: FloatParam,
    #[id = "delay"]
    pub delay_ms: IntParam,
    /// Test switch only: the product follows the system theme.
    #[id = "dark"]
    pub dark: BoolParam,
}

impl Default for TrackParams {
    fn default() -> Self {
        let db = |name: &str| {
            FloatParam::new(name, 0.0, FloatRange::Linear { min: -60.0, max: 12.0 })
                .with_step_size(0.1)
                .with_unit(" dB")
        };
        Self {
            you_hear: BoolParam::new("You hear", false),
            viewers_hear: BoolParam::new("Viewers hear", true),
            headphone_gain: db("Headphone level"),
            viewers_gain: db("Viewers level"),
            delay_ms: IntParam::new("Viewers delay", 0, IntRange::Linear { min: 0, max: 500 }).with_unit(" ms"),
            dark: BoolParam::new("Dark theme", false),
        }
    }
}

pub fn process(params: &TrackParams, buffer: &mut Buffer) {
    let gain = if params.you_hear.value() { util::db_to_gain(params.headphone_gain.value()) } else { 0.0 };
    for samples in buffer.iter_samples() {
        for sample in samples {
            *sample *= gain;
        }
    }
}

pub const LAYOUTS: &[AudioIOLayout] = &[AudioIOLayout {
    main_input_channels: NonZeroU32::new(2),
    main_output_channels: NonZeroU32::new(2),
    ..AudioIOLayout::const_default()
}];
