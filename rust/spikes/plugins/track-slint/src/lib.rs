//! R0.3 spike: the Track page as a VST3/CLAP plug-in with a Slint editor (slint-baseview, OpenGL).
use nice_plug::prelude::*;
use std::sync::Arc;

#[path = "../../common/track.rs"]
mod track;
#[path = "../../../ui-slint/src/backdrop.rs"]
mod backdrop;
#[path = "../../../ui-slint/src/tokens.rs"]
mod tokens;
mod editor;

#[derive(Default)]
pub struct SpikeSlint {
    params: Arc<track::TrackParams>,
}

impl Plugin for SpikeSlint {
    const NAME: &'static str = "HEARASIDE Spike Slint";
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
        Some(Box::new(editor::SlintEditor::new(self.params.clone())))
    }

    fn process(&mut self, buffer: &mut Buffer, _aux: &mut AuxiliaryBuffers, _context: &mut impl ProcessContext<Self>) -> ProcessStatus {
        track::process(&self.params, buffer);
        ProcessStatus::Normal
    }
}

impl ClapPlugin for SpikeSlint {
    const CLAP_ID: &'static str = "com.armz.hearaside.spike-slint";
    const CLAP_DESCRIPTION: Option<&'static str> = Some("R0.3 UI spike (Slint)");
    const CLAP_MANUAL_URL: Option<&'static str> = None;
    const CLAP_SUPPORT_URL: Option<&'static str> = None;
    const CLAP_FEATURES: &'static [ClapFeature] = &[ClapFeature::AudioEffect, ClapFeature::Stereo, ClapFeature::Utility];
}

impl Vst3Plugin for SpikeSlint {
    const VST3_CLASS_ID: [u8; 16] = *b"HsSpikeSlintR03a";
    const VST3_SUBCATEGORIES: &'static [Vst3SubCategory] = &[Vst3SubCategory::Fx, Vst3SubCategory::Tools];
}

nice_export_clap!(SpikeSlint);
nice_export_vst3!(SpikeSlint);
