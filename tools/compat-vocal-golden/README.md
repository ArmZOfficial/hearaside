# VocalChain migration goldens

This standalone tool compiles the existing `apps/karaoke/VocalChain.cpp` without
changing it. It captures the C++ source baseline at commit
`7f4eccc298734d78667b67fa51ee81dee41d0e2b`; it does not capture released binaries.

```powershell
cmake -S tools/compat-vocal-golden -B build/compat-vocal-golden -A x64
cmake --build build/compat-vocal-golden --config Release --target hs-vocal-golden
build/compat-vocal-golden/Release/hs-vocal-golden.exe --check compat/golden/vocal
build/compat-vocal-golden/Release/hs-vocal-golden.exe --compare compat/golden/vocal
ctest --test-dir build/compat-vocal-golden -C Release --output-on-failure
```

`--output <empty-directory>` creates a new capture. `--check <fixture-directory>`
regenerates the reference in memory and compares every file byte for byte without
writing files. Missing, changed or extra files fail. Preserve the committed
baseline when porting; do not regenerate it merely to hide an audio change.

`--compare` allows maximum absolute sample error `1e-6` (-120 dBFS), including
input references whose sine functions may round differently across compilers.
It rejects nonfinite samples, missing/extra files and different sample counts.
Manifest metadata compares semantically; measured peak/tail/stereo summary values
are ignored because the actual audio is compared. Other metadata must match.
This mode never writes files. CTest uses `--compare` for cross-platform checking;
`--check` retains exact same-toolchain repeatability checking.

The fixture has four original deterministic mono inputs: a 997 Hz sine, seeded
noise with quiet/loud/quiet segments, two signed impulses, and a synthetic chord
with percussive seeded noise. No external recordings are used. Every input has
32,768 frames at 48 kHz, processed in 256-frame blocks: 8,192 active input frames
followed by 24,576 silent frames to capture reverb/echo tails. `manifest.json`
records the seed, frames, channel order, control settings and measured peaks.
Input synthesis is specified by the checked-in `main.cpp`.

Each input runs through five fresh chain instances:

- Actual production defaults, with dry mono network send.
- Ambience off, retaining the fixed gate, EQ and compressor.
- Stronger reverb/echo and wet mono network send.
- A gate threshold above the input, allowing the initial release slew to settle.
- Muted output.

`input-*.f32` files are unprocessed mono references. VocalChain exposes no true
bypass, adjustable EQ or adjustable compressor API. Ambience-off is not a full
bypass. The production EQ/compressor and float operation order remain unchanged.

All audio files contain explicit little-endian IEEE 754 floats with no header.
Output files interleave three channels: headphone left, headphone right, mono
network send. Each input is 131,072 bytes; each output is 393,216 bytes. There are
20 output files and four input files. The generator also checks finite samples,
nonzero ordinary output, audible stereo ambience tails, exact muted silence,
dry-routing equality, wet-send averaging and gate settling.

The reference capture uses MSVC 19.44.35229.0, Windows x64, Release, without
fast-math. Repeated captures on this toolchain are bit identical. A different
compiler's transcendental math or JUCE implementation may produce small float
differences; investigate those rather than replacing the baseline. Rust DSP
comparison must use the migration plan's maximum absolute error of `1e-6`
(-120 dBFS). This tool's deterministic C++ regeneration check is stricter.

This is an offline generator. It does not hook allocations, locks or syscalls;
real-time safety remains a separate migration check. Other sample rates, variable
blocks, state changes during a stream and hardware listening tests are not
covered here.
