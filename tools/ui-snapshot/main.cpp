// ui-snapshot: renders the Track and Hub editors to PNG files (light + dark, Thai + English)
// so the native UI can be checked against the design mock-up without a DAW.
//   ui-snapshot <output folder>
#include "track/TrackProcessor.h"
#include "hub/HubProcessor.h"
#include "Settings.h"
#include "hub/MasteringPanel.h"
#include "ui/LookAndFeel.h"

#include <juce_audio_processors/juce_audio_processors.h>

#include <functional>

using namespace hearaside;

namespace {

void save(juce::Component& c, const juce::File& file) {
    c.setVisible(true);
    const auto img = c.createComponentSnapshot(c.getLocalBounds(), true, 2.0f);
    file.deleteFile();
    juce::FileOutputStream out(file);
    juce::PNGImageFormat().writeImageToStream(img, out);
    std::printf("wrote %s (%dx%d)\n", file.getFullPathName().toRawUTF8(), img.getWidth(), img.getHeight());
}

std::function<void(int)> gAudio;   // runs n audio blocks (keeps heartbeats / meters alive)

void pump(int ms) {
    const auto end = juce::Time::getMillisecondCounter() + juce::uint32(ms);
    while (juce::Time::getMillisecondCounter() < end) {
        if (gAudio) gAudio(4);
        juce::MessageManager::getInstance()->runDispatchLoopUntil(20);
    }
}

} // namespace

// --test-mastering [plug-in name]: hosts a real VST3 in the Hub's mastering chain, runs audio
// through it, saves and restores the chain. Exit code 0 = ok.
static int testMastering(const juce::String& wanted) {
    MasteringChain chain;
    chain.prepare(48000.0, 512);
    const auto files = MasteringChain::findPluginFiles();
    std::printf("found %d plug-in files (VST3 + VST2)\n", files.size());
    int failures = 0;
    auto check = [&](bool ok, const char* what) { std::printf("[%s] %s\n", ok ? " ok " : "FAIL", what); failures += ok ? 0 : 1; };
    check(files.size() > 0, "plug-in folder scan (files only, nothing loaded)");

    juce::File pick;
    for (const auto& f : files)
        if (wanted.isEmpty() ? f.getFileNameWithoutExtension().containsIgnoreCase("limiter") : f.getFileNameWithoutExtension().containsIgnoreCase(wanted)) { pick = f; break; }
    if (pick == juce::File()) { std::printf("no matching plug-in to test with\n"); return failures; }
    std::printf("testing with %s\n", pick.getFileName().toRawUTF8());

    auto types = chain.typesIn(pick);
    check(types.size() > 0, "plug-in types read from the file");
    if (types.isEmpty()) return failures + 1;
    const auto err = chain.add(*types[0]);
    std::printf("  add -> '%s'\n", err.toRawUTF8());
    check(err.isEmpty() && chain.size() == 1, "plug-in loaded into the chain");

    std::vector<float> l(512), r(512);
    float peakIn = 0, peakOut = 0;
    bool finite = true;
    for (int b = 0; b < 200; ++b) {
        for (int i = 0; i < 512; ++i) l[size_t(i)] = r[size_t(i)] = 0.5f * std::sin(float(b * 512 + i) * 0.0628f);
        for (int i = 0; i < 512; ++i) peakIn = std::max(peakIn, std::abs(l[size_t(i)]));
        chain.processStream(l.data(), r.data(), 512);
        for (int i = 0; i < 512; ++i) { peakOut = std::max(peakOut, std::abs(l[size_t(i)])); finite &= std::isfinite(l[size_t(i)]); }
    }
    std::printf("  in %.3f -> out %.3f, latency %d frames\n", peakIn, peakOut, chain.totalLatencyFrames());
    check(finite && peakOut > 0.0f, "audio runs through the hosted plug-in");

    chain.setBypassed(0, true);
    for (int i = 0; i < 512; ++i) l[size_t(i)] = r[size_t(i)] = 0.25f;
    chain.processStream(l.data(), r.data(), 512);
    check(l[100] == 0.25f, "bypass passes audio untouched");

    const auto xml = chain.toXml();
    MasteringChain restored;
    restored.prepare(48000.0, 512);
    restored.fromXml(*xml);
    check(restored.size() == 1 && restored.isBypassed(0) && restored.name(0) == chain.name(0), "chain saved and restored with the project");
    chain.remove(0);
    check(chain.size() == 0, "plug-in removed");
    std::printf("%s\n", failures == 0 ? "MASTERING TEST PASSED" : "MASTERING TEST FAILED");
    return failures;
}

