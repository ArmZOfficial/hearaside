// plugin-host-test: loads the built HEARASIDE Track / Hub binaries the way a DAW does (VST3 or
// VST2 via JUCE hosting) and checks the behaviour end to end through shared memory.
//   plugin-host-test <Track plugin> <Hub plugin>
// Exit code 0 = all checks passed.
#include <juce_audio_processors/juce_audio_processors.h>

#include "ssbus/bus.h"

#include <cmath>
#include <cstdio>
#include <cstdlib>

namespace {

int failures = 0;
void check(bool ok, const char* what) {
    std::printf("[%s] %s\n", ok ? " ok " : "FAIL", what);
    if (!ok) ++failures;
}

void pump(int ms) {
    const auto end = juce::Time::getMillisecondCounter() + juce::uint32(ms);
    while (juce::Time::getMillisecondCounter() < end) juce::MessageManager::getInstance()->runDispatchLoopUntil(10);
}

juce::String bundlePath(const juce::String& file) {
    // VST3 on Windows: <name>.vst3/Contents/x86_64-win/<name>.vst3 -> load the bundle folder
    juce::File f(file);
    if (f.getFileExtension() == ".vst3" && f.getParentDirectory().getFileName() == "x86_64-win")
        return f.getParentDirectory().getParentDirectory().getParentDirectory().getFullPathName();
    return f.getFullPathName();
}

std::unique_ptr<juce::AudioPluginInstance> load(juce::AudioPluginFormatManager& fm, const juce::String& path) {
    for (auto* format : fm.getFormats()) {
        if (!format->fileMightContainThisPluginType(path)) continue;
        juce::OwnedArray<juce::PluginDescription> types;
        format->findAllTypesForFile(types, path);
        if (types.isEmpty()) continue;
        juce::String err;
        if (auto inst = fm.createPluginInstance(*types[0], 48000.0, 256, err)) {
            std::printf("loaded %s (%s)\n", inst->getName().toRawUTF8(), format->getName().toRawUTF8());
            return inst;
        }
        std::printf("could not instantiate %s: %s\n", path.toRawUTF8(), err.toRawUTF8());
    }
    return nullptr;
}

// Track parameter order (TrackProcessor::createLayout)
enum TrackParam { kMon = 0, kStr, kStrGain, kStrPan, kStrDelay, kMonTrim, kStrSolo };

juce::AudioProcessorParameter* P(juce::AudioPluginInstance& p, int index) { return p.getParameters()[index]; }
void setParam(juce::AudioPluginInstance& p, int index, float norm) { P(p, index)->setValueNotifyingHost(norm); }

struct Rig {
    std::unique_ptr<juce::AudioPluginInstance> a, b, hub;
    juce::AudioBuffer<float> ba { 2, 256 }, bb { 2, 256 }, bh { 2, 256 };
    juce::MidiBuffer midi;
    int64_t t = 0;
    float outPeakA = 0;

    void block(float ampA, float ampB, int delayB = 0) {
        for (int c = 0; c < 2; ++c)
            for (int i = 0; i < 256; ++i) {
                const double ph = double(t + i) * 2.0 * juce::MathConstants<double>::pi * 480.0 / 48000.0;
                const double phB = double(t + i - delayB) * 2.0 * juce::MathConstants<double>::pi * 480.0 / 48000.0;
                ba.setSample(c, i, ampA * float(std::sin(ph)));
                bb.setSample(c, i, ampB * float(std::sin(phB)));
            }
        a->processBlock(ba, midi);
        b->processBlock(bb, midi);
        outPeakA = ba.getMagnitude(0, 0, 256);
        bh.clear();
        bh.addFrom(0, 0, ba, 0, 0, 256);
        bh.addFrom(1, 0, ba, 1, 0, 256);
        bh.addFrom(0, 0, bb, 0, 0, 256);
        bh.addFrom(1, 0, bb, 1, 0, 256);
        hub->processBlock(bh, midi);
        t += 256;
    }
};

// Peak of the Stream Mix over the last `frames` frames.
float streamPeak(ssbus::BusLayout& L, uint32_t frames) {
    std::vector<float> l(frames), r(frames);
    float* d[2] = { l.data(), r.data() };
    const uint64_t w = L.streamHeader.writePos[0].load();
    ssbus::ringReadAt(L.streamAudio[0], L.streamHeader.writePos[0], w - frames, d, frames);
    float pk = 0;
    for (uint32_t i = 0; i < frames; ++i) pk = std::max({ pk, std::abs(l[i]), std::abs(r[i]) });
    return pk;
}

} // namespace

