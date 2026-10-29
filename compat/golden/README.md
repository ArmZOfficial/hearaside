# C++ source audio baselines

These references come from the unchanged C++ sources at `7f4eccc`, not from the
released plug-in binaries. `core/` contains 20 f32 captures and manifest/source
provenance; `vocal/` contains four mono inputs and 20 VocalChain outputs plus its
manifest. The top-level `provenance.json` records all 47 corpus files, their
sizes/SHA-256, the VocalChain/compiler/JUCE identity and selected DSP source hashes.

`netsim/` holds three packet-arrival traces recorded through the real `tools/net-sim`
TCP proxy (profiles good 10±2 ms, home 25±10 ms with 300 ms stalls every ~8 s,
mobile 40±25 ms at 800 kbps) and the FriendReader captures replayed from them. Its
`provenance.json` names the Linux GCC 13.3.0 reference build. By the end of each
20 s trace: good had no underrun; home had 3 underruns and 3 re-primes around its
stalls (792 ms concealed); mobile had 2 underruns while priming (2.9 ms concealed).

Inputs are original deterministic sine, seeded noise, impulse and synthetic music.
Core scenarios cover smoother/pan/limiter, LUFS with a full short-term warm-up,
resampler/PI drift, TrackPublisher/HubEngine mixes, StreamConsumer and FriendReader
jitter/stall/recovery. Core FriendReader uses an offline packet-arrival schedule;
the net-sim corpus replays recorded proxy arrivals. Both record explicit
audio-window offsets plus full block telemetry. The proxy is random, so a trace
is a fixture: `node tools/net-sim/record-trace.mjs <directory>` records only the
missing profiles, never overwrites one, and rejects a recording when the sender
falls more than 5 ms behind (a stalled machine rather than the line). TCP never
loses or reorders packets, so loss/reordering remain untested.

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

Net-sim replay and the Linux reference check:

```bash
build/compat-baseline/tools/compat-golden/hs_compat_golden --netsim --compare compat/golden/netsim
build/compat-baseline/tools/compat-golden/hs_compat_golden --netsim --check compat/golden/netsim
```

## Linux results

Linux x86_64, GCC 13.3.0 Release, speexdsp `7a15878` (pinned in
`scripts/fetch-deps.ps1`) and JUCE 8.0.15 reproduce the Windows corpus within the
bound. Core: 19 of 20 captures are bit-identical; `dsp/pan.f32` differs in 46 of
4,098 samples by at most 5.96e-8 (−144.5 dBFS, libm sin/cos). VocalChain: 13 of 24
f32 files are bit-identical and the worst is `sine-wet-send.f32` at 1.19e-7
(−138.5 dBFS). Clang 18.1.3 gives the same core result and a bit-identical
net-sim corpus. `--compare` passes; `--check` is exact only on the reference
toolchain of each corpus.

Source hashes in `core/provenance.json` were taken from a CRLF Windows checkout;
the same files hash differently with LF line endings. `.gitattributes` keeps the
corpus and fixtures byte-exact on every checkout.

## Real-time audit

`hs_compat_golden --rt-audit` and `hs-vocal-golden --rt-audit <vocal-directory>`
regenerate their corpora while counting heap allocations and blocking mutex locks
inside the calls a host, OBS or the network thread makes on the audio thread
(`tools/compat-golden/rt_audit.h`). With glibc, malloc/calloc/realloc/aligned
allocation and `pthread_mutex_lock` are interposed; on Windows only `operator new`
is counted and locks are not detected. A self-test proves the counters fire first.

Smoother, pan, limiter, LUFS, resampler, PI drift, TrackPublisher, HubEngine and
VocalChain made zero allocations and blocking locks. The unchanged C++ has these
baseline exceptions, recorded as limits in the generator (the Rust port must reach
zero, PLAN R2.4):

| Call | Allocations | Cause |
|---|---|---|
| `StreamConsumer::pull` | 10, first pull | `tryConnect` opens the shared memory and `resync` constructs the Speex `VarResampler` on the OBS audio thread |
| `FriendReader::pull` | 1 per reader (4 of 5 scenarios), in the first ~90 ms | `setCorrectionPpm` calls `speex_resampler_set_rate_frac`; `update_filter` reallocates when a correction lengthens the filter |

The audit covers only the scenarios in this corpus, not every host block size,
reconnect path or parameter change.

## Offline CI

For CI without native dependencies:

```powershell
python compat/check_golden.py
```

This verifies the recorded corpus inventory and hashes. The compatibility
workflow runs it on Windows/Linux. `.github/workflows/compat-golden.yml` builds and
runs both generators, the net-sim replay and the real-time audits on Linux.
These snapshots do not prove DAW behavior, hardware latency or sound quality
through a real audio interface.
