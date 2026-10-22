#include "MainWindow.h"
#include "AudioSettings.h"
#include "JoinClient.h"
#include "MusicPlayer.h"
#include "PreflightChecker.h"
#include "RoomHost.h"
#include "VocalChain.h"

#include <algorithm>
#include <cmath>

namespace hearaside::karaoke {

enum class AppMode {
    Singer,
    Host
};

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
          preflightBtn_(icons::Icon::Check, 34.0f),
          startStopRoomBtn_(icons::Icon::Broadcast, 36.0f),
          prevBtn_(icons::Icon::ChevronLeft, "เพลงก่อนหน้า"),
          nextBtn_(icons::Icon::ChevronRight, "เพลงถัดไป") {
        setWantsKeyboardFocus(true);
        settings_->addChangeListener(this);
        deviceManager_.addAudioCallback(this);

        // Mode switch tabs
        modeSwitch_.setSegments({
            { "เข้าห้อง (Singer)", icons::Icon::Count },
            { "เปิดห้อง (Host)", icons::Icon::Count }
        });
        modeSwitch_.setSelected(0, false);
        modeSwitch_.onChange = [this](int idx) {
            appMode_ = (idx == 0) ? AppMode::Singer : AppMode::Host;
            updateModeVisibility();
            resized();
            repaint();
        };
        addAndMakeVisible(modeSwitch_);

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

        // ==========================================
        // Singer Mode Controls
        // ==========================================
        linkInput_.setPlaceholder("วางลิงก์ส่งเสียง เช่น https://.../s/<token> หรือ has://join?u=...");
        linkInput_.onReturnKey = [this] { onJoinToggleClicked(); };
        addAndMakeVisible(linkInput_);

        joinBtn_.setButtonText("เข้าห้อง");
        joinBtn_.onClick = [this] { onJoinToggleClicked(); };
        addAndMakeVisible(joinBtn_);

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

        joinClient_.onStateChanged = [this](ConnectionState s, const RoomInfo& info) {
            juce::ignoreUnused(s, info);
            repaint();
        };

        // ==========================================
        // Host Mode Controls
        // ==========================================
        openFileBtn_.setButtonText("เลือกไฟล์เพลง");
        openFileBtn_.onClick = [this] {
            fileChooser_ = std::make_unique<juce::FileChooser>(
                "เลือกไฟล์เพลงสำหรับร้องคาราโอเกะ",
                juce::File::getSpecialLocation(juce::File::userMusicDirectory),
                "*.wav;*.mp3;*.flac;*.m4a;*.aac;*.ogg;*.aiff;*.wma"
            );
            const int flags = juce::FileBrowserComponent::openMode |
                              juce::FileBrowserComponent::canSelectFiles |
                              juce::FileBrowserComponent::canSelectMultipleItems;
            fileChooser_->launchAsync(flags, [this](const juce::FileChooser& fc) {
                auto results = fc.getResults();
                for (const auto& f : results) {
                    musicPlayer_.addTrack(f);
                }
                if (!results.isEmpty()) {
                    musicPlayer_.setSourceMode(MusicSourceMode::AudioFile);
                    musicPlayer_.play();
                    overlay_.toast("โหลดเพลงเรียบร้อย: " + results[0].getFileNameWithoutExtension(), 2000);
                }
                repaint();
            });
        };
        addChildComponent(openFileBtn_);

        captureAppBtn_.setButtonText("ดึงเสียงแอป (YouTube/Spotify)");
        captureAppBtn_.onClick = [this] {
            auto apps = MusicPlayer::listAudioApps();
            if (apps.empty()) {
                overlay_.toast("ไม่พบโปรแกรมที่กำลังเล่นเสียงอยู่ (เปิด YouTube หรือ Spotify ก่อน)", 3000);
                return;
            }
            juce::PopupMenu menu;
            for (int i = 0; i < (int)apps.size(); ++i) {
                const auto& app = apps[(size_t)i];
                juce::String label = juce::String::fromUTF8(app.name.c_str()) + " (" + juce::String::fromUTF8(app.exe.c_str()) + ")";
                menu.addItem(i + 1, label);
            }
            menu.showMenuAsync(juce::PopupMenu::Options{}.withTargetComponent(&captureAppBtn_), [this, apps](int result) {
                if (result > 0 && result <= (int)apps.size()) {
                    const auto& chosen = apps[(size_t)(result - 1)];
                    const uint32_t pid = AppCapture::findRootProcess(chosen.exe);
                    if (pid != 0) {
                        musicPlayer_.startAppCapture(pid, juce::String::fromUTF8(chosen.name.c_str()));
                        overlay_.toast("กำลังดึงเสียงจาก: " + juce::String::fromUTF8(chosen.name.c_str()), 2500);
                    } else {
                        overlay_.toast("ไม่พบโพรเซสของโปรแกรมนี้", 2000);
                    }
                    repaint();
                }
            });
        };
        addChildComponent(captureAppBtn_);

        prevBtn_.onClick = [this] { musicPlayer_.prevTrack(); };
        addChildComponent(prevBtn_);

        playPauseBtn_.setButtonText("เล่น");
        playPauseBtn_.onClick = [this] {
            if (musicPlayer_.isPlaying()) {
                musicPlayer_.pause();
                playPauseBtn_.setButtonText("เล่น");
            } else {
                musicPlayer_.play();
                playPauseBtn_.setButtonText("หยุดชั่วคราว");
            }
        };
        addChildComponent(playPauseBtn_);

        stopBtn_.setButtonText("หยุด");
        stopBtn_.onClick = [this] {
            musicPlayer_.stop();
            playPauseBtn_.setButtonText("เล่น");
        };
        addChildComponent(stopBtn_);

        nextBtn_.onClick = [this] { musicPlayer_.nextTrack(); };
        addChildComponent(nextBtn_);

        startStopRoomBtn_.setButtonText("เปิดห้องร้องเพลง");
        startStopRoomBtn_.onClick = [this] {
            if (roomHost_.isRoomRunning()) {
                roomHost_.stopRoom();
                startStopRoomBtn_.setButtonText("เปิดห้องร้องเพลง");
                startStopRoomBtn_.setActive(false);
                overlay_.toast("ปิดห้องคาราโอเกะเรียบร้อย", 1500);
            } else {
                bool ok = roomHost_.startRoom("Karaoke Room");
                if (ok) {
                    startStopRoomBtn_.setButtonText("ปิดห้อง");
                    startStopRoomBtn_.setActive(true);
                    overlay_.toast("เปิดห้องเรียบร้อย พร้อมร้องเพลงและส่งเสียงไป OBS", 2500);
                } else {
                    overlay_.toast("ไม่สามารถเปิดห้องได้ (พอร์ตอาจถูกใช้งานอยู่)", 2500);
                }
            }
            repaint();
        };
        addChildComponent(startStopRoomBtn_);

        copyFriendLinkBtn_.setButtonText("คัดลอกลิงก์เพื่อน");
        copyFriendLinkBtn_.onClick = [this] {
            if (!roomHost_.isRoomRunning()) {
                overlay_.toast("กรุณากด 'เปิดห้อง' ก่อนคัดลอกลิงก์", 1800);
                return;
            }
            const auto url = roomHost_.friendUrl(0);
            if (url.isNotEmpty()) {
                juce::SystemClipboard::copyTextToClipboard(url);
                overlay_.toast("คัดลอกลิงก์เพื่อนแล้ว (ส่งให้เพื่อนเข้ามาร้องด้วยกัน)", 2500);
            } else {
                overlay_.toast("กำลังสร้างลิงก์ กรุณารอสักครู่...", 1500);
            }
        };
        addChildComponent(copyFriendLinkBtn_);

        copyListenLinkBtn_.setButtonText("คัดลอกลิงก์คนฟัง");
        copyListenLinkBtn_.onClick = [this] {
            if (!roomHost_.isRoomRunning()) {
                overlay_.toast("กรุณากด 'เปิดห้อง' ก่อนคัดลอกลิงก์", 1800);
                return;
            }
            const auto url = roomHost_.listenUrl();
            if (url.isNotEmpty()) {
                juce::SystemClipboard::copyTextToClipboard(url);
                overlay_.toast("คัดลอกลิงก์คนฟังแล้ว (ส่งให้คนดูฟังสดผ่านเบราว์เซอร์)", 2500);
            } else {
                overlay_.toast("กำลังสร้างลิงก์ กรุณารอสักครู่...", 1500);
            }
        };
        addChildComponent(copyListenLinkBtn_);

        recordRoomBtn_.setButtonText("อัดเสียงห้อง WAV");
        recordRoomBtn_.onClick = [this] {
            if (roomHost_.isRecording()) {
                roomHost_.stopRecording();
                recordRoomBtn_.setButtonText("อัดเสียงห้อง WAV");
                recordRoomBtn_.setStyle(GhostButton::Style::Ghost);
                overlay_.toast("หยุดบันทึกเสียงและบันทึกไฟล์เรียบร้อย", 2000);
            } else {
                const auto dir = juce::File::getSpecialLocation(juce::File::userDocumentsDirectory)
                                    .getChildFile("HEARASIDE")
                                    .getChildFile("Recordings");
                const auto now = juce::Time::getCurrentTime();
                const auto file = dir.getNonexistentChildFile(
                    juce::File::createLegalFileName("Karaoke " + now.formatted("%Y-%m-%d %H-%M-%S")),
                    ".wav",
                    false
                );
                if (roomHost_.startRecording(file)) {
                    recordRoomBtn_.setButtonText("● กำลังอัดเสียง...");
                    recordRoomBtn_.setStyle(GhostButton::Style::DangerSolid);
                    overlay_.toast("เริ่มบันทึกเสียงห้องลงไฟล์: " + file.getFileName(), 2000);
                } else {
                    overlay_.toast("ไม่สามารถสร้างไฟล์สำหรับบันทึกเสียงได้", 2000);
                }
            }
            repaint();
        };
        addChildComponent(recordRoomBtn_);

        // 8 Friend Slot controls
        for (int i = 0; i < 8; ++i) {
            auto& c = friendSlotControls_[i];
            c.meter = std::make_unique<MeterBar>(4.0f, true);
            c.muteBtn = std::make_unique<GhostButton>("ปิดเสียง", GhostButton::Style::Ghost);
            c.muteBtn->setSmall(true);
            c.muteBtn->onClick = [this, i] {
                auto info = roomHost_.slotInfo(i);
                bool newMuted = !info.isMuted;
                roomHost_.setSlotMuted(i, newMuted);
                friendSlotControls_[i].muteBtn->setButtonText(newMuted ? "เปิดเสียง" : "ปิดเสียง");
                overlay_.toast(newMuted ? ("ปิดเสียง " + info.name + " แล้ว") : ("เปิดเสียง " + info.name + " แล้ว"), 1500);
            };

            c.kickBtn = std::make_unique<GhostButton>("เตะ", GhostButton::Style::Danger);
            c.kickBtn->setSmall(true);
            c.kickBtn->onClick = [this, i] {
                auto info = roomHost_.slotInfo(i);
                roomHost_.kickSlot(i);
                overlay_.toast("เตะ " + info.name + " ออกจากห้องแล้ว", 1800);
                repaint();
            };

            c.copyBtn = std::make_unique<IconButton>(icons::Icon::Link, "คัดลอกลิงก์สำหรับเพื่อนคนนี้");
            c.copyBtn->onClick = [this, i] {
                juce::String url = roomHost_.friendUrl(i);
                if (url.isNotEmpty()) {
                    juce::SystemClipboard::copyTextToClipboard(url);
                    overlay_.toast("คัดลอกลิงก์ของ " + roomHost_.slotInfo(i).name + " แล้ว", 2000);
                } else {
                    overlay_.toast("กรุณาเปิดห้องก่อนคัดลอกลิงก์", 1500);
                }
            };

            addChildComponent(*c.meter);
            addChildComponent(*c.muteBtn);
            addChildComponent(*c.kickBtn);
            addChildComponent(*c.copyBtn);
        }

        // ==========================================
        // Common Audio Controls (Mic & Sliders)
        // ==========================================
        addAndMakeVisible(micMeter_);

        muteBtn_.onClick = [this] {
            const bool m = !vocalChain_.isMuted();
            vocalChain_.setMuted(m);
            muteBtn_.setActive(m);
            overlay_.toast(m ? "ปิดไมค์แล้ว (Muted)" : "เปิดไมค์แล้ว (Unmuted)", 1500);
        };
        addAndMakeVisible(muteBtn_);

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

        addAndMakeVisible(overlay_);

        updateModeVisibility();
        startTimerHz(30);
        updateDeviceInfo();
    }

