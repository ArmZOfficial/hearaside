//! `hs-app`: what the HEARASIDE plug-ins share outside the audio path (R3.5): the per-user `Settings` file, the account
//! sign-in (`AccountManager`), the Hub's permanent-link registration (`ShareDirectory`) and the Thai / English UI
//! strings. No UI and no plug-in framework: callbacks come out as events the caller dispatches on the thread it chooses.
//!
//! Everything on disk or on the wire is what the C++ plug-ins read and write, so both can run side by side.
pub mod account;
pub mod directory;
mod http;
pub mod links;
pub mod paths;
mod pin;
mod properties;
pub mod settings;
pub mod strings;
mod sync;
pub mod valuetext;
mod vault;

pub use account::{
    AccountConfig, AccountEvent, AccountListener, AccountManager, ClientMeta, DeviceCode, Profile,
    ProfileError, ProfilePatch, State, Timings,
};
pub use directory::{DirectoryState, Resolved, ShareDirectory};
pub use http::{get_json, origin_of};
#[doc(hidden)]
pub use pin::detached_workers;
pub use properties::PropertiesFile;
pub use settings::{Appearance, Settings, ThemeMode};
pub use strings::{
    Language, STR_COUNT, Str, current_language, format_db, set_current_language, tr, tr_in, trf,
};
pub use vault::roundtrip as vault_roundtrip;
