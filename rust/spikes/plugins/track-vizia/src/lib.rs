//! R0.3 spike: the Track page as a VST3/CLAP plug-in with a Vizia editor (vizia-plug, baseview).
use nice_plug::prelude::*;
use std::sync::Arc;
use vizia_plug::vizia;

#[path = "../../common/track.rs"]
mod track;
#[path = "../../../ui-slint/src/backdrop.rs"]
mod backdrop;
#[path = "../../../ui-slint/src/tokens.rs"]
mod tokens;
#[path = "../../../ui-vizia/src/page.rs"]
mod page;
mod editor;

pub struct SpikeVizia {
    params: Arc<track::TrackParams>,
    state: Arc<vizia_plug::ViziaState>,
}

impl Default for SpikeVizia {
    fn default() -> Self {
        Self {
            params: Arc::new(track::TrackParams::default()),
            state: vizia_plug::ViziaState::new(|| (page::W as u32, page::H as u32)),
        }
    }
}

impl Plugin for SpikeVizia {
    const NAME: &'static str = "HEARASIDE Spike Vizia";
    const VENDOR: &'static str = "HEARASIDE";
    const URL: &'static str = "https://github.com/ArmZOfficial/hearaside";
    const EMAIL: &'static str = "";
    const VERSION: &'static str = env!("CARGO_PKG_VERSION");
    const AUDIO_IO_LAYOUTS: &'static [AudioIOLayout] = track::LAYOUTS;
    type SysExMessage = ();
    type BackgroundTask = ();

    fn params(&self) -> Arc<dyn Params> {
        self.params.clone()
    }

    fn editor(&mut self, _async_executor: AsyncExecutor<Self>) -> Option<Box<dyn Editor>> {
        editor::create(self.params.clone(), self.state.clone())
    }

    fn process(&mut self, buffer: &mut Buffer, _aux: &mut AuxiliaryBuffers, _context: &mut impl ProcessContext<Self>) -> ProcessStatus {
        track::process(&self.params, buffer);
        ProcessStatus::Normal
    }
}

impl ClapPlugin for SpikeVizia {
    const CLAP_ID: &'static str = "com.armz.hearaside.spike-vizia";
    const CLAP_DESCRIPTION: Option<&'static str> = Some("R0.3 UI spike (Vizia)");
    const CLAP_MANUAL_URL: Option<&'static str> = None;
    const CLAP_SUPPORT_URL: Option<&'static str> = None;
    const CLAP_FEATURES: &'static [ClapFeature] = &[ClapFeature::AudioEffect, ClapFeature::Stereo, ClapFeature::Utility];
}

impl Vst3Plugin for SpikeVizia {
    const VST3_CLASS_ID: [u8; 16] = *b"HsSpikeViziaR03a";
    const VST3_SUBCATEGORIES: &'static [Vst3SubCategory] = &[Vst3SubCategory::Fx, Vst3SubCategory::Tools];
}

nice_export_clap!(SpikeVizia);
nice_export_vst3!(SpikeVizia);
