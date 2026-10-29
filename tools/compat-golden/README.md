# C++ core audio baseline

Build through the repository's test build so the generator links the same
`ssengine`, `ssdsp`, `ssbus`, and Speex resampler targets as the existing tests:

```powershell
cmake -S . -B build/compat-baseline -DHEARASIDE_BUILD_PLUGINS=OFF -DHEARASIDE_BUILD_KARAOKE=OFF -DHEARASIDE_BUILD_OBS=OFF -DHEARASIDE_BUILD_TOOLS=OFF
cmake --build build/compat-baseline --config Release --target hs_compat_golden
./build/compat-baseline/tools/compat-golden/Release/hs_compat_golden.exe --self-test
./build/compat-baseline/tools/compat-golden/Release/hs_compat_golden.exe --check compat/golden/core
./build/compat-baseline/tools/compat-golden/Release/hs_compat_golden.exe --compare compat/golden/core
./build/compat-baseline/tools/compat-golden/Release/hs_compat_golden.exe --output build/new-core-golden
```

Linux executable path: `build/compat-baseline/tools/compat-golden/hs_compat_golden`.

A leading `--netsim` selects the net-sim replay corpus for `--output`, `--check`,
`--compare` and `--self-test` (`compat/golden/netsim`, reference Linux x86_64 GCC
13.3.0). Its traces are embedded at configure time and written back verbatim as
`traces/*.trace`. `--rt-audit` regenerates both corpora and fails when an
audio-thread call allocates or takes a blocking lock beyond the recorded C++
baseline; see `compat/golden/README.md`.
`--output` requires a new or empty directory; it never refreshes an existing
baseline in place. Review generated changes before replacing reference fixtures.

`--self-test` generates twice, compares every byte, and checks that byte changes,
truncation, and out-of-tolerance/non-finite numeric values are rejected.
`--check` compares manifest and sample bytes exactly. `--compare` allows absolute
float error up to `1e-6` (−120 dBFS) for math-dependent samples/telemetry. LCG noise
and impulse inputs remain exact; file inventory and scenario metadata remain exact.
Both comparison modes reject missing and extra files. `provenance.json` records
the reference build's compiler, platform, source commit, and source/dependency
hashes; comparison requires its presence but does not compare it with the new build.
This lets later commits compare output with the original reference build.

The recorded reference uses Windows x64, MSVC 19.44.35229.0, Release.
CTest uses numeric comparison on every platform and additionally exact comparison
for that MSVC release compiler. Cross-platform bit identity is not promised;
the portable determinism test checks repeatability within each build.

The 20 f32 captures cover:

- Sine, LCG noise, impulse, and original synthetic music inputs.
- Smoother ramps, pan sweep, limiter attack/release/disabled path, 4 seconds of
  momentary/short-term LUFS (including a complete 3-second window), variable-rate
  resampling, and PI drift state.
- Two TrackPublishers into Hub headphone/stream/stem output, monitor gating,
  panic, preview, stopped transport, seek, suspension, and prepare epoch changes.
- StreamConsumer at 44.1 kHz to 48 kHz, fixed buffering and a simulated +150 ppm
  producer clock. Its private bus has pinned-live heartbeat; wall-clock scheduling
  cannot manufacture disconnections.
- FriendReader at 44.1/48 kHz, +200 ppm clock, seeded arrival jitter, 120/500 ms
  backlog stalls, and recovery. Packet timing follows simulated output frames.
  Three 0.5-second audio windows are stored with explicit source frame offsets;
  six state values are stored per block throughout the full 26-second simulation.

Every normal capture must contain finite, nonzero data; the LUFS meter must warm
up, limiter must obey its ceiling, and friends must re-prime and resume audio.
Unique buses keep simulations separate from live sessions. POSIX test segments
are unlinked after the generator's engine/consumer objects release them.

These are current C++ core references, not audio rendered by the shipped plug-in
binary. The test supplies a synthetic DAW master mix; it does not exercise the
JUCE TrackProcessor's own parameter smoothing. VocalChain has a separate
generator. Net-sim packet loss/reordering (TCP has neither), eight-friend Hub routing/Line up/mix-minus,
all host block sizes/sample rates, and real DAWs are outside this corpus.
