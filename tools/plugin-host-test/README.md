# Plug-in host test

The existing positional mode exercises Track, Hub and optional App Audio binaries
through JUCE's VST3/VST2 hosting APIs. The state capture mode loads one supplied
binary, captures six cases, and restores each saved file into a fresh instance:

```powershell
plugin-host-test --dump-state "C:/path/HEARASIDE Track.vst3" "C:/path/empty-fixture-directory"
```

The output directory must be empty. Each case produces the unmodified JUCE-host
state container (`.bin`) and its decoded plug-in XML (`.xml`). VST3 uses JUCE's
host wrapper with a base64 `IComponent`; VST2 uses an FXB bank containing the
plug-in chunk. `manifest.json` records the supplied path, reported version and
format, case labels, host parameter values, file sizes and round-trip result.
It also records the actual binary's JUCE `PluginDescription` unique ID, deprecated
UID, manufacturer and category. Each parameter includes its index, label, default,
step count, discrete/automatable/meta flags and minimum/maximum/default texts.
VST2 automation addresses parameters by zero-based index. These host APIs expose
normalized values; they do not provide a raw plain-value float range. `getText`
may use JUCE's normalized fallback when VST2 does not supply extended value
descriptions; `currentText` also records the plug-in's current display text.
It does not establish binary release provenance; record the binary hashes and
their verified release source alongside the captured files.

Cases are default, all parameters changed from default, all parameters at minimum,
all parameters at maximum, and two plug-in-specific metadata cases:

- Track: Thai name, stem and chain latency; then friend-input role and friend ID.
- Hub: Thai stem names and Line up; then a friend and remembered feeder/Track pair.
- App Audio: follow recording and link input; then a friend source.

Channel color comes from the host and is not serialized by these processors.
The capture tool records that limitation rather than inventing a state property.
Parameters use valid range endpoints so boolean and choice states round-trip
without the normalization loss of intermediate values.

Every captured state is read back from disk and restored after destroying the
original instance, so a live Track with the same UUID cannot trigger duplicate
identity recovery. The check compares all decoded XML attributes/children and
all host parameter values. Metadata cases also check the requested field values.
A failed check returns nonzero and sets `roundTripPassed` to false.

Capture uses unique bus, settings and signed-out account names; disables tunnels;
and points account API requests at loopback port 9. It does not prepare or process
audio, start recording, or request a real program's capture. Friend/link source
cases use shared-memory inputs. Fixture friend tokens are synthetic.

Build just the host without building production plug-ins or network libraries:

```powershell
cmake -S tools/plugin-host-test -B build/compat-host -A x64 -DHEARASIDE_VST2_SDK="C:/path/to/licensed/vstsdk2.4"
cmake --build build/compat-host --config Release --target hearaside_plugin_host_test
```

The VST2 SDK argument is optional; omit it to build VST3 hosting only.