int main(int argc, char** argv) {
    juce::ScopedJuceInitialiser_GUI gui;
    if (argc > 1 && juce::String(argv[1]) == "--test-mastering")
        return testMastering(argc > 2 ? juce::String(argv[2]) : juce::String()) == 0 ? 0 : 1;
    const juce::File dir = argc > 1 && juce::String(argv[1]) != "--demo" ? juce::File::getCurrentWorkingDirectory().getChildFile(argv[1])
                                    : juce::File::getCurrentWorkingDirectory().getChildFile("ui-snapshots");
    dir.createDirectory();

    SharedSettings settings;
    const auto oldLang = settings->language();
    const auto oldTheme = settings->themeMode();

    // a Hub and five Tracks on a private bus, like the mock-up
    HubProcessor hub;
    hub.setBusName("Snapshot");
    hub.prepareToPlay(48000, 256);
    struct T { const char* name; bool mon, str; float db, trim; };
    const T demo[] = { { "à¸”à¸™à¸•à¸£à¸µ (Backing)", true, true, -3.0f, 0.0f }, { "à¹€à¸ªà¸µà¸¢à¸‡à¸£à¹‰à¸­à¸‡", false, true, 0.0f, 0.0f },
                       { "à¸à¸µà¸•à¸²à¸£à¹Œ", true, true, -2.0f, -6.0f }, { "à¹€à¸¡à¹‚à¸—à¸£à¸™à¸­à¸¡ / à¹„à¸à¸”à¹Œ", true, false, -6.0f, -10.0f },
                       { "à¹„à¸¡à¸„à¹Œà¸žà¸¹à¸”", false, false, 0.0f, 0.0f } };
    std::vector<std::unique_ptr<TrackProcessor>> tracks;
    for (const auto& d : demo) {
        auto t = std::make_unique<TrackProcessor>();
        t->setBusName("Snapshot");
        t->setDisplayNameOverride(juce::String::fromUTF8(d.name));
        t->prepareToPlay(48000, 256);
        auto set = [&](const char* id, float v) { auto* p = t->params().getParameter(id); p->setValueNotifyingHost(p->convertTo0to1(v)); };
        set(trackparam::Mon, d.mon ? 1.0f : 0.0f);
        set(trackparam::Str, d.str ? 1.0f : 0.0f);
        set(trackparam::StrGain, d.db);
        set(trackparam::MonTrim, d.trim);
        if (tracks.empty()) set(trackparam::StrDelay, 40.0f);
        tracks.push_back(std::move(t));
    }
    // run a few audio blocks so meters, heartbeats and mirrors are live
    juce::AudioBuffer<float> buf(2, 256);
    juce::MidiBuffer midi;
    int block = 0;
    gAudio = [&](int count) { for (int k = 0; k < count; ++k, ++block) {
        const int b = block;
        for (size_t i = 0; i < tracks.size(); ++i) {
            for (int c = 0; c < 2; ++c)
                for (int s = 0; s < 256; ++s)
                    buf.setSample(c, s, 0.3f / float(i + 1) * std::sin(float(b * 256 + s) * 0.03f * float(i + 1)));
            tracks[i]->processBlock(buf, midi);
        }
        buf.clear();
        hub.processBlock(buf, midi);
    } };
    gAudio(200);

    // --demo <seconds>: keep the Hub and Tracks running (bus "Snapshot") so the OBS control dock
    // can be tried live: http://127.0.0.1:47621/?bus=Snapshot
    for (int i = 1; i + 1 < argc; ++i) {
        if (juce::String(argv[i]) == "--demo") {
            const int seconds = juce::String(argv[i + 1]).getIntValue();
            std::printf("demo running for %d s on bus \"Snapshot\"\n", seconds);
            std::fflush(stdout);
            pump(seconds * 1000);
            gAudio = nullptr;
            return 0;
        }
    }

    for (int lang = 0; lang < 2; ++lang) {
        for (int dark = 0; dark < 2; ++dark) {
            settings->setLanguage(lang == 0 ? Language::Thai : Language::English);
            settings->setThemeMode(dark ? ThemeMode::Dark : ThemeMode::Light);
            const juce::String suffix = juce::String(lang == 0 ? "th" : "en") + (dark ? "-dark" : "-light");
            {
                std::unique_ptr<juce::AudioProcessorEditor> ed(hub.createEditor());
                pump(400);
                save(*ed, dir.getChildFile("hub-" + suffix + ".png"));
            }
            {
                std::unique_ptr<juce::AudioProcessorEditor> ed(tracks[1]->createEditor());
                pump(400);
                save(*ed, dir.getChildFile("track-" + suffix + ".png"));
            }
        }
    }
    // mastering panel (list page with one plug-in, and the picker page)
    {
        settings->setLanguage(Language::Thai);
        settings->setThemeMode(ThemeMode::Dark);
        hearaside::LookAndFeel lnf;
        lnf.setDark(true);
        for (const auto& f : MasteringChain::findPluginFiles()) {
            if (!f.getFileNameWithoutExtension().containsIgnoreCase("limiter")) continue;
            auto types = hub.mastering().typesIn(f);
            if (!types.isEmpty()) hub.mastering().add(*types[0]);
            break;
        }
        {
            MasteringPanel panel(hub.mastering());
            panel.setLookAndFeel(&lnf);
            pump(200);
            juce::Component bg;
            bg.setSize(panel.getWidth(), panel.getHeight());
            save(panel, dir.getChildFile("mastering-list.png"));
            panel.setLookAndFeel(nullptr);
        }
        {
            MasteringPanel panel(hub.mastering());
            panel.setLookAndFeel(&lnf);
            if (auto* add = dynamic_cast<juce::Button*>(panel.getChildComponent(0))) add->triggerClick();
            pump(300);
            save(panel, dir.getChildFile("mastering-picker.png"));
            panel.setLookAndFeel(nullptr);
        }
        while (hub.mastering().size() > 0) hub.mastering().remove(0);
    }

    // preview + panic state (Thai, light)
    settings->setLanguage(Language::Thai);
    settings->setThemeMode(ThemeMode::Light);
    hub.setParam(hubparam::Preview, 1.0f);
    hub.setParam(hubparam::Panic, 1.0f);
    {
        std::unique_ptr<juce::AudioProcessorEditor> ed(hub.createEditor());
        pump(400);
        save(*ed, dir.getChildFile("hub-th-preview-panic.png"));
    }
    gAudio = nullptr;
    settings->setLanguage(oldLang);
    settings->setThemeMode(oldTheme);
    return 0;
}
