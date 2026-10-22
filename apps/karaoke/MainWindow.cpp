#include "MainWindow.h"
#include "AudioSettings.h"
#include "JoinClient.h"
#include "PreflightChecker.h"
#include "VocalChain.h"

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
          settingsBtn_(icons::Icon::Sliders, "ตั้งค่าอุปกรณ์เสียง (Audio Settings)"),
          muteBtn_(),
          micMeter_(6.0f, false),
          preflightBtn_(icons::Icon::Check, 34.0f) {
        setWantsKeyboardFocus(true);
        settings_->addChangeListener(this);
        deviceManager_.addAudioCallback(this);

        // Header controls
        themeToggleBtn_.onClick = [this] {
            settings_->setThemeMode(settings_->isDark() ? ThemeMode::Light : ThemeMode::Dark);
        };
        addAndMakeVisible(themeToggleBtn_);

        settingsBtn_.onClick = [this] {
            AudioSettings::showSettingsDialog(deviceManager_, pal(), [this] {
                updateDeviceInfo();
            });
        };
        addAndMakeVisible(settingsBtn_);

        // Join Section
        linkInput_.setPlaceholder("วางลิงก์ส่งเสียง เช่น https://.../s/<token> หรือ has://join?u=...");
        linkInput_.onReturnKey = [this] { onJoinToggleClicked(); };
        addAndMakeVisible(linkInput_);

        joinBtn_.setButtonText("เข้าห้อง");
        joinBtn_.onClick = [this] { onJoinToggleClicked(); };
        addAndMakeVisible(joinBtn_);

        // Monitor & Mic
        addAndMakeVisible(micMeter_);

        muteBtn_.onClick = [this] {
            const bool m = !vocalChain_.isMuted();
            vocalChain_.setMuted(m);
            muteBtn_.setActive(m);
            overlay_.toast(m ? "ปิดไมค์แล้ว (Muted)" : "เปิดไมค์แล้ว (Unmuted)", 1500);
        };
        addAndMakeVisible(muteBtn_);

        preflightBtn_.setButtonText("เช็กก่อนร้อง");
        preflightBtn_.onClick = [this] {
            if (preflight_.isTesting()) {
                preflight_.cancel();
                preflightBtn_.setButtonText("เช็กก่อนร้อง");
                preflightBtn_.setActive(false);
                overlay_.toast("ยกเลิกการตรวจเสียง", 1200);
            } else {
                const auto room = joinClient_.roomInfo();
                preflight_.startCheck(room.rttMs, room.jitterP95Ms, joinClient_.isSpeakerMode());
                preflightBtn_.setButtonText("กำลังตรวจ...");
                preflightBtn_.setActive(true);
                overlay_.toast("กำลังตรวจไมค์และลำโพง...", 1500);
            }
            repaint();
        };
        addAndMakeVisible(preflightBtn_);

        speakerModeSwitch_.onClick = [this] {
            const bool spk = !speakerModeSwitch_.isOn();
            speakerModeSwitch_.setOn(spk, true);
            joinClient_.setSpeakerMode(spk);
            overlay_.toast(spk ? "เปิดโหมดลำโพง (ตัดเสียงสะท้อน AEC)" : "ปิดโหมดลำโพง (โหมดหูฟัง)", 2000);
            repaint();
        };
        addAndMakeVisible(speakerModeSwitch_);

        vocalSlider_.setRange(-30.0, 6.0, 0.5);
        vocalSlider_.setValue(0.0, juce::dontSendNotification);
        vocalSlider_.onValueChange = [this] {
            vocalGainLinear_ = std::pow(10.0f, float(vocalSlider_.getValue()) * 0.05f);
        };
        addAndMakeVisible(vocalSlider_);

        musicSlider_.setRange(-30.0, 6.0, 0.5);
        musicSlider_.setValue(0.0, juce::dontSendNotification);
        musicSlider_.onValueChange = [this] {
            musicGainLinear_ = std::pow(10.0f, float(musicSlider_.getValue()) * 0.05f);
        };
        addAndMakeVisible(musicSlider_);

        reverbSlider_.setRange(0.0, 100.0, 1.0);
        reverbSlider_.setValue(25.0, juce::dontSendNotification);
        reverbSlider_.onValueChange = [this] {
            vocalChain_.setReverbLevel(float(reverbSlider_.getValue()) / 100.0f);
        };
        addAndMakeVisible(reverbSlider_);

        echoSlider_.setRange(0.0, 100.0, 1.0);
        echoSlider_.setValue(20.0, juce::dontSendNotification);
        echoSlider_.onValueChange = [this] {
            vocalChain_.setEchoLevel(float(echoSlider_.getValue()) / 100.0f);
        };
        addAndMakeVisible(echoSlider_);

        sendWetSwitch_.onClick = [this] {
            const bool wet = !sendWetSwitch_.isOn();
            sendWetSwitch_.setOn(wet, true);
            vocalChain_.setSendWet(wet);
            overlay_.toast(wet ? "ส่งเสียง Wet (มี Reverb/Echo ไปให้เพื่อน)" : "ส่งเสียง Dry (เสียงคลีน ให้ห้องใส่เอฟเฟกต์)", 2000);
        };
        addAndMakeVisible(sendWetSwitch_);

        joinClient_.onStateChanged = [this](ConnectionState s, const RoomInfo& info) {
            juce::ignoreUnused(s, info);
            repaint();
        };

        addAndMakeVisible(overlay_);

        startTimerHz(30);
        updateDeviceInfo();
    }

    ~KaraokeContent() override {
        stopTimer();
        preflight_.cancel();
        joinClient_.disconnect();
        deviceManager_.removeAudioCallback(this);
        settings_->removeChangeListener(this);
    }

    bool keyPressed(const juce::KeyPress& key) override {
        if (key.isKeyCode(juce::KeyPress::spaceKey) || key.getKeyCode() == 'M' || key.getKeyCode() == 'm') {
            const bool m = !vocalChain_.isMuted();
            vocalChain_.setMuted(m);
            muteBtn_.setActive(m);
            overlay_.toast(m ? "ปิดไมค์แล้ว (Muted)" : "เปิดไมค์แล้ว (Unmuted)", 1500);
            return true;
        }
        return false;
    }

    void handleDeepLink(const juce::String& link) {
        juce::String clean = link.trim();
        if (clean.startsWithIgnoreCase("has://join?u=")) {
            clean = juce::URL::removeEscapeChars(clean.substring(13));
        }
        linkInput_.setText(clean, juce::dontSendNotification);
        overlay_.toast("ได้รับลิงก์ห้องแล้ว: กด 'เข้าห้อง' เพื่อเริ่ม", 2500);
    }

    void onJoinToggleClicked() {
        if (joinClient_.isLive() || joinClient_.state() == ConnectionState::Connecting) {
            joinClient_.disconnect();
            joinBtn_.setButtonText("เข้าห้อง");
            joinBtn_.setActive(false);
            overlay_.toast("ออกจากห้องเรียบร้อย", 1500);
            repaint();
            return;
        }

        const juce::String raw = linkInput_.getText().trim();
        if (raw.isEmpty()) {
            overlay_.toast("กรุณาวางลิงก์ส่งเสียงที่ได้รับจากเพื่อน", 2000);
            return;
        }

        joinClient_.connect(raw);
        joinBtn_.setButtonText("ยกเลิก");
        joinBtn_.setActive(true);
        overlay_.toast("กำลังเชื่อมต่อห้อง...", 2000);
        repaint();
    }

    // AudioIODeviceCallback
    void audioDeviceAboutToStart(juce::AudioIODevice* dev) override {
        if (dev != nullptr) {
            const double rate = dev->getCurrentSampleRate();
            const int buf = dev->getCurrentBufferSizeSamples();
            sampleRate_ = rate > 0 ? rate : 48000.0;

            vocalChain_.prepare(sampleRate_, buf);
            joinClient_.prepare(sampleRate_, buf);
            preflight_.prepare(sampleRate_);

            const int allocSize = std::max(buf, 2048);
            scratchVocalL_.assign(allocSize, 0.0f);
            scratchVocalR_.assign(allocSize, 0.0f);
            scratchNetMic_.assign(allocSize, 0.0f);
            scratchDownL_.assign(allocSize, 0.0f);
            scratchDownR_.assign(allocSize, 0.0f);
        }
    }

    void audioDeviceStopped() override {
        micPeak_.store(0.0f, std::memory_order_relaxed);
    }

    void audioDeviceIOCallbackWithContext(const float* const* inputChannelData, int numInputChannels,
                                          float* const* outputChannelData, int numOutputChannels,
                                          int numSamples,
                                          const juce::AudioIODeviceCallbackContext&) override {
        const int n = std::min(numSamples, (int)scratchVocalL_.size());

        // Step 1: Input mic & local vocal chain
        const float* inMic = (numInputChannels > 0 && inputChannelData != nullptr) ? inputChannelData[0] : nullptr;

        float peak = 0.0f;
        if (inMic != nullptr) {
            for (int i = 0; i < n; ++i) {
                const float absV = std::abs(inMic[i]);
                if (absV > peak) peak = absV;
            }
        }
        micPeak_.store(peak, std::memory_order_relaxed);

        vocalChain_.process(inMic, scratchVocalL_.data(), scratchVocalR_.data(), scratchNetMic_.data(), n);

        // Step 2: Network stream
        const bool live = joinClient_.isLive();
        if (live) {
            joinClient_.pushUpstream(scratchNetMic_.data(), n, timelineCounter_);
            joinClient_.pullDownstream(scratchDownL_.data(), scratchDownR_.data(), n);
        } else {
            std::fill_n(scratchDownL_.data(), n, 0.0f);
            std::fill_n(scratchDownR_.data(), n, 0.0f);
        }
        timelineCounter_ += n;

        // Preflight acoustic test chirp injection & mic monitoring
        preflight_.processAudio(inMic, scratchDownL_.data(), scratchDownR_.data(), n);

        // Step 3: Mix to headphone output
        const float vocG = vocalGainLinear_;
        const float musG = musicGainLinear_;

        if (numOutputChannels >= 2 && outputChannelData != nullptr) {
            float* outL = outputChannelData[0];
            float* outR = outputChannelData[1];
            for (int i = 0; i < n; ++i) {
                float mixL = (scratchVocalL_[i] * vocG) + (scratchDownL_[i] * musG);
                float mixR = (scratchVocalR_[i] * vocG) + (scratchDownR_[i] * musG);

                // Soft limiter / soft knee saturation to protect ears from loud peaks
                mixL = std::clamp(mixL, -1.2f, 1.2f);
                mixR = std::clamp(mixR, -1.2f, 1.2f);
                if (mixL > 0.8f) mixL = 0.8f + (mixL - 0.8f) * 0.3f;
                else if (mixL < -0.8f) mixL = -0.8f + (mixL + 0.8f) * 0.3f;
                if (mixR > 0.8f) mixR = 0.8f + (mixR - 0.8f) * 0.3f;
                else if (mixR < -0.8f) mixR = -0.8f + (mixR + 0.8f) * 0.3f;

                outL[i] = mixL;
                outR[i] = mixR;
            }
        } else if (numOutputChannels == 1 && outputChannelData != nullptr) {
            float* out = outputChannelData[0];
            for (int i = 0; i < n; ++i) {
                float mix = (0.5f * (scratchVocalL_[i] + scratchVocalR_[i]) * vocG) +
                            (0.5f * (scratchDownL_[i] + scratchDownR_[i]) * musG);
                out[i] = std::clamp(mix, -0.95f, 0.95f);
            }
        }
    }

    // Timer (30 Hz)
    void timerCallback() override {
        const float p = micPeak_.load(std::memory_order_relaxed);
        smoothedPeak_ = std::max(p, smoothedPeak_ * 0.82f);
        micMeter_.setLevel(hearaside::meterPosition(smoothedPeak_));

        if (joinClient_.isLive()) {
            joinBtn_.setButtonText("ออกจากห้อง");
            joinBtn_.setActive(true);
        } else if (joinClient_.state() == ConnectionState::Connecting) {
            joinBtn_.setButtonText("กำลังเชื่อมต่อ...");
            joinBtn_.setActive(true);
        } else {
            joinBtn_.setButtonText("เข้าห้อง");
            joinBtn_.setActive(false);
        }

        if (preflight_.isTesting()) {
            preflightBtn_.setButtonText("กำลังตรวจ...");
            preflightBtn_.setActive(true);
        } else {
            preflightBtn_.setButtonText("เช็กก่อนร้อง");
            preflightBtn_.setActive(false);
        }

        updateDeviceInfo();
    }

    void updateDeviceInfo() {
        const auto info = AudioSettings::getLatencyInfo(deviceManager_);
        if (info.sampleRate > 0) {
            deviceInfo_ = info.driverType + ": " + info.deviceName +
                          " (" + juce::String(info.sampleRate / 1000) + " kHz, " +
                          juce::String(info.bufferSize) + " smp, latency ~" +
                          juce::String(info.totalMs, 1) + " ms)";
            joinClient_.setLocalLatencyMs(info.totalMs);
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
        const auto cState = joinClient_.state();
        const auto room = joinClient_.roomInfo();

        Dot dot = Dot::None;
        juce::String statusText = "พร้อมเข้าห้อง";
        if (cState == ConnectionState::Live) {
            dot = Dot::Ok;
            statusText = room.name.isNotEmpty() ? ("ในห้อง: " + room.name) : "ในห้อง (Live)";
            if (room.delayMs >= 0.0f) {
                statusText += " (ช้า ~" + juce::String(juce::roundToInt(room.delayMs)) + " ms)";
            }
        } else if (cState == ConnectionState::Connecting || cState == ConnectionState::Resolving || cState == ConnectionState::Handshaking) {
            dot = Dot::Warn;
            statusText = "กำลังต่อเข้าห้อง...";
        } else if (cState == ConnectionState::Reconnecting) {
            dot = Dot::Warn;
            statusText = "กำลังเชื่อมต่อใหม่...";
        }

        drawStatusDot(g, juce::Point<float>(264.0f, 29.0f), dot, p, 7.0f);
        g.setColour(p.graphite);
        g.setFont(uiFont(13.0f, Weight::Medium));
        g.drawText(statusText, juce::Rectangle<float>(276.0f, 17.0f, 220.0f, 24.0f), juce::Justification::centredLeft);

        // Badges in Header if Live (K2.1 & K2.2)
        if (cState == ConnectionState::Live) {
            float badgeX = 500.0f;
            const juce::String codecLabel = room.codec.isNotEmpty() ? room.codec.toUpperCase() : "OPUS";
            drawBadge(g, juce::Rectangle<float>(badgeX, 19.0f, 52.0f, 20.0f), codecLabel, BadgeStyle::Chip, p);
            badgeX += 58.0f;

            if (room.jitterP95Ms >= 0.0f) {
                const juce::String jitStr = "Jit " + juce::String(juce::roundToInt(room.jitterP95Ms)) + "ms";
                drawBadge(g, juce::Rectangle<float>(badgeX, 19.0f, 64.0f, 20.0f), jitStr, BadgeStyle::Chip, p);
                badgeX += 70.0f;
            }

            if (room.bufferMs > 0.0f) {
                const juce::String bufStr = "Buf " + juce::String(juce::roundToInt(room.bufferMs)) + "ms";
                drawBadge(g, juce::Rectangle<float>(badgeX, 19.0f, 62.0f, 20.0f), bufStr, BadgeStyle::Chip, p);
            }
        }

        // Divider
        g.setColour(p.hairline1);
        g.drawHorizontalLine(60, 0.0f, float(getWidth()));

        // Cards layout
        const float margin = 24.0f;
        const float contentW = float(getWidth()) - (margin * 2.0f);

        // Card 1: Join Box (Inset)
        const auto joinCard = juce::Rectangle<float>(margin, 74.0f, contentW, 118.0f);
        drawInset(g, joinCard, 14.0f, p);

        g.setColour(p.ink);
        g.setFont(uiFont(15.0f, Weight::SemiBold));
        g.drawText("เข้าร่วมห้องร้องเพลง (Join Room)", joinCard.reduced(18.0f, 14.0f).removeFromTop(22.0f), juce::Justification::topLeft);

        g.setColour(p.graphite);
        g.setFont(uiFont(12.0f, Weight::Regular));
        g.drawText("วางลิงก์ส่งเสียงที่ได้รับจากเพื่อนเพื่อเริ่มร้องเพลงร่วมกัน (รองรับทั้ง /s/ และ has://join)",
                   juce::Rectangle<float>(joinCard.getX() + 18.0f, joinCard.getY() + 36.0f, contentW - 36.0f, 20.0f), juce::Justification::topLeft);

        // Card 2: Audio & Microphone Inset (K2.3 & K2.4)
        const auto audioCard = juce::Rectangle<float>(margin, 204.0f, contentW, 148.0f);
        drawInset(g, audioCard, 14.0f, p);

        g.setColour(p.ink);
        g.setFont(uiFont(15.0f, Weight::SemiBold));
        g.drawText("ไมโครโฟนและอุปกรณ์เสียง", audioCard.reduced(18.0f, 12.0f).removeFromTop(22.0f), juce::Justification::topLeft);

        g.setColour(p.graphite);
        g.setFont(uiFont(12.0f, Weight::Regular));
        g.drawText(deviceInfo_, juce::Rectangle<float>(audioCard.getX() + 18.0f, audioCard.getY() + 34.0f, contentW - 150.0f, 20.0f), juce::Justification::topLeft);

        g.setColour(p.muted);
        g.setFont(uiFont(11.0f, Weight::Regular));
        g.drawText("ไมค์:", juce::Rectangle<float>(audioCard.getX() + 18.0f, audioCard.getY() + 66.0f, 32.0f, 20.0f), juce::Justification::centredLeft);

        // Speaker mode toggle label
        g.setColour(p.ink);
        g.setFont(uiFont(12.0f, Weight::Medium));
        g.drawText("โหมดลำโพง (ตัดเสียงสะท้อน AEC)",
                   juce::Rectangle<float>(audioCard.getX() + 18.0f, audioCard.getY() + 92.0f, 180.0f, 22.0f), juce::Justification::centredLeft);

        // Preflight Report line
        const auto pf = preflight_.result();
        if (pf.leakStatus != LeakStatus::Untested) {
            Dot pfDot = pf.isReady ? Dot::Ok : Dot::Warn;
            drawStatusDot(g, juce::Point<float>(audioCard.getX() + 260.0f, audioCard.getY() + 103.0f), pfDot, p, 6.0f);
            g.setColour(pf.isReady ? p.ok : p.ink);
            g.setFont(uiFont(12.0f, Weight::SemiBold));
            g.drawText(pf.summary, juce::Rectangle<float>(audioCard.getX() + 272.0f, audioCard.getY() + 92.0f, 140.0f, 22.0f), juce::Justification::centredLeft);

            g.setColour(p.graphite);
            g.setFont(uiFont(11.5f, Weight::Regular));
            g.drawText(pf.advice, juce::Rectangle<float>(audioCard.getX() + 18.0f, audioCard.getY() + 118.0f, contentW - 36.0f, 20.0f), juce::Justification::centredLeft);
        } else {
            g.setColour(p.muted);
            g.setFont(uiFont(11.5f, Weight::Regular));
            g.drawText("กด 'เช็กก่อนร้อง' เพื่อตรวจไมค์ เสียงลำโพงสะท้อน และสัญญาณเน็ต",
                       juce::Rectangle<float>(audioCard.getX() + 260.0f, audioCard.getY() + 92.0f, contentW - 278.0f, 22.0f), juce::Justification::centredLeft);
        }

        // Card 3: Headphone Monitor Balance Inset
        const auto monitorCard = juce::Rectangle<float>(margin, 364.0f, contentW, 178.0f);
        drawInset(g, monitorCard, 14.0f, p);

        g.setColour(p.ink);
        g.setFont(uiFont(15.0f, Weight::SemiBold));
        g.drawText("ระดับเสียงและเอฟเฟกต์ (Vocal & Ambience)", monitorCard.reduced(18.0f, 14.0f).removeFromTop(22.0f), juce::Justification::topLeft);

        g.setColour(p.graphite);
        g.setFont(uiFont(12.0f, Weight::Regular));
        g.drawText("เสียงฉัน (Voice)", juce::Rectangle<float>(monitorCard.getX() + 18.0f, monitorCard.getY() + 38.0f, 110.0f, 22.0f), juce::Justification::centredLeft);
        g.drawText("เสียงเพลง (Music)", juce::Rectangle<float>(monitorCard.getX() + 18.0f, monitorCard.getY() + 66.0f, 110.0f, 22.0f), juce::Justification::centredLeft);
        g.drawText("รีเวิร์บ (Reverb)", juce::Rectangle<float>(monitorCard.getX() + 18.0f, monitorCard.getY() + 94.0f, 110.0f, 22.0f), juce::Justification::centredLeft);
        g.drawText("เอคโค่ (Echo)", juce::Rectangle<float>(monitorCard.getX() + 18.0f, monitorCard.getY() + 122.0f, 110.0f, 22.0f), juce::Justification::centredLeft);

        g.drawText("ส่งเสียง Wet ไปให้ห้อง", juce::Rectangle<float>(monitorCard.getX() + 18.0f, monitorCard.getY() + 148.0f, 160.0f, 20.0f), juce::Justification::centredLeft);
    }

    void resized() override {
        overlay_.setBounds(getLocalBounds());

        const int w = getWidth();
        themeToggleBtn_.setBounds(w - 76, 12, 34, 34);
        settingsBtn_.setBounds(w - 38, 12, 34, 34);

        const int margin = 24;
        const int contentW = w - (margin * 2);

        // Card 1: Join Row
        linkInput_.setBounds(margin + 18, 130, contentW - 146, 38);
        joinBtn_.setBounds(w - margin - 118, 130, 100, 38);

        // Card 2: Audio & Mic
        preflightBtn_.setBounds(w - margin - 128, 226, 110, 32);
        micMeter_.setBounds(margin + 54, 272, contentW - 190, 8);
        muteBtn_.setBounds(w - margin - 118, 258, 100, 34);
        speakerModeSwitch_.setBounds(margin + 204, 298, 44, 22);

        // Card 3: Sliders
        const int sliderX = margin + 140;
        const int sliderW = contentW - 160;
        vocalSlider_.setBounds(sliderX, 402, sliderW, 22);
        musicSlider_.setBounds(sliderX, 430, sliderW, 22);
        reverbSlider_.setBounds(sliderX, 458, sliderW, 22);
        echoSlider_.setBounds(sliderX, 486, sliderW, 22);
        sendWetSwitch_.setBounds(margin + 170, 512, 44, 22);
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
    PrimaryButton preflightBtn_;
    Switch speakerModeSwitch_;

    MeterBar micMeter_;
    LevelSlider vocalSlider_;
    LevelSlider musicSlider_;
    LevelSlider reverbSlider_;
    LevelSlider echoSlider_;
    Switch sendWetSwitch_;

    VocalChain vocalChain_;
    JoinClient joinClient_;
    PreflightChecker preflight_;

    double sampleRate_ = 48000.0;
    float vocalGainLinear_ = 1.0f;
    float musicGainLinear_ = 1.0f;
    uint64_t timelineCounter_ = 0;

    std::vector<float> scratchVocalL_;
    std::vector<float> scratchVocalR_;
    std::vector<float> scratchNetMic_;
    std::vector<float> scratchDownL_;
    std::vector<float> scratchDownR_;

    std::atomic<float> micPeak_{ 0.0f };
    float smoothedPeak_ = 0.0f;
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

    // Initialise audio device manager from saved settings or defaults
    AudioSettings::loadState(deviceManager_, settings_);

    content_ = std::make_unique<KaraokeContent>(*this, deviceManager_);
    setContentOwned(content_.release(), false);

    setUsingNativeTitleBar(false);
    setResizable(true, true);
    setResizeLimits(600, 560, 1920, 1080);

    const auto saved = settings_->windowSize("karaoke");
    if (saved.x >= 600 && saved.y >= 560) {
        setSize(saved.x, saved.y);
    } else {
        setSize(760, 580);
    }

    centreWithSize(getWidth(), getHeight());
    setVisible(true);
}

MainWindow::~MainWindow() {
    AudioSettings::saveState(deviceManager_, settings_);
    settings_->removeChangeListener(this);
    clearContentComponent();
    setLookAndFeel(nullptr);
}

void MainWindow::closeButtonPressed() {
    const auto size = juce::Point<int>(getWidth(), getHeight());
    settings_->setWindowSize("karaoke", size);
    AudioSettings::saveState(deviceManager_, settings_);
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
