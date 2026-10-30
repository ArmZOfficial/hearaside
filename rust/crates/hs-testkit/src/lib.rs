//! `hs-testkit` (PLAN R3.6): the Rust stack driven the way the C++ `ui-snapshot` tool drives the plug-ins, so the existing
//! Node tests (`tests/web-friend/*.mjs`) and ctest names (`rest_api`, `friends_room`, `share_directory`, `ui_values`,
//! `friend_page`, `friend_wire`, `friend_opus`) run against it unchanged.
//!
//! * [`hub::TestHub`]: a Hub without an editor or a host: `HubEngine` + friends room + `ShareServer` + REST API + takes.
//! * [`track::TestTrack`]: a Track without an editor: the publisher, the parameters and the mailbox.
//! * [`checks`]: one module per `--test-*` / `--serve-friend` flag.
//!
//! Pieces that the Rust plug-ins (R6 to R8) will also need are marked as candidates in their module docs; nothing here is
//! shipped in a plug-in.
pub mod checks;
pub mod friend_room;
pub mod hub;
pub mod recorder;
pub mod report;
pub mod track;
pub mod ws;
