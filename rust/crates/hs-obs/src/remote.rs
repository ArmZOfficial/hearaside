//! Hook point for R4.2: `obs/hearaside-obs/src/remote.{h,cpp}` (`hearaside::BusClient`), the commands OBS hotkeys
//! send to the Hub through the bus (`post_remote` with `RemoteParam::Panic` / `Preview`).
//!
//! NOT PORTED YET. The two hotkeys are registered under their final names (so scene collections and Stream Deck
//! bindings load) but pressing them does nothing in this build until R4.2 fills these functions in.
//! They run on OBS's hotkey thread, never on the feed thread.

/// Hotkey `hearaside.mute_stream`: toggle the Hub's panic (mute the viewers' stream) on bus `bus`.
pub fn toggle_panic(_bus: &str) {}

/// Hotkey `hearaside.preview`: toggle "hear the viewers' mix" in the Hub on bus `bus`.
pub fn toggle_preview(_bus: &str) {}

/// `obs_module_unload`: drop the bus mappings `BusClient` keeps open.
pub fn release_all() {}
