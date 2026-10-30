//! HEARASIDE engines: the real-time core of the Track and Hub plug-ins, free of any UI or plug-in framework.
//! Rust port of `libs/ssengine` (MIT).
pub mod friend_reader;
pub mod hub_engine;
pub mod measure;
mod pairing;
pub mod track_publisher;