    ~KaraokeContent() override {
        stopTimer();
        preflight_.cancel();
        joinClient_.disconnect();
        roomHost_.stopRoom();
        deviceManager_.removeAudioCallback(this);
        settings_->removeChangeListener(this);
    }

    void updateModeVisibility() {
        const bool isSinger = (appMode_ == AppMode::Singer);

        // Singer components
        linkInput_.setVisible(isSinger);
        joinBtn_.setVisible(isSinger);
        preflightBtn_.setVisible(isSinger);
        speakerModeSwitch_.setVisible(isSinger);

        // Host components
        openFileBtn_.setVisible(!isSinger);
        captureAppBtn_.setVisible(!isSinger);
        prevBtn_.setVisible(!isSinger);
        playPauseBtn_.setVisible(!isSinger);
        stopBtn_.setVisible(!isSinger);
        nextBtn_.setVisible(!isSinger);
        startStopRoomBtn_.setVisible(!isSinger);
        copyFriendLinkBtn_.setVisible(!isSinger);
        copyListenLinkBtn_.setVisible(!isSinger);
        recordRoomBtn_.setVisible(!isSinger);

        for (int i = 0; i < 8; ++i) {
            auto& c = friendSlotControls_[i];
            c.meter->setVisible(!isSinger);
            c.muteBtn->setVisible(!isSinger);
            c.kickBtn->setVisible(!isSinger);
            c.copyBtn->setVisible(!isSinger);
        }
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
        if (clean.startsWithIgnoreCase("has://join?u=") || clean.startsWithIgnoreCase("hearaside://join?url=")) {
            int prefixLen = clean.startsWithIgnoreCase("has://join?u=") ? 13 : 21;
            clean = juce::URL::removeEscapeChars(clean.substring(prefixLen));
        }
        if (appMode_ != AppMode::Singer) {
            modeSwitch_.setSelected(0, true);
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
            musicPlayer_.prepare(sampleRate_, buf);
            roomHost_.prepare(sampleRate_, buf);

            const int allocSize = std::max(buf, 2048);
            scratchVocalL_.assign(allocSize, 0.0f);
            scratchVocalR_.assign(allocSize, 0.0f);
            scratchNetMic_.assign(allocSize, 0.0f);
            scratchDownL_.assign(allocSize, 0.0f);
            scratchDownR_.assign(allocSize, 0.0f);
            scratchMusicL_.assign(allocSize, 0.0f);
            scratchMusicR_.assign(allocSize, 0.0f);
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

        const float vocG = vocalGainLinear_;
        const float musG = musicGainLinear_;

        if (appMode_ == AppMode::Singer) {
            // ================= Singer Mode =================
            const bool live = joinClient_.isLive();
            if (live) {
                joinClient_.pushUpstream(scratchNetMic_.data(), n, timelineCounter_);
                joinClient_.pullDownstream(scratchDownL_.data(), scratchDownR_.data(), n);
            } else {
                std::fill_n(scratchDownL_.data(), n, 0.0f);
                std::fill_n(scratchDownR_.data(), n, 0.0f);
            }
            timelineCounter_ += n;

            preflight_.processAudio(inMic, scratchDownL_.data(), scratchDownR_.data(), n);

            if (numOutputChannels >= 2 && outputChannelData != nullptr) {
                float* outL = outputChannelData[0];
                float* outR = outputChannelData[1];
                for (int i = 0; i < n; ++i) {
                    float mixL = (scratchVocalL_[i] * vocG) + (scratchDownL_[i] * musG);
                    float mixR = (scratchVocalR_[i] * vocG) + (scratchDownR_[i] * musG);

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
        } else {
            // ================= Host Mode =================
            musicPlayer_.pullAudio(scratchMusicL_.data(), scratchMusicR_.data(), n);

            for (int i = 0; i < n; ++i) {
                scratchVocalL_[i] *= vocG;
                scratchVocalR_[i] *= vocG;
                scratchMusicL_[i] *= musG;
                scratchMusicR_[i] *= musG;
            }

            float* outL = (numOutputChannels >= 1 && outputChannelData != nullptr) ? outputChannelData[0] : nullptr;
            float* outR = (numOutputChannels >= 2 && outputChannelData != nullptr) ? outputChannelData[1] : outL;

            roomHost_.processAudio(scratchVocalL_.data(), scratchVocalR_.data(),
                                  scratchMusicL_.data(), scratchMusicR_.data(),
                                  outL, outR, n);
        }
    }

    // Timer (30 Hz)
    void timerCallback() override {
        const float p = micPeak_.load(std::memory_order_relaxed);
        smoothedPeak_ = std::max(p, smoothedPeak_ * 0.82f);
        micMeter_.setLevel(hearaside::meterPosition(smoothedPeak_));

        if (appMode_ == AppMode::Singer) {
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
        } else {
            // Host timer updates
            if (musicPlayer_.isPlaying()) {
                playPauseBtn_.setButtonText("หยุดชั่วคราว");
            } else {
                playPauseBtn_.setButtonText("เล่น");
            }

            if (roomHost_.isRecording()) {
                recordRoomBtn_.setButtonText("● อัดเสียง (" + juce::String(roomHost_.recordedSeconds(), 1) + "s)");
            } else {
                recordRoomBtn_.setButtonText("อัดเสียงห้อง WAV");
            }

            for (int i = 0; i < 8; ++i) {
                auto info = roomHost_.slotInfo(i);
                friendSlotControls_[i].meter->setLevel(hearaside::meterPosition(info.peak));
            }
        }

        updateDeviceInfo();
        repaint();
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
        drawWordmark(g, headerArea.reduced(24.0f, 14.0f).removeFromLeft(130.0f), "", 22.0f, p.ink);
        drawBadge(g, juce::Rectangle<float>(160.0f, 18.0f, 72.0f, 22.0f), "KARAOKE", BadgeStyle::Solid, p);

        // Header Status
        Dot dot = Dot::None;
        juce::String statusText = "พร้อมใช้งาน";

        if (appMode_ == AppMode::Singer) {
            const auto cState = joinClient_.state();
            const auto room = joinClient_.roomInfo();

            if (cState == ConnectionState::Live) {
                dot = Dot::Ok;
                statusText = room.name.isNotEmpty() ? ("ในห้อง: " + room.name) : "ในห้อง (Live)";
                if (room.delayMs >= 0.0f) {
                    statusText += " (~" + juce::String(juce::roundToInt(room.delayMs)) + " ms)";
                }
            } else if (cState == ConnectionState::Connecting || cState == ConnectionState::Resolving || cState == ConnectionState::Handshaking) {
                dot = Dot::Warn;
                statusText = "กำลังต่อเข้าห้อง...";
            } else if (cState == ConnectionState::Reconnecting) {
                dot = Dot::Warn;
                statusText = "กำลังเชื่อมต่อใหม่...";
            } else {
                statusText = "พร้อมเข้าห้อง";
            }
        } else {
            if (roomHost_.isRoomRunning()) {
                dot = Dot::Ok;
                statusText = "เปิดห้องแล้ว (พอร์ต " + juce::String(roomHost_.serverPort()) + ")";
            } else {
                dot = Dot::None;
                statusText = "ห้องยังไม่เปิด";
            }
        }

        drawStatusDot(g, juce::Point<float>(510.0f, 29.0f), dot, p, 7.0f);
        g.setColour(p.graphite);
        g.setFont(uiFont(13.0f, Weight::Medium));
        g.drawText(statusText, juce::Rectangle<float>(522.0f, 17.0f, 160.0f, 24.0f), juce::Justification::centredLeft);

        // Divider
        g.setColour(p.hairline1);
        g.drawHorizontalLine(60, 0.0f, float(getWidth()));

        const float margin = 24.0f;
        const float contentW = float(getWidth()) - (margin * 2.0f);

        if (appMode_ == AppMode::Singer) {
            paintSingerView(g, p, margin, contentW);
        } else {
            paintHostView(g, p, margin, contentW);
        }
    }

    void paintSingerView(juce::Graphics& g, const theme::Palette& p, float margin, float contentW) {
        // Badges in Header if Live
        const auto cState = joinClient_.state();
        const auto room = joinClient_.roomInfo();
        if (cState == ConnectionState::Live) {
            float badgeX = 660.0f;
            const juce::String codecLabel = room.codec.isNotEmpty() ? room.codec.toUpperCase() : "OPUS";
            drawBadge(g, juce::Rectangle<float>(badgeX, 19.0f, 52.0f, 20.0f), codecLabel, BadgeStyle::Chip, p);
        }

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

        // Card 2: Audio & Microphone Inset
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

        g.setColour(p.ink);
        g.setFont(uiFont(12.0f, Weight::Medium));
        g.drawText("โหมดลำโพง (ตัดเสียงสะท้อน AEC)",
                   juce::Rectangle<float>(audioCard.getX() + 18.0f, audioCard.getY() + 92.0f, 180.0f, 22.0f), juce::Justification::centredLeft);

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

    void paintHostView(juce::Graphics& g, const theme::Palette& p, float margin, float contentW) {
        // Card 1: Music Player
        const auto musicCard = juce::Rectangle<float>(margin, 70.0f, contentW, 102.0f);
        drawInset(g, musicCard, 14.0f, p);

        g.setColour(p.ink);
        g.setFont(uiFont(14.0f, Weight::SemiBold));
        g.drawText("แหล่งเพลง (Music Player / App Audio)", musicCard.reduced(16.0f, 10.0f).removeFromTop(20.0f), juce::Justification::topLeft);

        // Song title and duration
        juce::String trackTitle = "ยังไม่ได้เลือกเพลง (เล่นไฟล์ WAV/MP3 หรือดึงจากเบราว์เซอร์)";
        if (musicPlayer_.sourceMode() == MusicSourceMode::AudioFile) {
            const int idx = musicPlayer_.currentTrackIndex();
            if (idx >= 0 && idx < musicPlayer_.trackCount()) {
                const auto trk = musicPlayer_.trackAt(idx);
                const int curM = int(musicPlayer_.currentPositionSec()) / 60;
                const int curS = int(musicPlayer_.currentPositionSec()) % 60;
                const int lenM = int(trk.durationSec) / 60;
                const int lenS = int(trk.durationSec) % 60;
                trackTitle = trk.title + " (" + juce::String::formatted("%02d:%02d / %02d:%02d", curM, curS, lenM, lenS) + ")";
            }
        } else if (musicPlayer_.sourceMode() == MusicSourceMode::AppCapture) {
            trackTitle = "ดึงเสียงจากแอป: " + musicPlayer_.capturedAppName();
        }

        g.setColour(p.graphite);
        g.setFont(uiFont(12.0f, Weight::Medium));
        g.drawText(trackTitle, juce::Rectangle<float>(musicCard.getX() + 16.0f, musicCard.getY() + 72.0f, contentW - 200.0f, 22.0f), juce::Justification::centredLeft);

        // Card 2: Host Room & OBS Broadcast
        const auto hostCard = juce::Rectangle<float>(margin, 180.0f, contentW, 82.0f);
        drawInset(g, hostCard, 14.0f, p);

        g.setColour(p.ink);
        g.setFont(uiFont(14.0f, Weight::SemiBold));
        g.drawText("ห้องร้องเพลงและส่งสัญญาณ (Room Host & OBS)", hostCard.reduced(16.0f, 10.0f).removeFromTop(20.0f), juce::Justification::topLeft);

        // OBS Status badge
        if (roomHost_.isRoomRunning()) {
            drawBadge(g, juce::Rectangle<float>(musicCard.getX() + contentW - 130.0f, hostCard.getY() + 10.0f, 114.0f, 20.0f),
                      roomHost_.hasDawCollision() ? "OBS: Bus Karaoke" : "OBS: Bus Main",
                      BadgeStyle::Chip, p);
        }

        // Card 3: Friends Room (8 Slots)
        const auto friendCard = juce::Rectangle<float>(margin, 270.0f, contentW, 166.0f);
        drawInset(g, friendCard, 14.0f, p);

        g.setColour(p.ink);
        g.setFont(uiFont(14.0f, Weight::SemiBold));
        g.drawText("เพื่อนในห้องร้องเพลง (8 ช่อง)", friendCard.reduced(16.0f, 10.0f).removeFromTop(20.0f), juce::Justification::topLeft);

        // Draw slots names and status
        const float colW = (contentW - 40.0f) * 0.5f;
        for (int i = 0; i < 8; ++i) {
            const int col = i / 4;
            const int row = i % 4;
            const float sx = friendCard.getX() + 16.0f + float(col) * (colW + 16.0f);
            const float sy = friendCard.getY() + 34.0f + float(row) * 31.0f;

            auto info = roomHost_.slotInfo(i);
            drawStatusDot(g, juce::Point<float>(sx + 6.0f, sy + 11.0f), info.isLive ? Dot::Ok : Dot::None, p, 5.0f);

            g.setColour(info.isLive ? p.ink : p.muted);
            g.setFont(uiFont(11.5f, Weight::Medium));
            g.drawText(info.name, juce::Rectangle<float>(sx + 16.0f, sy, 70.0f, 22.0f), juce::Justification::centredLeft);

            if (info.isLive && info.delayMs >= 0.0f) {
                drawBadge(g, juce::Rectangle<float>(sx + 88.0f, sy + 2.0f, 44.0f, 18.0f),
                          juce::String(juce::roundToInt(info.delayMs)) + "ms", BadgeStyle::Chip, p);
            }
        }

        // Card 4: Host Microphone & Monitor Balance
        const auto monitorCard = juce::Rectangle<float>(margin, 444.0f, contentW, 126.0f);
        drawInset(g, monitorCard, 14.0f, p);

        g.setColour(p.ink);
        g.setFont(uiFont(14.0f, Weight::SemiBold));
        g.drawText("ไมค์เจ้าของห้องและเอฟเฟกต์ (Host Vocal & Effects)", monitorCard.reduced(16.0f, 8.0f).removeFromTop(20.0f), juce::Justification::topLeft);

        g.setColour(p.graphite);
        g.setFont(uiFont(11.5f, Weight::Regular));
        g.drawText("เสียงฉัน (Voice)", juce::Rectangle<float>(monitorCard.getX() + 16.0f, monitorCard.getY() + 34.0f, 90.0f, 20.0f), juce::Justification::centredLeft);
        g.drawText("เสียงเพลง (Music)", juce::Rectangle<float>(monitorCard.getX() + 16.0f, monitorCard.getY() + 58.0f, 90.0f, 20.0f), juce::Justification::centredLeft);
        g.drawText("รีเวิร์บ (Reverb)", juce::Rectangle<float>(monitorCard.getX() + 16.0f, monitorCard.getY() + 82.0f, 90.0f, 20.0f), juce::Justification::centredLeft);
        g.drawText("เอคโค่ (Echo)", juce::Rectangle<float>(monitorCard.getX() + 16.0f, monitorCard.getY() + 104.0f, 90.0f, 20.0f), juce::Justification::centredLeft);
    }

    void resized() override {
        overlay_.setBounds(getLocalBounds());

        const int w = getWidth();
        themeToggleBtn_.setBounds(w - 76, 12, 34, 34);
        settingsBtn_.setBounds(w - 38, 12, 34, 34);

        // Mode switch tabs in header
        modeSwitch_.setBounds(240, 14, 250, 32);

        const int margin = 24;
        const int contentW = w - (margin * 2);

        if (appMode_ == AppMode::Singer) {
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
        } else {
            // Host Mode Layout
            // Card 1: Music Player
            openFileBtn_.setBounds(margin + 16, 100, 110, 28);
            captureAppBtn_.setBounds(margin + 134, 100, 180, 28);
            prevBtn_.setBounds(margin + 322, 100, 28, 28);
            playPauseBtn_.setBounds(margin + 356, 100, 84, 28);
            stopBtn_.setBounds(margin + 446, 100, 56, 28);
            nextBtn_.setBounds(margin + 508, 100, 28, 28);

            // Card 2: Room Host & OBS
            startStopRoomBtn_.setBounds(margin + 16, 218, 130, 32);
            copyFriendLinkBtn_.setBounds(margin + 154, 218, 126, 32);
            copyListenLinkBtn_.setBounds(margin + 288, 218, 126, 32);
            recordRoomBtn_.setBounds(margin + 422, 218, 150, 32);

            // Card 3: Friends Room (8 Slots)
            const int colW = (contentW - 40) / 2;
            for (int i = 0; i < 8; ++i) {
                const int col = i / 4;
                const int row = i % 4;
                const int sx = margin + 16 + col * (colW + 16);
                const int sy = 304 + row * 31;

                auto& c = friendSlotControls_[i];
                c.meter->setBounds(sx + 136, sy + 7, colW - 270, 6);
                c.muteBtn->setBounds(sx + colW - 124, sy, 52, 22);
                c.kickBtn->setBounds(sx + colW - 68, sy, 36, 22);
                c.copyBtn->setBounds(sx + colW - 28, sy, 22, 22);
            }

            // Card 4: Host Microphone & Monitor Balance
            micMeter_.setBounds(margin + 48, 546, 100, 6);
            muteBtn_.setBounds(w - margin - 100, 472, 84, 28);

            const int sliderX = margin + 110;
            const int sliderW = contentW - 230;
            vocalSlider_.setBounds(sliderX, 476, sliderW, 20);
            musicSlider_.setBounds(sliderX, 500, sliderW, 20);
            reverbSlider_.setBounds(sliderX, 524, sliderW, 20);
            echoSlider_.setBounds(sliderX, 548, sliderW, 20);
        }
    }

private:
    MainWindow& owner_;
    juce::AudioDeviceManager& deviceManager_;
    SharedSettings settings_;
    Overlay overlay_;

    SegmentedControl modeSwitch_;
    AppMode appMode_ = AppMode::Singer;

    // Singer components
    TextField linkInput_;
    PrimaryButton joinBtn_;
    IconButton themeToggleBtn_;
    IconButton settingsBtn_;
    MuteButton muteBtn_;
    PrimaryButton preflightBtn_;
    Switch speakerModeSwitch_;

    // Host components
    GhostButton openFileBtn_{ "เลือกไฟล์เพลง" };
    GhostButton captureAppBtn_{ "ดึงเสียงแอป (YouTube/Spotify)" };
    IconButton prevBtn_;
    GhostButton playPauseBtn_{ "เล่น" };
    GhostButton stopBtn_{ "หยุด" };
    IconButton nextBtn_;
    PrimaryButton startStopRoomBtn_;
    GhostButton copyFriendLinkBtn_{ "คัดลอกลิงก์เพื่อน" };
    GhostButton copyListenLinkBtn_{ "คัดลอกลิงก์คนฟัง" };
    GhostButton recordRoomBtn_{ "อัดเสียงห้อง WAV" };
    std::unique_ptr<juce::FileChooser> fileChooser_;

    struct SlotRowControls {
        std::unique_ptr<MeterBar> meter;
        std::unique_ptr<GhostButton> muteBtn;
        std::unique_ptr<GhostButton> kickBtn;
        std::unique_ptr<IconButton> copyBtn;
    };
    std::array<SlotRowControls, 8> friendSlotControls_;

    MeterBar micMeter_;
    LevelSlider vocalSlider_;
    LevelSlider musicSlider_;
    LevelSlider reverbSlider_;
    LevelSlider echoSlider_;
    Switch sendWetSwitch_;

    VocalChain vocalChain_;
    JoinClient joinClient_;
    PreflightChecker preflight_;
    MusicPlayer musicPlayer_;
    RoomHost roomHost_;

    double sampleRate_ = 48000.0;
    float vocalGainLinear_ = 1.0f;
    float musicGainLinear_ = 1.0f;
    uint64_t timelineCounter_ = 0;

    std::vector<float> scratchVocalL_;
    std::vector<float> scratchVocalR_;
    std::vector<float> scratchNetMic_;
    std::vector<float> scratchDownL_;
    std::vector<float> scratchDownR_;
    std::vector<float> scratchMusicL_;
    std::vector<float> scratchMusicR_;

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
    setResizeLimits(680, 580, 1920, 1080);

    const auto saved = settings_->windowSize("karaoke");
    if (saved.x >= 680 && saved.y >= 580) {
        setSize(saved.x, saved.y);
    } else {
        setSize(780, 600);
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
