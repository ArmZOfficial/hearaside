#include "MainWindow.h"

#include <cmath>

namespace hearaside::karaoke {

class KaraokeContent : public juce::Component,
                       public juce::Timer,
                       public juce::AudioIODeviceCallback,
                       private juce::ChangeListener {
public:
    KaraokeContent(MainWindow& owner, juce::AudioDeviceManager& dm)
        : owner_(owner), deviceManager_(dm),
          joinBtn_(icons::Icon::Broadcast, 42.0f),
          themeToggleBtn_(icons::Icon::Sliders, "เปลี่ยนธีม สว่าง / มืด"),
          settingsBtn_(icons::Icon::Sliders, "ตั้งค่าอุปกรณ์เสียง"),
          muteBtn_(),
          micMeter_(6.0f, false) {
        settings_->addChangeListener(this);
        deviceManager_.addAudioCallback(this);

        // Header controls
        themeToggleBtn_.onClick = [this] {
            settings_->setThemeMode(settings_->isDark() ? ThemeMode::Light : ThemeMode::Dark);
        };
        addAndMakeVisible(themeToggleBtn_);

        settingsBtn_.onClick = [this] {
            openAudioSettings();
        };
        addAndMakeVisible(settingsBtn_);

        // Join Section
        linkInput_.setPlaceholder("วางลิงก์ส่งเสียง เช่น https://.../s/<token> หรือ has://join?u=...");
        linkInput_.onReturnKey = [this] { onJoinClicked(); };
        addAndMakeVisible(linkInput_);

        joinBtn_.setButtonText("เข้าห้อง");
        joinBtn_.onClick = [this] { onJoinClicked(); };
        addAndMakeVisible(joinBtn_);

        // Monitor & Mic
        addAndMakeVisible(micMeter_);

        muteBtn_.onClick = [this] {
            micMuted_ = !micMuted_;
            muteBtn_.setActive(micMuted_);
            overlay_.toast(micMuted_ ? "ปิดไมค์แล้ว (Muted)" : "เปิดไมค์แล้ว (Unmuted)", 1500);
        };
        addAndMakeVisible(muteBtn_);

        vocalSlider_.setRange(-30.0, 6.0, 0.5);
        vocalSlider_.setValue(0.0, juce::dontSendNotification);
        addAndMakeVisible(vocalSlider_);

        musicSlider_.setRange(-30.0, 6.0, 0.5);
        musicSlider_.setValue(0.0, juce::dontSendNotification);
        addAndMakeVisible(musicSlider_);

        friendsSlider_.setRange(-30.0, 6.0, 0.5);
        friendsSlider_.setValue(0.0, juce::dontSendNotification);
        addAndMakeVisible(friendsSlider_);

        addAndMakeVisible(overlay_);

        startTimerHz(30);
        updateDeviceInfo();
    }

    ~KaraokeContent() override {
        stopTimer();
        deviceManager_.removeAudioCallback(this);
        settings_->removeChangeListener(this);
    }

    void handleDeepLink(const juce::String& link) {
        juce::String clean = link.trim();
        if (clean.startsWithIgnoreCase("has://join?u=")) {
            clean = juce::URL::removeEscapeChars(clean.substring(13));
        }
        linkInput_.setText(clean, juce::dontSendNotification);
        overlay_.toast("ได้รับลิงก์ห้องแล้ว: กด 'เข้าห้อง' เพื่อเริ่ม", 2500);
    }

    void openAudioSettings() {
        auto selector = std::make_unique<juce::AudioDeviceSelectorComponent>(
            deviceManager_,
            1, 2,  // min/max in
            2, 2,  // min/max out
            true,  // show input options
            true,  // show output options
            true,  // stereo pairs
            false  // hide MIDI
        );
        selector->setSize(480, 320);

        juce::DialogWindow::LaunchOptions opt;
        opt.content.setOwned(selector.release());
        opt.dialogTitle = "ตั้งค่าอุปกรณ์เสียง (Audio Settings)";
        opt.dialogBackgroundColour = pal().paper;
        opt.escapeKeyTriggersCloseButton = true;
        opt.useNativeTitleBar = false;
        opt.resizable = false;
        opt.launchAsync();
    }

    void onJoinClicked() {
        const juce::String raw = linkInput_.getText().trim();
        if (raw.isEmpty()) {
            overlay_.toast("กรุณาวางลิงก์ส่งเสียงที่ได้รับจากเพื่อน", 2000);
            return;
        }

        juce::String token = raw;
        const int slashPos = token.lastIndexOfChar('/');
        if (slashPos >= 0) token = token.substring(slashPos + 1);
        if (token.containsChar('?')) token = token.upToFirstOccurrenceOf("?", false, false);

        if (token.isEmpty()) {
            overlay_.toast("รูปแบบลิงก์ไม่ถูกต้อง", 2000);
            return;
        }

        connected_ = true;
        roomToken_ = token;
        overlay_.toast("เชื่อมต่อห้องสำเร็จ (Phase K1 MVP): " + token, 3000);
        repaint();
    }

    // AudioIODeviceCallback
    void audioDeviceIOCallbackWithContext(const float* const* inputChannelData, int numInputChannels,
                                          float* const*, int, int numSamples,
                                          const juce::AudioIODeviceCallbackContext&) override {
        float peak = 0.0f;
        if (!micMuted_ && numInputChannels > 0 && inputChannelData != nullptr && inputChannelData[0] != nullptr) {
            for (int i = 0; i < numSamples; ++i) {
                const float v = std::abs(inputChannelData[0][i]);
                if (v > peak) peak = v;
            }
        }
        micPeak_.store(peak, std::memory_order_relaxed);
    }

    void audioDeviceAboutToStart(juce::AudioIODevice*) override {}
    void audioDeviceStopped() override { micPeak_.store(0.0f); }

    // Timer
    void timerCallback() override {
        const float p = micPeak_.load(std::memory_order_relaxed);
        smoothedPeak_ = std::max(p, smoothedPeak_ * 0.82f);
        micMeter_.setLevel(hearaside::meterPosition(smoothedPeak_));
        updateDeviceInfo();
    }

    void updateDeviceInfo() {
        if (auto* dev = deviceManager_.getCurrentAudioDevice()) {
            deviceInfo_ = dev->getTypeName() + ": " + dev->getName() +
                          " (" + juce::String(juce::roundToInt(dev->getCurrentSampleRate() / 1000.0)) + " kHz, " +
                          juce::String(dev->getCurrentBufferSizeSamples()) + " smp)";
        } else {
            deviceInfo_ = "ไม่มีอุปกรณ์เสียง (No audio device)";
        }
    }

    void changeListenerCallback(juce::ChangeBroadcaster*) override {
        repaint();
    }

    const theme::Palette& pal() const {
        return settings_->isDark() ? theme::dark() : theme::light();
    }

    void paint(juce::Graphics& g) override {
        const auto& p = pal();
        g.fillAll(p.paper);

        // Header
        auto bounds = getLocalBounds().toFloat();
        const auto headerArea = bounds.removeFromTop(60.0f);

        // Wordmark & Badge
        drawWordmark(g, headerArea.reduced(24.0f, 14.0f).removeFromLeft(140.0f), "", 22.0f, p.ink);
        drawBadge(g, juce::Rectangle<float>(172.0f, 18.0f, 76.0f, 22.0f), "KARAOKE", BadgeStyle::Solid, p);

        // Header Status
        const auto dot = connected_ ? Dot::Ok : (deviceManager_.getCurrentAudioDevice() != nullptr ? Dot::None : Dot::Warn);
        const juce::String statusText = connected_ ? ("ในห้อง: " + roomToken_) : "พร้อมเข้าห้อง";
        drawStatusDot(g, juce::Point<float>(264.0f, 29.0f), dot, p, 7.0f);
        g.setColour(p.graphite);
        g.setFont(uiFont(13.0f, Weight::Medium));
        g.drawText(statusText, juce::Rectangle<float>(276.0f, 17.0f, 200.0f, 24.0f), juce::Justification::centredLeft);

        // Divider
        g.setColour(p.hairline1);
        g.drawHorizontalLine(60, 0.0f, float(getWidth()));

        // Cards layout
        const float margin = 24.0f;
        const float contentW = float(getWidth()) - (margin * 2.0f);

        // Card 1: Join Box (Inset)
        const auto joinCard = juce::Rectangle<float>(margin, 76.0f, contentW, 136.0f);
        drawInset(g, joinCard, 14.0f, p);

        g.setColour(p.ink);
        g.setFont(uiFont(15.0f, Weight::SemiBold));
        g.drawText("เข้าร่วมห้องร้องเพลง (Join Room)", joinCard.reduced(18.0f, 14.0f).removeFromTop(22.0f), juce::Justification::topLeft);

        g.setColour(p.graphite);
        g.setFont(uiFont(12.5f, Weight::Regular));
        g.drawText("วางลิงก์ส่งเสียงที่ได้รับจากเพื่อนเพื่อเริ่มร้องเพลงร่วมกัน",
                   juce::Rectangle<float>(joinCard.getX() + 18.0f, joinCard.getY() + 38.0f, contentW - 36.0f, 20.0f), juce::Justification::topLeft);

        // Card 2: Audio & Microphone Inset
        const auto audioCard = juce::Rectangle<float>(margin, 226.0f, contentW, 110.0f);
        drawInset(g, audioCard, 14.0f, p);

        g.setColour(p.ink);
        g.setFont(uiFont(15.0f, Weight::SemiBold));
        g.drawText("ไมโครโฟนและอุปกรณ์เสียง", audioCard.reduced(18.0f, 14.0f).removeFromTop(22.0f), juce::Justification::topLeft);

        g.setColour(p.graphite);
        g.setFont(uiFont(12.0f, Weight::Regular));
        g.drawText(deviceInfo_, juce::Rectangle<float>(audioCard.getX() + 18.0f, audioCard.getY() + 38.0f, contentW - 140.0f, 20.0f), juce::Justification::topLeft);

        g.setColour(p.muted);
        g.setFont(uiFont(11.0f, Weight::Regular));
        g.drawText("ไมค์:", juce::Rectangle<float>(audioCard.getX() + 18.0f, audioCard.getY() + 72.0f, 32.0f, 20.0f), juce::Justification::centredLeft);

        // Card 3: Headphone Monitor Balance Inset
        const auto monitorCard = juce::Rectangle<float>(margin, 350.0f, contentW, 144.0f);
        drawInset(g, monitorCard, 14.0f, p);

        g.setColour(p.ink);
        g.setFont(uiFont(15.0f, Weight::SemiBold));
        g.drawText("ระดับเสียงในหูฟัง (Monitor Balance)", monitorCard.reduced(18.0f, 14.0f).removeFromTop(22.0f), juce::Justification::topLeft);

        g.setColour(p.graphite);
        g.setFont(uiFont(12.5f, Weight::Regular));
        g.drawText("เสียงฉัน (Voice)", juce::Rectangle<float>(monitorCard.getX() + 18.0f, monitorCard.getY() + 42.0f, 110.0f, 24.0f), juce::Justification::centredLeft);
        g.drawText("เสียงเพลง (Music)", juce::Rectangle<float>(monitorCard.getX() + 18.0f, monitorCard.getY() + 74.0f, 110.0f, 24.0f), juce::Justification::centredLeft);
        g.drawText("เสียงเพื่อน (Friends)", juce::Rectangle<float>(monitorCard.getX() + 18.0f, monitorCard.getY() + 106.0f, 110.0f, 24.0f), juce::Justification::centredLeft);
    }

    void resized() override {
        overlay_.setBounds(getLocalBounds());

        const int w = getWidth();
        themeToggleBtn_.setBounds(w - 76, 12, 34, 34);
        settingsBtn_.setBounds(w - 38, 12, 34, 34);

        const int margin = 24;
        const int contentW = w - (margin * 2);

        // Join Row
        linkInput_.setBounds(margin + 18, 138, contentW - 146, 38);
        joinBtn_.setBounds(w - margin - 118, 138, 100, 38);

        // Mic card
        micMeter_.setBounds(margin + 54, 301, contentW - 190, 8);
        muteBtn_.setBounds(w - margin - 118, 286, 100, 36);

        // Sliders
        const int sliderX = margin + 140;
        const int sliderW = contentW - 160;
        vocalSlider_.setBounds(sliderX, 394, sliderW, 24);
        musicSlider_.setBounds(sliderX, 426, sliderW, 24);
        friendsSlider_.setBounds(sliderX, 458, sliderW, 24);
    }

private:
    MainWindow& owner_;
    juce::AudioDeviceManager& deviceManager_;
    SharedSettings settings_;
    Overlay overlay_;

    TextField linkInput_;
    PrimaryButton joinBtn_;
    IconButton themeToggleBtn_;
    IconButton settingsBtn_;
    MuteButton muteBtn_;

    MeterBar micMeter_;
    LevelSlider vocalSlider_;
    LevelSlider musicSlider_;
    LevelSlider friendsSlider_;

    std::atomic<float> micPeak_{ 0.0f };
    float smoothedPeak_ = 0.0f;
    bool micMuted_ = false;
    bool connected_ = false;
    juce::String roomToken_;
    juce::String deviceInfo_;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR(KaraokeContent)
};

MainWindow::MainWindow()
    : DocumentWindow("HAS Karaoke",
                     lnf_.pal().paper,
                     DocumentWindow::allButtons) {
    setLookAndFeel(&lnf_);
    lnf_.setDark(settings_->isDark());
    settings_->addChangeListener(this);

    // Initialise audio device manager with default audio device
    deviceManager_.initialiseWithDefaultDevices(1, 2);

    content_ = std::make_unique<KaraokeContent>(*this, deviceManager_);
    setContentOwned(content_.release(), false);

    setUsingNativeTitleBar(false);
    setResizable(true, true);
    setResizeLimits(580, 480, 1920, 1080);

    const auto saved = settings_->windowSize("karaoke");
    if (saved.x >= 580 && saved.y >= 480) {
        setSize(saved.x, saved.y);
    } else {
        setSize(740, 540);
    }

    centreWithSize(getWidth(), getHeight());
    setVisible(true);
}

MainWindow::~MainWindow() {
    settings_->removeChangeListener(this);
    clearContentComponent();
    setLookAndFeel(nullptr);
}

void MainWindow::closeButtonPressed() {
    const auto size = juce::Point<int>(getWidth(), getHeight());
    settings_->setWindowSize("karaoke", size);
    juce::JUCEApplication::getInstance()->systemRequestedQuit();
}

void MainWindow::handleCommandLine(const juce::String& cmd) {
    if (cmd.isNotEmpty() && content_ != nullptr) {
        content_->handleDeepLink(cmd);
    }
}

void MainWindow::handleDeepLink(const juce::String& link) {
    if (content_ != nullptr) {
        content_->handleDeepLink(link);
    }
}

void MainWindow::changeListenerCallback(juce::ChangeBroadcaster*) {
    applyTheme(settings_->isDark());
}

void MainWindow::applyTheme(bool dark) {
    lnf_.setDark(dark);
    setBackgroundColour(lnf_.pal().paper);
    repaint();
}

} // namespace hearaside::karaoke