int main(int argc, char** argv) {
    if (argc < 3) { std::printf("usage: plugin-host-test <Track plugin> <Hub plugin>\n"); return 2; }
    juce::ScopedJuceInitialiser_GUI gui;
    juce::AudioPluginFormatManager fm;
    juce::addDefaultFormatsToManager(fm);

    // isolate from any real session: the plug-ins use this bus when nothing else is configured
    const juce::String bus = "HostTest" + juce::String(juce::Random::getSystemRandom().nextInt(100000));
#if JUCE_WINDOWS
    _putenv_s("HEARASIDE_DEFAULT_BUS", bus.toRawUTF8());
#else
    setenv("HEARASIDE_DEFAULT_BUS", bus.toRawUTF8(), 1);
#endif
    Rig rig;
    rig.a = load(fm, bundlePath(argv[1]));
    rig.b = load(fm, bundlePath(argv[1]));
    rig.hub = load(fm, bundlePath(argv[2]));
    check(rig.a && rig.b && rig.hub, "plug-ins load in a host");
    if (!(rig.a && rig.b && rig.hub)) return 1;

    for (auto* p : { rig.a.get(), rig.b.get(), rig.hub.get() }) {
        p->enableAllBuses();
        p->setPlayConfigDetails(2, 2, 48000.0, 256);
        p->prepareToPlay(48000.0, 256);
    }
    pump(300);

    ssbus::SharedMemory::Status st {};
    auto shm = ssbus::SharedMemory::openExisting(bus.toStdString(), st);
    check(shm != nullptr, "bus segment exists");
    if (!shm) return 1;
    auto& L = shm->layout();
    int active = 0;
    for (auto& s : L.slots) active += s.state.load() == ssbus::kSlotActive;
    check(active == 2, "two tracks registered on the bus");

    // ---- null test: B = -A, both STR on -> stream must be silent ---------------------------------
    for (int i = 0; i < 200; ++i) rig.block(0.5f, -0.5f);
    const float nullPk = streamPeak(L, 4096);
    std::printf("  null test residual %.1f dBFS\n", 20.0 * std::log10(std::max(1e-12f, nullPk)));
    check(nullPk < std::pow(10.0f, -90.0f / 20.0f), "null test: inverted copies cancel in the Stream Mix (< -90 dBFS)");

    // ---- one track alone reaches the stream at unity --------------------------------------------
    for (int i = 0; i < 100; ++i) rig.block(0.5f, 0.0f);
    const float alone = streamPeak(L, 4096);
    check(std::abs(alone - 0.5f) < 0.01f, "single track reaches the stream at unity gain");

    // ---- headphone level (monTrim) changes only the DAW output ------------------------------------
    setParam(*rig.a, kMonTrim, (-20.0f + 60.0f) / 66.0f);   // -60..+6 dB range
    pump(50);
    for (int i = 0; i < 100; ++i) rig.block(0.5f, 0.0f);
    std::printf("  DAW output %.3f, stream %.3f\n", rig.outPeakA, streamPeak(L, 4096));
    check(std::abs(rig.outPeakA - 0.05f) < 0.003f, "Headphone Level -20 dB lowers the track's DAW output");
    check(std::abs(streamPeak(L, 4096) - 0.5f) < 0.01f, "...while viewers still get full level");

    // ---- You Hear off: DAW output silent, stream unchanged -----------------------------------------
    setParam(*rig.a, kMon, 0.0f);
    pump(50);
    for (int i = 0; i < 100; ++i) rig.block(0.5f, 0.0f);
    check(rig.outPeakA < 1e-6f, "You Hear off silences the DAW output");
    check(std::abs(streamPeak(L, 4096) - 0.5f) < 0.01f, "...and viewers still hear the track");

    // ---- viewers delay: B is the same sine 40 ms late, A delayed 40 ms + inverted -> null ---------
    setParam(*rig.a, kStrDelay, 40.0f / 500.0f);
    pump(50);
    for (int i = 0; i < 300; ++i) rig.block(0.5f, -0.5f, 1920);
    const float delayPk = streamPeak(L, 4096);
    std::printf("  delay residual %.1f dBFS\n", 20.0 * std::log10(std::max(1e-12f, delayPk)));
    check(delayPk < 0.001f, "Viewers Delay 40 ms lines a track up with a 1920-sample later copy");

    // ---- remote control from the Hub (mailbox -> host parameter) ----------------------------------
    int slotA = -1;
    for (int i = 0; i < ssbus::kMaxSlots; ++i)
        if (L.slots[i].state.load() == ssbus::kSlotActive && !(L.slots[i].flags.load() & ssbus::kFlagMon)) slotA = i;
    check(slotA >= 0, "track A mirrors You Hear = off to the bus");
    if (slotA >= 0) {
        ssbus::postCommand(L.slots[slotA], ssbus::ParamId::Mon, 1.0f);
        pump(300);   // the Track applies commands on its message-thread timer
        rig.block(0.5f, 0.0f);
        check((L.slots[slotA].flags.load() & ssbus::kFlagMon) != 0, "Hub command turns You Hear back on");
        check(P(*rig.a, kMon)->getValue() > 0.5f, "...and the host sees the parameter change");
    }

    // ---- remote control from OBS (dock / hotkeys): remote queue -> Hub -> Track -------------------
    {
        ssbus::HubStateView hs;
        check(ssbus::readHubState(L, hs) && (hs.sceneMask & 7u) == 7u && !hs.sceneNames[0].empty(),
              "Hub publishes its state (scenes, levels) for OBS");
        ssbus::postRemote(L, ssbus::RemoteParam::Panic, 1.0f);
        pump(300);
        for (int i = 0; i < 40; ++i) rig.block(0.5f, 0.0f);
        check((L.header.hubFlags.load() & ssbus::kHubPanic) != 0, "OBS command: mute stream reaches the Hub");
        check(streamPeak(L, 2048) < 1e-6f, "...and the stream goes silent");
        ssbus::postRemote(L, ssbus::RemoteParam::Panic, 0.0f);
        ssbus::postRemote(L, ssbus::RemoteParam::MasterDb, -6.0f);
        if (slotA >= 0) ssbus::postRemote(L, slotA, uint32_t(ssbus::ParamId::StrGainDb), -6.0f);
        pump(400);
        for (int i = 0; i < 60; ++i) rig.block(0.5f, 0.0f);
        ssbus::readHubState(L, hs);
        check(std::abs(hs.masterDb + 6.0f) < 0.05f, "OBS command: stream level -6 dB applied by the Hub");
        check(slotA >= 0 && std::abs(ssbus::bitsFloat(L.slots[slotA].strGainBits.load()) + 6.0f) < 0.05f,
              "OBS command: track viewers level forwarded to the Track");
        const float pk = streamPeak(L, 4096);
        std::printf("  stream after -6 / -6 dB: %.3f (expected %.3f)\n", pk, 0.5f * std::pow(10.0f, -12.0f / 20.0f));
        check(std::abs(pk - 0.5f * std::pow(10.0f, -12.0f / 20.0f)) < 0.01f, "...both gains reach the Stream Mix");
        ssbus::postRemote(L, ssbus::RemoteParam::MasterDb, 0.0f);
        if (slotA >= 0) ssbus::postRemote(L, slotA, uint32_t(ssbus::ParamId::StrGainDb), 0.0f);
        pump(300);
        rig.block(0.5f, 0.0f);
    }

    // ---- state round trip --------------------------------------------------------------------------
    juce::MemoryBlock saved;
    rig.a->getStateInformation(saved);
    setParam(*rig.a, kStrDelay, 0.0f);
    rig.a->setStateInformation(saved.getData(), int(saved.getSize()));
    pump(50);
    check(std::abs(P(*rig.a, kStrDelay)->getValue() - 40.0f / 500.0f) < 0.002f, "state save / restore keeps the delay");

    for (auto* p : { rig.a.get(), rig.b.get(), rig.hub.get() }) p->releaseResources();
    rig.a.reset(); rig.b.reset(); rig.hub.reset();
    std::printf("%s (%d failure%s)\n", failures == 0 ? "PLUGIN HOST TEST PASSED" : "PLUGIN HOST TEST FAILED", failures, failures == 1 ? "" : "s");
    return failures == 0 ? 0 : 1;
}
