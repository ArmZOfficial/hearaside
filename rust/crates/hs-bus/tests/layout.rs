//! R1.2: every struct and field of the Rust protocol matches the C++ v14 layout recorded in
//! `compat/fixtures/shm-v14-layout.json` (offset and size of each field, struct size and alignment).
use hs_bus::protocol::{self, BusLayout};
use std::collections::BTreeSet;

fn fixture() -> serde_json::Value {
    let path = concat!(
        env!("CARGO_MANIFEST_DIR"),
        "/../../../compat/fixtures/shm-v14-layout.json"
    );
    serde_json::from_str(&std::fs::read_to_string(path).expect("read layout fixture"))
        .expect("parse layout fixture")
}

#[test]
fn every_field_matches_the_cpp_layout() {
    let fixture = fixture();
    let structs = fixture["structs"].as_object().expect("structs");
    let mut seen = BTreeSet::new();
    for (name, field, offset, size) in protocol::layout_table() {
        let expected = &structs[name]["fields"][field];
        assert_eq!(
            expected["offset"].as_u64(),
            Some(offset as u64),
            "{name}::{field} offset"
        );
        assert_eq!(
            expected["size"].as_u64(),
            Some(size as u64),
            "{name}::{field} size"
        );
        seen.insert((name.to_owned(), field.to_owned()));
    }
    let mut expected_fields = BTreeSet::new();
    for (name, info) in structs {
        for field in info["fields"].as_object().expect("fields").keys() {
            expected_fields.insert((name.clone(), field.clone()));
        }
    }
    assert_eq!(
        seen, expected_fields,
        "every fixture field has a Rust counterpart and no more"
    );
    assert_eq!(seen.len(), 157);
}

#[test]
fn segment_size_and_identity_match() {
    let fixture = fixture();
    let constants = &fixture["constants"];
    assert_eq!(
        constants["kLayoutSize"].as_u64(),
        Some(core::mem::size_of::<BusLayout>() as u64)
    );
    assert_eq!(protocol::LAYOUT_SIZE, core::mem::size_of::<BusLayout>());
    assert_eq!(
        constants["kMagic"].as_u64(),
        Some(u64::from(protocol::MAGIC))
    );
    assert_eq!(
        constants["kProtocolVersion"].as_u64(),
        Some(u64::from(protocol::PROTOCOL_VERSION))
    );
    assert_eq!(
        constants["kBeaconMagic"].as_u64(),
        Some(u64::from(protocol::BEACON_MAGIC))
    );
}
