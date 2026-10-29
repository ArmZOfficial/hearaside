# Rust migration tooling

Run from this directory; `rust-toolchain.toml` pins Rust and its lint/format components.

```powershell
cargo test --workspace --locked
cargo clippy --workspace --all-targets --locked -- -D warnings
cargo fmt --all -- --check
cargo build --workspace --release --locked
cargo install cargo-deny --version 0.18.4 --locked
cargo deny --locked check licenses bans sources
```

`hs-tools` prints its version and tests the primitive/atomic layout assumptions
for 64-bit little-endian shared memory. `hs-compat-dump` loads a Windows x64 VST3
binary or bundle and records its factory, raw class IDs, and controller parameter
metadata. It uses the permissively licensed `vst3` bindings, not `vst3-sys`.
Neither tool replaces any C++ runtime code.

```powershell
cargo run --release --locked --bin hs-compat-dump -- 'C:/path/HEARASIDE Track.vst3'
cargo run --release --locked --bin hs-compat-dump -- --check ../compat/fixtures/plugins-1.0.10/track/vst3/metadata.json 'C:/path/HEARASIDE Track.vst3'
```

The dumper emits JSON to stdout; diagnostics and check results go to stderr.
`--check` compares JSON values and preserves class/parameter array order.
Each invocation sets unique bus, settings, and account names before loading the
plug-in, disables tunnels, and points account requests to loopback port 9.
Use trusted release binaries for baseline fixtures; a local rebuild is not release provenance.

`class_id_bytes` are the actual 16 TUID bytes in Windows memory order. Parameter
`plain_at_normalized_*` fields record the controller's SDK conversions. JUCE may
return normalized values there; `display_at_normalized_*` and `default_display`
capture the actual value text, including engineering units/formatting where supplied.
The full v14 shared-memory layout fixture lives under `compat/fixtures` separately.

The Windows MSVC build uses a static CRT, matching the existing C++ distribution.
Release builds keep unwinding so future FFI boundaries can catch panics.
