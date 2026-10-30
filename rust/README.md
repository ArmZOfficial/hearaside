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

`hs-net::rtc::RtcPeer` (R3.2) owns its UDP sockets and is driven by `poll()` on a
network worker (`hs_net::share` gives each session its own thread for it). Its
events carry the existing SDP/candidate fields. Do not construct, poll, send or drop this peer on an
audio thread. Hub callers set `HUB_UDP_FIRST..=HUB_UDP_LAST` (47830–47999).

The transport gathers host and UDP STUN candidates. Configured TURN and TLS STUN
URLs return `Unsupported`; they are not silently ignored. Browser-side TURN is
still possible, but relay-only and public Internet connections have not been
validated. The existing C++ Hub currently configures UDP STUN only.

To check the unchanged Send and Listen pages against the Rust transport in real
Chrome, install the existing `web/site` Node dependencies and Google Chrome, then
run from the repository root:

```powershell
cd rust
cargo build -p hs-net --example rtc-peer --locked
cd ..
node tests/web-friend/rtc.test.mjs rust/target/debug/examples/rtc-peer.exe
```

On Linux, omit `.exe`. The JSON-lines example and HTTP/WebSocket test fixture
exercise signaling, HRA3/Opus downlink, microphone uplink and channel closure;
they do not replace the production ShareServer or the R3.6 `hs-testkit`.

`hs_net::share::ShareServer` (R3.3) is the Hub's embedded web server: pages, WebSocket
and WebRTC audio for listeners, the send-in link and friends, and the cloudflared
quick tunnel. It takes an `Arc<hs_bus::shm::SharedMemory>` and runs on plain std
threads; none of them is an audio thread (the bus is touched only through the ring
functions). `tests/share.rs` runs it on loopback against a temp bus; the cloudflared
tests use `examples/fake-cloudflared.rs` (and `examples/share-host.rs` for the
"host crashes, tunnel dies" case on Windows), so they need no internet.

`hs_net::control::ControlServer` and `hs_net::rest` (R3.4) are the Hub's REST API
(`docs/rest-api.md`): an HTTP server on `127.0.0.1:47800..=47809` whose requests are
queued and answered by `ControlServer::service` on the host's message thread (2 s,
else 503), and the routes written against the `rest::HubControl` trait, which the
Rust Hub plug-in implements (`set_param` for the Hub parameters, which owns the host
change gesture; Track and App Audio commands through their mailboxes). `rest::RestService` is the
Hub's `serviceControl` (starts the server only for the bus owner with the switch on,
makes the key, retries a busy port every 5 s). Answers are written by `hs_net::json`
exactly as `juce::JSON::toString(var, true)` wrote them (`tests/rest.rs` checks the
number formatting against output of a real juce_core build). `tests/rest.rs` runs
every check of `ui-snapshot --test-rest` against a fake Hub plus the fixed C++ bugs
(`bug_*` tests); each test picks its own port (`ControlServer::with_ports(&[0])`), so
they run side by side and need no free 47800.
