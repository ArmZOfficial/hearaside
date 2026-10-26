#include "AudioSettings.h"
#include "Utf8.h"
#include "Components.h"

namespace hearaside::karaoke {

LatencyInfo AudioSettings::getLatencyInfo(juce::AudioDeviceManager& dm) {
    LatencyInfo info;
    if (auto* dev = dm.getCurrentAudioDevice()) {
        info.sampleRate = juce::roundToInt(dev->getCurrentSampleRate());
        info.bufferSize = dev->getCurrentBufferSizeSamples();
        info.driverType = dev->getTypeName();
        info.deviceName = dev->getName();
        if (info.sampleRate > 0) {
            info.inputMs = (dev->getInputLatencyInSamples() * 1000.0) / info.sampleRate;
            info.outputMs = (dev->getOutputLatencyInSamples() * 1000.0) / info.sampleRate;
            info.totalMs = info.inputMs + info.outputMs;
        }
    }
    return info;
}

void AudioSettings::saveState(juce::AudioDeviceManager& dm, SharedSettings& settings) {
    if (auto xml = dm.createStateXml()) {
        settings->setAudioDeviceState("karaoke", xml->toString());
    }
}

// Ask for 2 inputs (a mono mic still gets its 1): opened with only channel 1, a stereo USB mic (Fantech Leviosa)
// can deliver loud noise until the channels are re-picked. Saved states without explicit channels use this count.
void AudioSettings::loadState(juce::AudioDeviceManager& dm, SharedSettings& settings) {
    const juce::String xmlStr = settings->audioDeviceState("karaoke");
    if (xmlStr.isNotEmpty()) {
        if (auto xml = juce::XmlDocument::parse(xmlStr)) {
            dm.initialise(2, 2, xml.get(), true);
            return;
        }
    }
    dm.initialiseWithDefaultDevices(2, 2);
}

class AudioSettingsDialogContent : public juce::Component {
public:
    AudioSettingsDialogContent(juce::AudioDeviceManager& dm, const theme::Palette& pal)
        : dm_(dm), pal_(pal),
          selector_(dm, 1, 2, 2, 2, false, false, true, false) {   // no MIDI rows: the app has no MIDI
        addAndMakeVisible(selector_);
        setSize(520, 380);
    }

    void paint(juce::Graphics& g) override {
        g.fillAll(pal_.paper);
        const auto info = AudioSettings::getLatencyInfo(dm_);

        // Latency banner at bottom
        const auto bannerArea = juce::Rectangle<float>(16.0f, float(getHeight() - 54), float(getWidth() - 32), 40.0f);
        drawInset(g, bannerArea, 10.0f, pal_);

        g.setFont(uiFont(12.0f, Weight::Medium));
        g.setColour(pal_.ink);

        juce::String latText = "ความหน่วงรวม: "_th + juce::String(info.totalMs, 1) + " ms (" +
                               juce::String(info.inputMs, 1) + " ms in + " +
                               juce::String(info.outputMs, 1) + " ms out)";
        if (info.totalMs <= 10.0) {
            drawStatusDot(g, juce::Point<float>(bannerArea.getX() + 14.0f, bannerArea.getCentreY()), Dot::Ok, pal_);
            g.drawText(latText + " (Ultra-low latency)",
                       juce::Rectangle<float>(bannerArea.getX() + 26.0f, bannerArea.getY(), bannerArea.getWidth() - 30.0f, bannerArea.getHeight()),
                       juce::Justification::centredLeft);
        } else if (info.totalMs <= 30.0) {
            drawStatusDot(g, juce::Point<float>(bannerArea.getX() + 14.0f, bannerArea.getCentreY()), Dot::None, pal_);
            g.drawText(latText + " (Good for singing)",
                       juce::Rectangle<float>(bannerArea.getX() + 26.0f, bannerArea.getY(), bannerArea.getWidth() - 30.0f, bannerArea.getHeight()),
                       juce::Justification::centredLeft);
        } else {
            drawStatusDot(g, juce::Point<float>(bannerArea.getX() + 14.0f, bannerArea.getCentreY()), Dot::Warn, pal_);
            g.drawText(latText + " (แนะนำ Exclusive หรือ buffer เล็ก)"_th,
                       juce::Rectangle<float>(bannerArea.getX() + 26.0f, bannerArea.getY(), bannerArea.getWidth() - 30.0f, bannerArea.getHeight()),
                       juce::Justification::centredLeft);
        }
    }

    void resized() override {
        selector_.setBounds(12, 12, getWidth() - 24, getHeight() - 74);
    }

private:
    juce::AudioDeviceManager& dm_;
    const theme::Palette& pal_;
    juce::AudioDeviceSelectorComponent selector_;
};

void AudioSettings::showSettingsDialog(juce::AudioDeviceManager& dm, const theme::Palette& pal,
                                       std::function<void()> onClosed) {
    auto content = std::make_unique<AudioSettingsDialogContent>(dm, pal);
    juce::DialogWindow::LaunchOptions opt;
    opt.content.setOwned(content.release());
    opt.dialogTitle = "ตั้งค่าอุปกรณ์เสียง (Audio Settings)"_th;
    opt.dialogBackgroundColour = pal.paper;
    opt.escapeKeyTriggersCloseButton = true;
    opt.useNativeTitleBar = false;
    opt.resizable = false;
    opt.launchAsync();
    if (onClosed) onClosed();
}

} // namespace hearaside::karaoke
