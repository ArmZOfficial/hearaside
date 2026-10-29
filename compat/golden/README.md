# C++ source audio baselines

These references come from the unchanged C++ sources at `7f4eccc`, not from the
released plug-in binaries. `core/` contains 20 f32 captures and manifest/source
provenance; `vocal/` contains four mono inputs and 20 VocalChain outputs plus its
manifest. The top-level `provenance.json` records all 47 corpus files, their
sizes/SHA-256, the VocalChain/compiler/JUCE identity and selected DSP source hashes.

Inputs are original deterministic sine, seeded noise, impulse and synthetic music.
Core scenarios cover smoother/pan/limiter, LUFS with a full short-term warm-up,
resampler/PI drift, TrackPublisher/HubEngine mixes, StreamConsumer and FriendReader
jitter/stall/recovery. FriendReader uses an offline packet-arrival schedule and
records explicit audio-window offsets plus full block telemetry. It does not
run the real network proxy or exercise packet loss/reordering.

The audio encoding is headerless little-endian IEEE 754 float32. Channel order,
frame counts, rates, parameters, input seeds and telemetry units are in each
manifest. Vocal outputs interleave headphone left, headphone right and network
mono. Some core f32 files contain meter/control telemetry rather than audio.

Build and check core references from the repository root:

```powershell
cmake -S . -B build/compat-baseline -A x64 -DHEARASIDE_BUILD_PLUGINS=OFF -DHEARASIDE_BUILD_KARAOKE=OFF -DHEARASIDE_BUILD_OBS=OFF -DHEARASIDE_BUILD_TOOLS=OFF
cmake --build build/compat-baseline --config Release --target hs_compat_golden
build/compat-baseline/tools/compat-golden/Release/hs_compat_golden.exe --check compat/golden/core
ctest --test-dir build/compat-baseline -C Release -R compat_golden --output-on-failure
```

Core uses the existing SpeexDSP/library dependencies. See
`tools/compat-golden/README.md` for portable build details and the complete cases.
VocalChain builds separately with JUCE and also joins the main test build when
JUCE DSP is available:

```powershell
cmake -S tools/compat-vocal-golden -B build/compat-vocal-golden -A x64
cmake --build build/compat-vocal-golden --config Release
build/compat-vocal-golden/Release/hs-vocal-golden.exe --check compat/golden/vocal
ctest --test-dir build/compat-vocal-golden -C Release --output-on-failure
```

Both generators support `--output <empty-directory>` without overwriting an
existing baseline. `--check` regenerates and compares exact bytes on the reference
Windows x64/MSVC 19.44.35229.0 Release toolchain. `--compare` allows maximum
absolute f32 difference `1e-6` for DSP/math outputs and telemetry; core seeded
noise/impulse remain exact. For audio this bound is -120 dBFS. It does not add any
latency or perceptual tolerance. CTest uses the numeric comparison, plus core
determinism and a reference-toolchain-only exact check.

For CI without native dependencies:

```powershell
python compat/check_golden.py
```

This verifies the recorded corpus inventory and hashes. The compatibility
workflow runs it on Windows/Linux; it does not compile or rerun DSP there.
Live generator execution has been verified only on Windows. These snapshots do
not prove real-time allocation safety, DAW behavior, hardware latency or sound
quality through a real audio interface.
