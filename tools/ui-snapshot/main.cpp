// ui-snapshot: renders the Track and Hub editors to PNG files (light + dark, Thai + English)
// so the native UI can be checked against the design mock-up without a DAW.
//   ui-snapshot <output folder>
#include "track/TrackProcessor.h"
#include "hub/HubProcessor.h"
#include "Settings.h"

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

int main(int argc, char** argv) {
    juce::ScopedJuceInitialiser_GUI gui;
    const juce::File dir = argc > 1 ? juce::File::getCurrentWorkingDirectory().getChildFile(argv[1])
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
    const T demo[] = { { "ดนตรี (Backing)", true, true, -3.0f, 0.0f }, { "เสียงร้อง", false, true, 0.0f, 0.0f },
                       { "กีตาร์", true, true, -2.0f, -6.0f }, { "เมโทรนอม / ไกด์", true, false, -6.0f, -10.0f },
                       { "ไมค์พูด", false, false, 0.0f, 0.0f } };
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
