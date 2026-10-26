#include "MainWindow.h"
#include "Utf8.h"
#include "AudioSettings.h"
#include "Backdrop.h"
#include "JoinClient.h"
#include "MusicPlayer.h"
#include "PreflightChecker.h"
#include "RoomHost.h"
#include "ScrollArea.h"
#include "SettingsPanel.h"
#include "UpdateChecker.h"
#include "VocalChain.h"

#include <algorithm>
#include <array>
#include <cmath>

namespace hearaside::karaoke {

enum class AppMode {
    Singer,
    Host
};

// "A new HEARASIDE is out": a row in the house style under the header
class UpdateBanner : public juce::Component {
public:
    UpdateBanner() {
        downloadBtn_.setSmall(true);
        downloadBtn_.onClick = [this] {
            if (info_.downloadUrl.isNotEmpty()) {
                juce::URL(info_.downloadUrl).launchInDefaultBrowser();
            }
        };
        downloadBtn_.setTooltip("เปิดหน้าดาวน์โหลด HEARASIDE รุ่นล่าสุด"_th);
        addAndMakeVisible(downloadBtn_);

        dismissBtn_.onClick = [this] {
            setVisible(false);
            if (onDismiss) onDismiss();
        };
        addAndMakeVisible(dismissBtn_);
    }

    void showUpdate(const ReleaseInfo& info) {
        info_ = info;
        setVisible(true);
        repaint();
    }

    std::function<void()> onDismiss;

    void paint(juce::Graphics& g) override {
        const auto& p = paletteOf(*this);
        const auto b = getLocalBounds().toFloat();
        drawInset(g, b, theme::radius::row, p);
        drawStatusDot(g, juce::Point<float>(b.getX() + 18.0f, b.getCentreY()), Dot::Ok, p);

        juce::String text = "พบ HEARASIDE รุ่นใหม่ "_th + info_.latestVersion + " (รุ่นปัจจุบัน "_th + info_.currentVersion + ")";
        if (info_.releaseDate.isNotEmpty()) text += " · "_th + info_.releaseDate;
        const auto f = uiFont(12.5f, Weight::Medium);
        const auto area = b.withTrimmedLeft(32.0f).withTrimmedRight(float(downloadBtn_.getWidth() + dismissBtn_.getWidth() + 24));
        g.setColour(p.ink);
        g.setFont(f);
        g.drawText(ellipsize(f, text, area.getWidth()), area, juce::Justification::centredLeft, false);
    }

    void resized() override {
        auto r = getLocalBounds().reduced(6, 0);
        dismissBtn_.setBounds(r.removeFromRight(30).withSizeKeepingCentre(30, 30));
        r.removeFromRight(6);
        const int dw = downloadBtn_.idealWidth();
        downloadBtn_.setBounds(r.removeFromRight(dw).withSizeKeepingCentre(dw, 28));
    }

private:
    ReleaseInfo info_;
    GhostButton downloadBtn_ { "ดาวน์โหลด"_th, GhostButton::Style::Solid };
    IconButton dismissBtn_ { icons::Icon::Close, "ปิดการแจ้งเตือน"_th };
};

// A scrolled column whose painting is done by the owner
struct PaintBody : juce::Component {
    std::function<void(juce::Graphics&)> painter;
    void paint(juce::Graphics& g) override { if (painter) painter(g); }
};

class KaraokeContent : public juce::Component,
                       public juce::Timer,
                       public juce::AudioIODeviceCallback,
                       private juce::ChangeListener {
public:
    KaraokeContent(MainWindow& owner, juce::AudioDeviceManager& dm)
        : owner_(owner), deviceManager_(dm),
          joinBtn_(icons::Icon::Broadcast, 40.0f),
          themeToggleBtn_(icons::Icon::Appearance, "เปลี่ยนธีม สว่าง / มืด"_th),
          settingsBtn_(icons::Icon::Sliders, "ตั้งค่าอุปกรณ์เสียง (Audio Settings)"_th),
          micMeter_(6.0f, false),
          preflightBtn_(icons::Icon::Check, 44.0f),
          startStopRoomBtn_(icons::Icon::Broadcast, 40.0f),
          prevBtn_(icons::Icon::ChevronLeft, "เพลงก่อนหน้า"_th),
          nextBtn_(icons::Icon::ChevronRight, "เพลงถัดไป"_th) {
        setWantsKeyboardFocus(true);
        settings_->addChangeListener(this);
        deviceManager_.addAudioCallback(this);

        // Mode switch tabs
        modeSwitch_.setSegments({
            { tr(Str::KaraokeModeSinger), icons::Icon::Headphones },
            { tr(Str::KaraokeModeHost), icons::Icon::Broadcast }
        });
        modeSwitch_.setTall(true);
        modeSwitch_.setSelected(0, false);
        modeSwitch_.onChange = [this](int idx) {
            appMode_ = (idx == 0) ? AppMode::Singer : AppMode::Host;
            applyMode();
            resized();
            repaint();
        };
        addAndMakeVisible(modeSwitch_);
        statusChip_.setInterceptsMouseClicks(false, false);
        addAndMakeVisible(statusChip_);

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
        linkInput_.setPlaceholder("วางลิงก์ส่งเสียง เช่น https://.../s/<token> หรือ has://join?u=..."_th);
        linkInput_.onReturnKey = [this] { onJoinToggleClicked(); };

        joinBtn_.setButtonText("เข้าห้อง"_th);
        joinBtn_.onClick = [this] { onJoinToggleClicked(); };

        preflightBtn_.setButtonText("เช็กก่อนร้อง"_th);
        preflightBtn_.onClick = [this] {
            if (preflight_.isTesting()) {
                preflight_.cancel();
                preflightBtn_.setButtonText("เช็กก่อนร้อง"_th);
                preflightBtn_.setActive(false);
                overlay_.toast("ยกเลิกการตรวจเสียง"_th, 1200);
            } else {
                const auto room = joinClient_.roomInfo();
                preflight_.startCheck(room.rttMs, room.jitterP95Ms, joinClient_.isSpeakerMode());
                preflightBtn_.setButtonText("กำลังตรวจ..."_th);
                preflightBtn_.setActive(true);
                overlay_.toast("กำลังตรวจไมค์และลำโพง..."_th, 1500);
            }
            repaint();
        };

        speakerRow_.onClick = [this] {
            const bool spk = !speakerRow_.isOn();
            speakerRow_.setOn(spk, true);
            joinClient_.setSpeakerMode(spk);
            overlay_.toast(spk ? "เปิดโหมดลำโพง (ตัดเสียงสะท้อน AEC)"_th : "ปิดโหมดลำโพง (โหมดหูฟัง)"_th, 2000);
            repaint();
        };

        joinClient_.onStateChanged = [this](ConnectionState s, const RoomInfo& info) {
            juce::ignoreUnused(s, info);
            repaint();
        };

        // ==========================================
        // Host Mode Controls
        // ==========================================
        openFileBtn_.setButtonText("เลือกไฟล์เพลง"_th);
        openFileBtn_.setIcon(icons::Icon::Folder);
        openFileBtn_.onClick = [this] {
            fileChooser_ = std::make_unique<juce::FileChooser>(
                "เลือกไฟล์เพลงสำหรับร้องคาราโอเกะ"_th,
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
                    overlay_.toast("โหลดเพลงเรียบร้อย: "_th + results[0].getFileNameWithoutExtension(), 2000);
                }
                repaint();
            });
        };

        captureAppBtn_.setButtonText("ดึงเสียงแอป (YouTube/Spotify)"_th);
        captureAppBtn_.setIcon(icons::Icon::Window);
        captureAppBtn_.onClick = [this] {
            auto apps = MusicPlayer::listAudioApps();
            if (apps.empty()) {
                overlay_.toast("ไม่พบโปรแกรมที่กำลังเล่นเสียงอยู่ (เปิด YouTube หรือ Spotify ก่อน)"_th, 3000);
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
                        overlay_.toast("กำลังดึงเสียงจาก: "_th + juce::String::fromUTF8(chosen.name.c_str()), 2500);
                    } else {
                        overlay_.toast("ไม่พบโพรเซสของโปรแกรมนี้"_th, 2000);
                    }
                    repaint();
                }
            });
        };

        prevBtn_.onClick = [this] { musicPlayer_.prevTrack(); };

        playPauseBtn_.setButtonText("เล่น"_th);
        playPauseBtn_.setIcon(icons::Icon::Play);
        playPauseBtn_.onClick = [this] {
            if (musicPlayer_.isPlaying()) {
                musicPlayer_.pause();
                playPauseBtn_.setButtonText("เล่น"_th);
            } else {
                musicPlayer_.play();
                playPauseBtn_.setButtonText("หยุดชั่วคราว"_th);
            }
        };

        stopBtn_.setButtonText("หยุด"_th);
        stopBtn_.onClick = [this] {
            musicPlayer_.stop();
            playPauseBtn_.setButtonText("เล่น"_th);
        };

        nextBtn_.onClick = [this] { musicPlayer_.nextTrack(); };

        startStopRoomBtn_.setButtonText("เปิดห้องร้องเพลง"_th);
        startStopRoomBtn_.onClick = [this] {
            if (roomHost_.isRoomRunning()) {
                roomHost_.stopRoom();
                startStopRoomBtn_.setButtonText("เปิดห้องร้องเพลง"_th);
                startStopRoomBtn_.setActive(false);
                overlay_.toast("ปิดห้องคาราโอเกะเรียบร้อย"_th, 1500);
            } else {
                bool ok = roomHost_.startRoom("Karaoke Room");
                if (ok) {
                    startStopRoomBtn_.setButtonText("ปิดห้อง"_th);
                    startStopRoomBtn_.setActive(true);
                    overlay_.toast("เปิดห้องเรียบร้อย พร้อมร้องเพลงและส่งเสียงไป OBS"_th, 2500);
                } else {
                    overlay_.toast("ไม่สามารถเปิดห้องได้ (พอร์ตอาจถูกใช้งานอยู่)"_th, 2500);
                }
            }
            repaint();
        };

        copyFriendLinkBtn_.setButtonText("คัดลอกลิงก์เพื่อน"_th);
        copyFriendLinkBtn_.setIcon(icons::Icon::Link);
        copyFriendLinkBtn_.onClick = [this] {
            if (!roomHost_.isRoomRunning()) {
                overlay_.toast("กรุณากด 'เปิดห้อง' ก่อนคัดลอกลิงก์"_th, 1800);
                return;
            }
            const auto url = roomHost_.friendUrl(0);
            if (url.isNotEmpty()) {
                juce::SystemClipboard::copyTextToClipboard(url);
                overlay_.toast("คัดลอกลิงก์เพื่อนแล้ว (ส่งให้เพื่อนเข้ามาร้องด้วยกัน)"_th, 2500);
            } else {
                overlay_.toast("กำลังสร้างลิงก์ กรุณารอสักครู่..."_th, 1500);
            }
        };

        copyListenLinkBtn_.setButtonText("คัดลอกลิงก์คนฟัง"_th);
        copyListenLinkBtn_.setIcon(icons::Icon::Headphones);
        copyListenLinkBtn_.onClick = [this] {
            if (!roomHost_.isRoomRunning()) {
                overlay_.toast("กรุณากด 'เปิดห้อง' ก่อนคัดลอกลิงก์"_th, 1800);
                return;
            }
            const auto url = roomHost_.listenUrl();
            if (url.isNotEmpty()) {
                juce::SystemClipboard::copyTextToClipboard(url);
                overlay_.toast("คัดลอกลิงก์คนฟังแล้ว (ส่งให้คนดูฟังสดผ่านเบราว์เซอร์)"_th, 2500);
            } else {
                overlay_.toast("กำลังสร้างลิงก์ กรุณารอสักครู่..."_th, 1500);
            }
        };

        recordRoomBtn_.setButtonText("อัดเสียงห้อง WAV"_th);
        recordRoomBtn_.setIcon(icons::Icon::Record);
        recordRoomBtn_.onClick = [this] {
            if (roomHost_.isRecording()) {
                roomHost_.stopRecording();
                recordRoomBtn_.setButtonText("อัดเสียงห้อง WAV"_th);
                recordRoomBtn_.setStyle(GhostButton::Style::Ghost);
                overlay_.toast("หยุดบันทึกเสียงและบันทึกไฟล์เรียบร้อย"_th, 2000);
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
                    recordRoomBtn_.setButtonText("● กำลังอัดเสียง..."_th);
                    recordRoomBtn_.setStyle(GhostButton::Style::DangerSolid);
                    overlay_.toast("เริ่มบันทึกเสียงห้องลงไฟล์: "_th + file.getFileName(), 2000);
                } else {
                    overlay_.toast("ไม่สามารถสร้างไฟล์สำหรับบันทึกเสียงได้"_th, 2000);
                }
            }
            repaint();
        };

        // 8 Friend Slot controls
        for (int i = 0; i < 8; ++i) {
            auto& c = friendSlotControls_[i];
            c.meter = std::make_unique<MeterBar>(4.0f, true);
            c.muteBtn = std::make_unique<GhostButton>("ปิดเสียง"_th, GhostButton::Style::Ghost);
            c.muteBtn->setSmall(true);
            c.muteBtn->onClick = [this, i] {
                auto info = roomHost_.slotInfo(i);
                bool newMuted = !info.isMuted;
                roomHost_.setSlotMuted(i, newMuted);
                friendSlotControls_[i].muteBtn->setButtonText(newMuted ? "เปิดเสียง"_th : "ปิดเสียง"_th);
                overlay_.toast(newMuted ? ("ปิดเสียง "_th + info.name + " แล้ว"_th) : ("เปิดเสียง "_th + info.name + " แล้ว"_th), 1500);
            };

            c.kickBtn = std::make_unique<GhostButton>("เตะ"_th, GhostButton::Style::Danger);
            c.kickBtn->setSmall(true);
            c.kickBtn->onClick = [this, i] {
                auto info = roomHost_.slotInfo(i);
                roomHost_.kickSlot(i);
                overlay_.toast("เตะ "_th + info.name + " ออกจากห้องแล้ว"_th, 1800);
                repaint();
            };

            c.copyBtn = std::make_unique<IconButton>(icons::Icon::Link, "คัดลอกลิงก์สำหรับเพื่อนคนนี้"_th);
            c.copyBtn->onClick = [this, i] {
                juce::String url = roomHost_.friendUrl(i);
                if (url.isNotEmpty()) {
                    juce::SystemClipboard::copyTextToClipboard(url);
                    overlay_.toast("คัดลอกลิงก์ของ "_th + roomHost_.slotInfo(i).name + " แล้ว"_th, 2000);
                } else {
                    overlay_.toast("กรุณาเปิดห้องก่อนคัดลอกลิงก์"_th, 1500);
                }
            };
        }

        // ==========================================
        // Common Audio Controls (Mic & Sliders)
        // ==========================================
        muteBtn_.onClick = [this] {
            const bool m = !vocalChain_.isMuted();
            vocalChain_.setMuted(m);
            setMicMuted(m);
            overlay_.toast(m ? "ปิดไมค์แล้ว (Muted)"_th : "เปิดไมค์แล้ว (Unmuted)"_th, 1500);
        };

        vocalSlider_.setRange(-30.0, 6.0, 0.5);
        vocalSlider_.setValue(0.0, juce::dontSendNotification);
        vocalSlider_.onValueChange = [this] {
            vocalGainLinear_ = std::pow(10.0f, float(vocalSlider_.getValue()) * 0.05f);
            updateValueTexts();
        };

        musicSlider_.setRange(-30.0, 6.0, 0.5);
        musicSlider_.setValue(0.0, juce::dontSendNotification);
        musicSlider_.onValueChange = [this] {
            musicGainLinear_ = std::pow(10.0f, float(musicSlider_.getValue()) * 0.05f);
            updateValueTexts();
        };

        reverbSlider_.setRange(0.0, 100.0, 1.0);
        reverbSlider_.setValue(25.0, juce::dontSendNotification);
        reverbSlider_.onValueChange = [this] {
            vocalChain_.setReverbLevel(float(reverbSlider_.getValue()) / 100.0f);
            updateValueTexts();
        };

        echoSlider_.setRange(0.0, 100.0, 1.0);
        echoSlider_.setValue(20.0, juce::dontSendNotification);
        echoSlider_.onValueChange = [this] {
            vocalChain_.setEchoLevel(float(echoSlider_.getValue()) / 100.0f);
            updateValueTexts();
        };

        sendWetRow_.onClick = [this] {
            const bool wet = !sendWetRow_.isOn();
            sendWetRow_.setOn(wet, true);
            vocalChain_.setSendWet(wet);
            overlay_.toast(wet ? "ส่งเสียง Wet (มี Reverb/Echo ไปให้เพื่อน)"_th : "ส่งเสียง Dry (เสียงคลีน ให้ห้องใส่เอฟเฟกต์)"_th, 2000);
        };

        hearSelfRow_.setOn(true, false);
        hearSelfRow_.setTooltip(tr(Str::KaraokeHearSelfTip));
        hearSelfRow_.onClick = [this] {
            const bool on = !hearSelfRow_.isOn();
            hearSelfRow_.setOn(on, true);
            hearSelf_.store(on, std::memory_order_relaxed);
        };

        othersHearRow_.setOn(true, false);
        othersHearRow_.setTooltip(tr(Str::KaraokeOthersHearTip));
        othersHearRow_.onClick = [this] {
            const bool on = !othersHearRow_.isOn();
            othersHearRow_.setOn(on, true);
            othersHear_.store(on, std::memory_order_relaxed);
        };

        for (auto* v : { &vocalValue_, &musicValue_, &reverbValue_, &echoValue_ })
            v->setJustification(juce::Justification::centredRight);
        updateValueTexts();

        // ==========================================
        // Cards: two scrolled columns under the header
        // ==========================================
        leftBody_.painter = [this](juce::Graphics& g) { paintLeftBody(g); };
        rightBody_.painter = [this](juce::Graphics& g) { paintRightBody(g); };
        leftScroll_.setContent(leftBody_);
        rightScroll_.setContent(rightBody_);
        addAndMakeVisible(leftScroll_);
        addAndMakeVisible(rightScroll_);

        for (juce::Component* c : std::initializer_list<juce::Component*> {
                 &linkInput_, &joinBtn_, &sendWetRow_, &startStopRoomBtn_, &recordRoomBtn_, &copyFriendLinkBtn_, &copyListenLinkBtn_ })
            leftBody_.addChildComponent(c);
        for (auto& c : friendSlotControls_)
            for (juce::Component* k : std::initializer_list<juce::Component*> { c.meter.get(), c.muteBtn.get(), c.kickBtn.get(), c.copyBtn.get() })
                leftBody_.addChildComponent(k);
        for (juce::Component* c : std::initializer_list<juce::Component*> {
                 &muteBtn_, &micMeter_, &speakerRow_, &preflightBtn_, &openFileBtn_, &captureAppBtn_, &prevBtn_, &playPauseBtn_, &stopBtn_, &nextBtn_ })
            rightBody_.addChildComponent(c);

        addChildComponent(updateBanner_);
        updateBanner_.onDismiss = [this] {
            resized();
            repaint();
        };

        updateChecker_.checkForUpdates([this](const ReleaseInfo& info) {
            if (info.hasUpdate) {
                updateBanner_.showUpdate(info);
                overlay_.toast("พบ HEARASIDE รุ่นใหม่ "_th + info.latestVersion + " (คลิกที่แถบด้านบนเพื่อดาวน์โหลด)"_th, 3500);
                resized();
                repaint();
            }
        });

        addAndMakeVisible(overlay_);
        applyMode();
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

    // Shows the widgets of the mode; "your voice" lives in the left card (Join) or the right card (Host).
    void applyMode() {
        const bool isSinger = (appMode_ == AppMode::Singer);

        for (juce::Component* c : std::initializer_list<juce::Component*> { &linkInput_, &joinBtn_, &sendWetRow_, &speakerRow_, &preflightBtn_ })
            c->setVisible(isSinger);
        for (juce::Component* c : std::initializer_list<juce::Component*> {
                 &openFileBtn_, &captureAppBtn_, &prevBtn_, &playPauseBtn_, &stopBtn_, &nextBtn_,
                 &startStopRoomBtn_, &copyFriendLinkBtn_, &copyListenLinkBtn_, &recordRoomBtn_ })
            c->setVisible(!isSinger);
        for (auto& c : friendSlotControls_)
            for (juce::Component* k : std::initializer_list<juce::Component*> { c.meter.get(), c.muteBtn.get(), c.kickBtn.get(), c.copyBtn.get() })
                k->setVisible(!isSinger);

        auto& voiceHome = isSinger ? leftBody_ : rightBody_;
        for (juce::Component* c : std::initializer_list<juce::Component*> {
                 &hearSelfRow_, &othersHearRow_, &vocalField_, &musicField_, &reverbField_, &echoField_ })
            voiceHome.addAndMakeVisible(c);
        muteBtn_.setVisible(true);
        micMeter_.setVisible(true);

        hearSelfRow_.setTexts(tr(Str::KaraokeHearSelf), tr(Str::KaraokeHearSelfCap));
        othersHearRow_.setTexts(tr(Str::KaraokeOthersHear), tr(isSinger ? Str::KaraokeOthersHearCap : Str::KaraokeHostOthersHearCap));
        speakerRow_.setTexts(tr(Str::KaraokeSpeakerMode), tr(Str::KaraokeSpeakerModeCap));
        sendWetRow_.setTexts(tr(Str::KaraokeSendWet), tr(Str::KaraokeSendWetCap));
        vocalField_.setTexts(tr(Str::KaraokeVoice), {});
        musicField_.setTexts(tr(Str::KaraokeMusic), {});
        reverbField_.setTexts(tr(Str::KaraokeReverb), {});
        echoField_.setTexts(tr(Str::KaraokeEcho), {});
        leftScroll_.setViewPosition(0, 0);
        rightScroll_.setViewPosition(0, 0);
    }

    bool keyPressed(const juce::KeyPress& key) override {
        if (key.isKeyCode(juce::KeyPress::spaceKey) || key.getKeyCode() == 'M' || key.getKeyCode() == 'm') {
            const bool m = !vocalChain_.isMuted();
            vocalChain_.setMuted(m);
            setMicMuted(m);
            overlay_.toast(m ? "ปิดไมค์แล้ว (Muted)"_th : "เปิดไมค์แล้ว (Unmuted)"_th, 1500);
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
        overlay_.toast("ได้รับลิงก์ห้องแล้ว: กด 'เข้าห้อง' เพื่อเริ่ม"_th, 2500);
    }

    void onJoinToggleClicked() {
        if (joinClient_.isLive() || joinClient_.state() == ConnectionState::Connecting) {
            joinClient_.disconnect();
            joinBtn_.setButtonText("เข้าห้อง"_th);
            joinBtn_.setActive(false);
            overlay_.toast("ออกจากห้องเรียบร้อย"_th, 1500);
            repaint();
            return;
        }

        const juce::String raw = linkInput_.getText().trim();
        if (raw.isEmpty()) {
            overlay_.toast("กรุณาวางลิงก์ส่งเสียงที่ได้รับจากเพื่อน"_th, 2000);
            return;
        }

        joinClient_.connect(raw);
        joinBtn_.setButtonText("ยกเลิก"_th);
        joinBtn_.setActive(true);
        overlay_.toast("กำลังเชื่อมต่อห้อง..."_th, 2000);
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
        const bool hearSelf = hearSelf_.load(std::memory_order_relaxed);
        const bool othersHear = othersHear_.load(std::memory_order_relaxed);

        if (appMode_ == AppMode::Singer) {
            // ================= Singer Mode =================
            const bool live = joinClient_.isLive();
            if (live) {
                if (!othersHear) std::fill_n(scratchNetMic_.data(), n, 0.0f);   // keep sending: silence keeps the line and the timing
                joinClient_.pushUpstream(scratchNetMic_.data(), n, timelineCounter_);
                joinClient_.pullDownstream(scratchDownL_.data(), scratchDownR_.data(), n);
            } else {
                std::fill_n(scratchDownL_.data(), n, 0.0f);
                std::fill_n(scratchDownR_.data(), n, 0.0f);
            }
            timelineCounter_ += n;

            preflight_.processAudio(inMic, scratchDownL_.data(), scratchDownR_.data(), n);

            const float selfG = hearSelf ? vocG : 0.0f;
            if (numOutputChannels >= 2 && outputChannelData != nullptr) {
                float* outL = outputChannelData[0];
                float* outR = outputChannelData[1];
                for (int i = 0; i < n; ++i) {
                    float mixL = (scratchVocalL_[i] * selfG) + (scratchDownL_[i] * musG);
                    float mixR = (scratchVocalR_[i] * selfG) + (scratchDownR_[i] * musG);

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
                    float mix = (0.5f * (scratchVocalL_[i] + scratchVocalR_[i]) * selfG) +
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
                                  outL, outR, n, hearSelf, othersHear);
        }
    }

    // Timer (30 Hz)
    void timerCallback() override {
        const float p = micPeak_.load(std::memory_order_relaxed);
        smoothedPeak_ = std::max(p, smoothedPeak_ * 0.82f);
        micMeter_.setLevel(hearaside::meterPosition(smoothedPeak_));

        if (appMode_ == AppMode::Singer) {
            if (joinClient_.isLive()) {
                joinBtn_.setButtonText("ออกจากห้อง"_th);
                joinBtn_.setActive(true);
            } else if (joinClient_.state() == ConnectionState::Connecting) {
                joinBtn_.setButtonText("กำลังเชื่อมต่อ..."_th);
                joinBtn_.setActive(true);
            } else {
                joinBtn_.setButtonText("เข้าห้อง"_th);
                joinBtn_.setActive(false);
            }

            if (preflight_.isTesting()) {
                preflightBtn_.setButtonText("กำลังตรวจ..."_th);
                preflightBtn_.setActive(true);
            } else {
                preflightBtn_.setButtonText("เช็กก่อนร้อง"_th);
                preflightBtn_.setActive(false);
            }
        } else {
            // Host timer updates
            if (musicPlayer_.isPlaying()) {
                playPauseBtn_.setButtonText("หยุดชั่วคราว"_th);
            } else {
                playPauseBtn_.setButtonText("เล่น"_th);
            }

            if (roomHost_.isRecording()) {
                recordRoomBtn_.setButtonText("● อัดเสียง ("_th + juce::String(roomHost_.recordedSeconds(), 1) + "s)");
            } else {
                recordRoomBtn_.setButtonText("อัดเสียงห้อง WAV"_th);
            }

            for (int i = 0; i < 8; ++i) {
                auto info = roomHost_.slotInfo(i);
                friendSlotControls_[i].meter->setLevel(hearaside::meterPosition(info.peak));
            }
        }

        const auto st = headerStatus();
        if (st.text != chipText_) {   // the chip is as wide as its text
            chipText_ = st.text;
            statusChip_.set(st.text, st.dot);
            resized();
        }
        statusChip_.set(st.text, st.dot);

        // the bodies' heights follow what is shown (live badges, check results)
        const auto pf = preflight_.result();
        const auto key = juce::String(int(appMode_)) + "|" + juce::String(int(joinClient_.state() == ConnectionState::Live))
                       + "|" + juce::String(int(pf.leakStatus)) + "|" + pf.advice;
        if (key != layoutKey_) {
            layoutKey_ = key;
            layoutBodies();
        }

        updateDeviceInfo();
        repaint();
    }

    void updateDeviceInfo() {
        latency_ = AudioSettings::getLatencyInfo(deviceManager_);
        if (latency_.sampleRate > 0) {
            joinClient_.setLocalLatencyMs(latency_.totalMs);
        }
    }

    void changeListenerCallback(juce::ChangeBroadcaster*) override {
        backdrop_.invalidate();
        resized();
        repaint();
    }

    const theme::Palette& pal() const {
        return settings_->isDark() ? theme::dark() : theme::light();
    }

    // ---- layout ---------------------------------------------------------------------------------
    static constexpr float kPad = 16.0f, kGap = 12.0f, kHeaderH = 58.0f;

    void resized() override {
        overlay_.setBounds(getLocalBounds());
        const auto b = getLocalBounds().toFloat();
        auto r = b.reduced(kPad);

        header_ = r.removeFromTop(kHeaderH);
        r.removeFromTop(kGap);
        if (updateBanner_.isVisible()) {
            updateBanner_.setBounds(r.removeFromTop(42.0f).toNearestInt());
            r.removeFromTop(kGap);
        }
        const float rightW = juce::jlimit(280.0f, 340.0f, r.getWidth() * 0.4f);
        rightCard_ = r.removeFromRight(rightW);
        r.removeFromRight(kGap);
        leftCard_ = r;

        backdrop_.setBlobs({ { { -80.0f, -120.0f, 520.0f, 300.0f }, true },
                             { { b.getWidth() - 300.0f, b.getHeight() - 280.0f, 420.0f, 340.0f }, false } });
        backdrop_.setCards({ { header_, theme::radius::header }, { leftCard_, theme::radius::card }, { rightCard_, theme::radius::card } });

        // header: wordmark, mode tabs, status | theme, settings
        auto h = header_.withTrimmedLeft(20.0f).withTrimmedRight(11.0f);
        settingsBtn_.setBounds(h.removeFromRight(36.0f).withSizeKeepingCentre(36.0f, 36.0f).toNearestInt());
        h.removeFromRight(6.0f);
        themeToggleBtn_.setBounds(h.removeFromRight(36.0f).withSizeKeepingCentre(36.0f, 36.0f).toNearestInt());
        h.removeFromRight(12.0f);
        wordmark_ = h.removeFromLeft(130.0f).withSizeKeepingCentre(130.0f, 30.0f);
        h.removeFromLeft(14.0f);
        const float segW = float(modeSwitch_.idealWidth());
        modeSwitch_.setBounds(h.removeFromLeft(segW).withSizeKeepingCentre(segW, 36.0f).toNearestInt());
        h.removeFromLeft(10.0f);
        const float chipW = juce::jmin(h.getWidth(), float(statusChip_.idealWidth()) + 4.0f);
        statusChip_.setBounds(h.removeFromRight(chipW).withSizeKeepingCentre(chipW, 34.0f).toNearestInt());
        statusChip_.setVisible(chipW >= 90.0f);

        // cards: title + caption painted, the rest scrolls
        auto lc = leftCard_.reduced(20.0f, 18.0f);
        leftTitle_ = lc.removeFromTop(20.0f + 3.0f + 17.0f);
        lc.removeFromTop(14.0f);
        leftScroll_.setBounds(lc.withTrimmedRight(-14.0f).toNearestInt());
        auto rc = rightCard_.reduced(20.0f, 18.0f);
        rightTitle_ = rc.removeFromTop(20.0f + 3.0f + 17.0f);
        rc.removeFromTop(14.0f);
        rightScroll_.setBounds(rc.withTrimmedRight(-14.0f).toNearestInt());
        const auto fade = pal().paper.overlaidWith(pal().glass);
        leftScroll_.setFadeColour(fade);
        rightScroll_.setFadeColour(fade);

        layoutBodies();
    }

    void layoutBodies() {
        const int lw = leftScroll_.contentWidth(), rw = rightScroll_.contentWidth();
        if (lw <= 0 || rw <= 0) return;
        if (appMode_ == AppMode::Singer) {
            leftBody_.setSize(lw, layoutSingerLeft(lw));
            rightBody_.setSize(rw, layoutSingerRight(rw));
        } else {
            leftBody_.setSize(lw, layoutHostLeft(lw));
            rightBody_.setSize(rw, layoutHostRight(rw));
        }
    }

    // Join: link + Join, live badges, your voice (hear rows, voice / music), effects
    int layoutSingerLeft(int width) {
        const float w = float(width);
        float y = 2.0f;
        const int joinW = 128;
        linkInput_.setBounds(0, int(y), width - joinW - 8, 40);
        joinBtn_.setBounds(width - joinW, int(y), joinW, 40);
        y += 40.0f;
        liveRow_ = {};
        if (joinClient_.state() == ConnectionState::Live) {
            y += 10.0f;
            liveRow_ = { 0.0f, y, w, 22.0f };
            y += 22.0f;
        }
        y += 20.0f;
        divider1_ = { 0.0f, y, w, 1.0f };
        y += 18.0f;
        voiceTitle_ = { 0.0f, y, w, 20.0f };
        y += 30.0f;
        y = placeVoice(width, y);
        y += 8.0f;
        divider2_ = { 0.0f, y, w, 1.0f };
        y += 18.0f;
        fxTitle_ = { 0.0f, y, w, 20.0f };
        y += 30.0f;
        reverbField_.setBounds(0, int(y), width, reverbField_.idealHeight(width));
        y += float(reverbField_.getHeight()) + 12.0f;
        echoField_.setBounds(0, int(y), width, echoField_.idealHeight(width));
        y += float(echoField_.getHeight()) + 16.0f;
        sendWetRow_.setBounds(0, int(y), width, int(theme::layout::rowH));
        y += theme::layout::rowH + 4.0f;
        return juce::roundToInt(y);
    }

    // the two hear rows and the voice / music levels, from y; returns the new y
    float placeVoice(int width, float y) {
        hearSelfRow_.setBounds(0, int(y), width, int(theme::layout::rowH));
        y += theme::layout::rowH + theme::space::rowGap;
        othersHearRow_.setBounds(0, int(y), width, int(theme::layout::rowH));
        y += theme::layout::rowH + 18.0f;
        vocalField_.setBounds(0, int(y), width, vocalField_.idealHeight(width));
        y += float(vocalField_.getHeight()) + 12.0f;
        musicField_.setBounds(0, int(y), width, musicField_.idealHeight(width));
        y += float(musicField_.getHeight()) + 10.0f;
        return y;
    }

    // Ready: mic + Mute, meter, speaker mode, device, check, results, connection when live
    int layoutSingerRight(int width) {
        const float w = float(width);
        float y = 2.0f;
        placeMicRow(width, y);
        y += 32.0f + 10.0f;
        micMeter_.setBounds(0, int(y), width, 6);
        y += 6.0f + 18.0f;
        speakerRow_.setBounds(0, int(y), width, int(theme::layout::rowH));
        y += theme::layout::rowH + 14.0f;
        deviceBox_ = { 0.0f, y, w, 92.0f };
        y += 92.0f + 14.0f;
        preflightBtn_.setBounds(0, int(y), width, 44);
        y += 44.0f + 14.0f;
        const auto pf = preflight_.result();
        const auto cap = uiFont(12.0f);
        if (pf.leakStatus == LeakStatus::Untested) {
            resultBox_ = { 0.0f, y, w, wrappedHeight(cap, checkHint(), w, 4.0f) };
        } else {
            resultBox_ = { 0.0f, y, w, 20.0f + 4.0f + wrappedHeight(cap, pf.advice, w, 4.0f) };
        }
        y += resultBox_.getHeight() + 16.0f;
        connBox_ = {};
        if (joinClient_.state() == ConnectionState::Live) {
            connBox_ = { 0.0f, y, w, 12.0f + 18.0f + 4.0f * 22.0f + 10.0f };
            y += connBox_.getHeight() + 8.0f;
        }
        return juce::roundToInt(y);
    }

    void placeMicRow(int width, float y) {
        const int muteW = juce::jmax(muteBtn_.idealWidth(), 116);
        micLabel_ = { 0.0f, y, float(width - muteW - 8), 32.0f };
        muteBtn_.setBounds(width - muteW, int(y), muteW, 32);
    }

    // Room: open / record, copy links, 8 friend rows
    int layoutHostLeft(int width) {
        const float w = float(width);
        float y = 2.0f;
        const int half = (width - 8) / 2;
        startStopRoomBtn_.setBounds(0, int(y), half, 40);
        recordRoomBtn_.setBounds(half + 8, int(y), width - half - 8, 40);
        y += 48.0f;
        copyFriendLinkBtn_.setBounds(0, int(y), half, 36);
        copyListenLinkBtn_.setBounds(half + 8, int(y), width - half - 8, 36);
        y += 36.0f + 20.0f;
        divider1_ = { 0.0f, y, w, 1.0f };
        y += 18.0f;
        friendsTitle_ = { 0.0f, y, w, 20.0f };
        y += 30.0f;
        for (int i = 0; i < 8; ++i) {
            const auto row = juce::Rectangle<float>(0.0f, y, w, 50.0f);
            friendRows_[size_t(i)] = row;
            auto& c = friendSlotControls_[i];
            auto r = row.reduced(10.0f, 0.0f).toNearestInt();
            c.copyBtn->setBounds(r.removeFromRight(32).withSizeKeepingCentre(32, 32));
            r.removeFromRight(6);
            const int kw = c.kickBtn->idealWidth();
            c.kickBtn->setBounds(r.removeFromRight(kw).withSizeKeepingCentre(kw, 30));
            r.removeFromRight(6);
            const int mw = juce::jmax(c.muteBtn->idealWidth(), 72);
            c.muteBtn->setBounds(r.removeFromRight(mw).withSizeKeepingCentre(mw, 30));
            r.removeFromRight(14);
            r.removeFromLeft(150);
            c.meter->setBounds(r.withSizeKeepingCentre(r.getWidth(), 4));
            c.meter->setVisible(r.getWidth() >= 30 && appMode_ == AppMode::Host);
            y += 50.0f + 6.0f;
        }
        return juce::roundToInt(y);
    }

    // Music + your voice: now playing, file / app, transport, mic + Mute, meter, hear rows, levels
    int layoutHostRight(int width) {
        const float w = float(width);
        float y = 2.0f;
        songLine_ = { 0.0f, y, w, 22.0f };
        y += 30.0f;
        openFileBtn_.setBounds(0, int(y), width, 34);
        y += 34.0f + 8.0f;
        captureAppBtn_.setBounds(0, int(y), width, 34);
        y += 34.0f + 8.0f;
        prevBtn_.setBounds(0, int(y), 34, 34);
        nextBtn_.setBounds(width - 34, int(y), 34, 34);
        const int stopW = 72;
        stopBtn_.setBounds(width - 34 - 6 - stopW, int(y), stopW, 34);
        playPauseBtn_.setBounds(40, int(y), width - 34 - 6 - stopW - 6 - 40, 34);
        y += 34.0f + 20.0f;
        divider2_ = { 0.0f, y, w, 1.0f };
        y += 18.0f;
        placeMicRow(width, y);
        y += 32.0f + 10.0f;
        micMeter_.setBounds(0, int(y), width, 6);
        y += 6.0f + 18.0f;
        y = placeVoice(width, y);
        reverbField_.setBounds(0, int(y), width, reverbField_.idealHeight(width));
        y += float(reverbField_.getHeight()) + 12.0f;
        echoField_.setBounds(0, int(y), width, echoField_.idealHeight(width));
        y += float(echoField_.getHeight()) + 6.0f;
        return juce::roundToInt(y);
    }

    // ---- painting -------------------------------------------------------------------------------
    struct Status { juce::String text; Dot dot; };

    Status headerStatus() const {
        if (appMode_ == AppMode::Singer) {
            const auto cState = joinClient_.state();
            const auto room = joinClient_.roomInfo();
            if (cState == ConnectionState::Live) {
                juce::String t = room.name.isNotEmpty() ? ("ในห้อง: "_th + room.name) : "ในห้อง (Live)"_th;
                if (room.delayMs >= 0.0f) t += " (~" + juce::String(juce::roundToInt(room.delayMs)) + " ms)";
                return { t, Dot::Ok };
            }
            if (cState == ConnectionState::Connecting || cState == ConnectionState::Resolving || cState == ConnectionState::Handshaking)
                return { "กำลังต่อเข้าห้อง..."_th, Dot::Warn };
            if (cState == ConnectionState::Reconnecting)
                return { "กำลังเชื่อมต่อใหม่..."_th, Dot::Warn };
            return { "พร้อมเข้าห้อง"_th, Dot::Muted };
        }
        if (roomHost_.isRoomRunning())
            return { "เปิดห้องแล้ว (พอร์ต "_th + juce::String(roomHost_.serverPort()) + ")", Dot::Ok };
        return { "ห้องยังไม่เปิด"_th, Dot::Muted };
    }

    static juce::String checkHint() { return "กด 'เช็กก่อนร้อง' เพื่อตรวจไมค์ เสียงลำโพงสะท้อน และสัญญาณเน็ต"_th; }

    void paintCardTitle(juce::Graphics& g, juce::Rectangle<float> r, const juce::String& title, const juce::String& caption) {
        const auto& p = pal();
        g.setColour(p.ink);
        g.setFont(uiFont(theme::type::h2, Weight::SemiBold));
        g.drawText(title, r.removeFromTop(20.0f), juce::Justification::centredLeft, true);
        r.removeFromTop(3.0f);
        g.setColour(p.graphite);
        g.setFont(uiFont(12.5f));
        g.drawText(ellipsize(uiFont(12.5f), caption, r.getWidth()), r, juce::Justification::centredLeft, false);
    }

    void paintSection(juce::Graphics& g, juce::Rectangle<float> r, const juce::String& title, const juce::String& right = {}) {
        const auto& p = pal();
        if (right.isNotEmpty()) {
            g.setColour(p.graphite);
            g.setFont(uiFont(12.0f, Weight::Medium));
            g.drawText(right, r, juce::Justification::centredRight, false);
        }
        g.setColour(p.ink);
        g.setFont(uiFont(14.0f, Weight::SemiBold));
        g.drawText(title, r, juce::Justification::centredLeft, true);
    }

    void paint(juce::Graphics& g) override {
        const auto& p = pal();
        backdrop_.paint(g, getLocalBounds(), p, settings_->glassAlpha(), settings_->isDark());
        drawWordmark(g, wordmark_, "KARAOKE", 14.0f, p.ink);
        if (appMode_ == AppMode::Singer) {
            paintCardTitle(g, leftTitle_, tr(Str::KaraokeJoinTitle), tr(Str::KaraokeJoinCap));
            paintCardTitle(g, rightTitle_, tr(Str::KaraokeCheckTitle), tr(Str::KaraokeCheckCap));
        } else {
            paintCardTitle(g, leftTitle_, tr(Str::KaraokeYourRoom), tr(Str::KaraokeRoomCap));
            paintCardTitle(g, rightTitle_, tr(Str::KaraokeHostMixTitle), tr(Str::KaraokeHostMixCap));
        }
    }

    void paintLeftBody(juce::Graphics& g) {
        const auto& p = pal();
        g.setColour(p.hairline2);
        g.fillRect(divider1_);
        if (appMode_ == AppMode::Singer) {
            if (!liveRow_.isEmpty()) {
                const auto room = joinClient_.roomInfo();
                float x = liveRow_.getX();
                auto badge = [&](const juce::String& t, BadgeStyle s) {
                    const float bw = badgeWidth(t, s);
                    drawBadge(g, { x, liveRow_.getY(), bw, liveRow_.getHeight() }, t, s, p);
                    x += bw + 6.0f;
                };
                badge(room.isUdp ? "UDP" : "WS", room.isUdp ? BadgeStyle::Ok : BadgeStyle::Chip);
                badge(room.codec.isNotEmpty() ? room.codec.toUpperCase() : "OPUS", BadgeStyle::Chip);
                if (room.delayMs >= 0.0f) badge(juce::String(juce::roundToInt(room.delayMs)) + " ms", BadgeStyle::Chip);
            }
            g.setColour(p.hairline2);
            g.fillRect(divider2_);
            paintSection(g, voiceTitle_, tr(Str::KaraokeYourVoice));
            paintSection(g, fxTitle_, tr(Str::KaraokeEffects));
            return;
        }
        int live = 0;
        for (int i = 0; i < 8; ++i) live += roomHost_.slotInfo(i).isLive ? 1 : 0;
        paintSection(g, friendsTitle_, tr(Str::KaraokeFriends), juce::String(live) + " / 8");
        for (int i = 0; i < 8; ++i) {
            const auto row = friendRows_[size_t(i)];
            const auto info = roomHost_.slotInfo(i);
            drawInset(g, row, theme::radius::row, p, info.isLive);
            drawStatusDot(g, { row.getX() + 18.0f, row.getCentreY() }, info.isLive ? Dot::Ok : Dot::Muted, p);
            const auto nameArea = juce::Rectangle<float>(row.getX() + 30.0f, row.getY(), 116.0f, row.getHeight());
            g.setColour(info.isLive ? p.ink : p.muted);
            g.setFont(uiFont(13.0f, info.isLive ? Weight::SemiBold : Weight::Medium));
            const juce::String name = info.name.isNotEmpty() ? info.name : tr(Str::KaraokeSlot) + " " + juce::String(i + 1);
            if (info.isLive && info.delayMs >= 0.0f) {
                const juce::String d = juce::String(juce::roundToInt(info.delayMs)) + " ms";
                const float bw = badgeWidth(d);
                g.drawText(ellipsize(uiFont(13.0f, Weight::SemiBold), name, nameArea.getWidth() - bw - 8.0f), nameArea, juce::Justification::centredLeft, false);
                const float nw = juce::jmin(textWidth(uiFont(13.0f, Weight::SemiBold), name), nameArea.getWidth() - bw - 8.0f);
                drawBadge(g, { nameArea.getX() + nw + 6.0f, row.getCentreY() - 10.0f, bw, 20.0f }, d, BadgeStyle::Chip, p);
            } else {
                g.drawText(ellipsize(uiFont(13.0f, Weight::Medium), name, nameArea.getWidth()), nameArea, juce::Justification::centredLeft, false);
            }
        }
    }

    void paintRightBody(juce::Graphics& g) {
        const auto& p = pal();
        g.setColour(p.ink);
        g.setFont(uiFont(13.0f, Weight::Medium));
        g.drawText(appMode_ == AppMode::Singer ? tr(Str::KaraokeMic) : tr(Str::KaraokeYourVoice), micLabel_, juce::Justification::centredLeft, true);

        if (appMode_ == AppMode::Host) {
            juce::String song = "ยังไม่ได้เลือกเพลง (เล่นไฟล์ WAV/MP3 หรือดึงจากเบราว์เซอร์)"_th;
            if (musicPlayer_.sourceMode() == MusicSourceMode::AudioFile) {
                const int idx = musicPlayer_.currentTrackIndex();
                if (idx >= 0 && idx < musicPlayer_.trackCount()) {
                    const auto trk = musicPlayer_.trackAt(idx);
                    const int curM = int(musicPlayer_.currentPositionSec()) / 60;
                    const int curS = int(musicPlayer_.currentPositionSec()) % 60;
                    const int lenM = int(trk.durationSec) / 60;
                    const int lenS = int(trk.durationSec) % 60;
                    song = trk.title + "  " + juce::String::formatted("%02d:%02d / %02d:%02d", curM, curS, lenM, lenS);
                }
            } else if (musicPlayer_.sourceMode() == MusicSourceMode::AppCapture) {
                song = "ดึงเสียงจากแอป: "_th + musicPlayer_.capturedAppName();
            }
            const auto f = uiFont(13.0f, Weight::SemiBold);
            g.setColour(musicPlayer_.isPlaying() ? p.ink : p.ink2);
            g.setFont(f);
            g.drawText(ellipsize(f, song, songLine_.getWidth()), songLine_, juce::Justification::centredLeft, false);
            g.setColour(p.hairline2);
            g.fillRect(divider2_);
            return;
        }

        // audio device
        drawInset(g, deviceBox_, 12.0f, p);
        {
            auto r = deviceBox_.reduced(14.0f, 12.0f);
            g.setColour(p.muted);
            g.setFont(uiFont(11.5f, Weight::Medium));
            g.drawText(tr(Str::KaraokeDevice), r.removeFromTop(16.0f), juce::Justification::centredLeft, true);
            r.removeFromTop(2.0f);
            const auto nf = uiFont(13.0f, Weight::SemiBold);
            g.setColour(p.ink);
            g.setFont(nf);
            const juce::String name = latency_.sampleRate > 0 ? latency_.deviceName : "ไม่มีอุปกรณ์เสียง (No audio device)"_th;
            g.drawText(ellipsize(nf, name, r.getWidth()), r.removeFromTop(20.0f), juce::Justification::centredLeft, false);
            if (latency_.sampleRate > 0) {
                g.setColour(p.graphite);
                g.setFont(uiFont(12.0f));
                g.drawText(latency_.driverType + juce::String(juce::CharPointer_UTF8(" \xc2\xb7 ")) + juce::String(latency_.sampleRate / 1000) + " kHz"
                           + juce::String(juce::CharPointer_UTF8(" \xc2\xb7 ")) + juce::String(latency_.bufferSize) + " smp",
                           r.removeFromTop(18.0f), juce::Justification::centredLeft, true);
                auto lat = r.removeFromTop(18.0f);
                drawStatusDot(g, { lat.getX() + 4.0f, lat.getCentreY() }, latency_.totalMs <= 30.0 ? Dot::Ok : Dot::Warn, p, 6.0f);
                g.setColour(p.ink2);
                g.setFont(uiFont(12.0f, Weight::Medium));
                g.drawText(tr(Str::KaraokeDelayHere) + " " + juce::String(latency_.totalMs, 1) + " ms", lat.withTrimmedLeft(14.0f), juce::Justification::centredLeft, true);
            }
        }

        // check result
        const auto pf = preflight_.result();
        if (pf.leakStatus == LeakStatus::Untested) {
            drawWrapped(g, checkHint(), uiFont(12.0f), p.graphite, resultBox_, 4.0f);
        } else {
            auto r = resultBox_;
            auto top = r.removeFromTop(20.0f);
            drawStatusDot(g, { top.getX() + 4.0f, top.getCentreY() }, pf.isReady ? Dot::Ok : Dot::Warn, p, 7.0f);
            g.setColour(pf.isReady ? p.ok : p.ink);
            g.setFont(uiFont(13.5f, Weight::SemiBold));
            g.drawText(pf.summary, top.withTrimmedLeft(16.0f), juce::Justification::centredLeft, true);
            r.removeFromTop(4.0f);
            drawWrapped(g, pf.advice, uiFont(12.0f), p.graphite, r, 4.0f);
        }

        // connection while live
        if (!connBox_.isEmpty()) {
            const auto room = joinClient_.roomInfo();
            g.setColour(p.hairline2);
            g.drawRoundedRectangle(connBox_.reduced(0.5f), 12.0f, 1.0f);
            auto r = connBox_.reduced(14.0f, 12.0f);
            g.setColour(p.muted);
            g.setFont(uiFont(11.5f, Weight::Medium));
            g.drawText(tr(Str::KaraokeConnection) + "  " + room.transport, r.removeFromTop(18.0f), juce::Justification::centredLeft, true);
            auto line = [&](const juce::String& k, const juce::String& v) {
                auto l = r.removeFromTop(22.0f);
                g.setColour(p.graphite);
                g.setFont(uiFont(12.0f));
                g.drawText(k, l, juce::Justification::centredLeft, false);
                g.setColour(p.ink);
                g.setFont(uiFont(12.0f, Weight::Medium));
                g.drawText(v, l, juce::Justification::centredRight, false);
            };
            auto ms = [](float v) { return v >= 0.0f ? juce::String(juce::roundToInt(v)) + " ms" : juce::String("-"); };
            line("Delay", ms(room.delayMs));
            line("RTT", ms(room.rttMs));
            line("Jitter p95", ms(room.jitterP95Ms));
            line("Buffer", ms(room.bufferMs));
        }
    }

    void setMicMuted(bool m) {
        muteBtn_.setButtonText(tr(m ? Str::KaraokeMicIsOff : Str::KaraokeMicOff));
        muteBtn_.setStyle(m ? GhostButton::Style::DangerSolid : GhostButton::Style::Ghost);
    }

    void updateValueTexts() {
        auto db = [](double v) {
            return v <= -30.0 + 1.0e-6 ? juce::String(juce::CharPointer_UTF8("\xe2\x88\x92\xe2\x88\x9e dB"))
                                      : juce::String(v, 1).replace("-", juce::String(juce::CharPointer_UTF8("\xe2\x88\x92"))) + " dB";
        };
        vocalValue_.setText(db(vocalSlider_.getValue()));
        musicValue_.setText(db(musicSlider_.getValue()));
        reverbValue_.setText(juce::String(juce::roundToInt(reverbSlider_.getValue())) + " %");
        echoValue_.setText(juce::String(juce::roundToInt(echoSlider_.getValue())) + " %");
    }

private:
    MainWindow& owner_;
    juce::AudioDeviceManager& deviceManager_;
    SharedSettings settings_;
    Overlay overlay_;
    UpdateChecker updateChecker_;
    UpdateBanner updateBanner_;
    Backdrop backdrop_;

    SegmentedControl modeSwitch_;
    AppMode appMode_ = AppMode::Singer;
    StatusChip statusChip_ { 34.0f };

    // Singer components
    TextField linkInput_;
    PrimaryButton joinBtn_;
    IconButton themeToggleBtn_;
    IconButton settingsBtn_;
    GhostButton muteBtn_ { tr(Str::KaraokeMicOff) };
    MeterBar micMeter_;
    PrimaryButton preflightBtn_;
    ToggleRow speakerRow_ { icons::Icon::SpeakerOn };

    // Host components
    GhostButton openFileBtn_{ "เลือกไฟล์เพลง"_th };
    GhostButton captureAppBtn_{ "ดึงเสียงแอป (YouTube/Spotify)"_th };
    IconButton prevBtn_;
    GhostButton playPauseBtn_{ "เล่น"_th };
    GhostButton stopBtn_{ "หยุด"_th };
    IconButton nextBtn_;
    PrimaryButton startStopRoomBtn_;
    GhostButton copyFriendLinkBtn_{ "คัดลอกลิงก์เพื่อน"_th };
    GhostButton copyListenLinkBtn_{ "คัดลอกลิงก์คนฟัง"_th };
    GhostButton recordRoomBtn_{ "อัดเสียงห้อง WAV"_th };
    std::unique_ptr<juce::FileChooser> fileChooser_;

    struct SlotRowControls {
        std::unique_ptr<MeterBar> meter;
        std::unique_ptr<GhostButton> muteBtn;
        std::unique_ptr<GhostButton> kickBtn;
        std::unique_ptr<IconButton> copyBtn;
    };
    std::array<SlotRowControls, 8> friendSlotControls_;

    // Your voice (left card when joining, right card when hosting)
    ToggleRow hearSelfRow_ { icons::Icon::Headphones };   // like "You hear" in the Track plug-in
    ToggleRow othersHearRow_ { icons::Icon::Broadcast };  // like "Viewers hear"
    ToggleRow sendWetRow_ { icons::Icon::Waveform };
    LevelSlider vocalSlider_;
    LevelSlider musicSlider_;
    LevelSlider reverbSlider_;
    LevelSlider echoSlider_;
    TextLine vocalValue_ { 12.5f, Weight::Medium, TextLine::Tone::Ink2 };
    TextLine musicValue_ { 12.5f, Weight::Medium, TextLine::Tone::Ink2 };
    TextLine reverbValue_ { 12.5f, Weight::Medium, TextLine::Tone::Ink2 };
    TextLine echoValue_ { 12.5f, Weight::Medium, TextLine::Tone::Ink2 };
    Field vocalField_ { {}, &vocalSlider_, 20, &vocalValue_, 80 };
    Field musicField_ { {}, &musicSlider_, 20, &musicValue_, 80 };
    Field reverbField_ { {}, &reverbSlider_, 20, &reverbValue_, 80 };
    Field echoField_ { {}, &echoSlider_, 20, &echoValue_, 80 };
    std::atomic<bool> hearSelf_{ true };
    std::atomic<bool> othersHear_{ true };

    // cards
    ScrollArea leftScroll_, rightScroll_;
    PaintBody leftBody_, rightBody_;

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
    LatencyInfo latency_;
    juce::String layoutKey_, chipText_;

    // painted geometry (content / body coordinates)
    juce::Rectangle<float> header_, wordmark_, leftCard_, rightCard_, leftTitle_, rightTitle_;
    juce::Rectangle<float> liveRow_, divider1_, divider2_, voiceTitle_, fxTitle_, micLabel_, deviceBox_, resultBox_, connBox_;
    juce::Rectangle<float> friendsTitle_, songLine_;
    std::array<juce::Rectangle<float>, 8> friendRows_;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR(KaraokeContent)
};

MainWindow::MainWindow()
    : DocumentWindow("HAS Karaoke",
                     theme::dark().paper,
                     DocumentWindow::allButtons) {
    setLookAndFeel(&lnf_);
    juce::LookAndFeel::setDefaultLookAndFeel(&lnf_);   // the Audio Settings window gets the same frame
    lnf_.setDark(settings_->isDark());
    setBackgroundColour(lnf_.pal().paper);
    settings_->addChangeListener(this);

    // Initialise audio device manager from saved settings or defaults
    AudioSettings::loadState(deviceManager_, settings_);

    content_ = std::make_unique<KaraokeContent>(*this, deviceManager_);
    setContentNonOwned(content_.get(), false);

    setUsingNativeTitleBar(false);
    setTitleBarHeight(KaraokeLookAndFeel::kTitleBarH);
    setResizable(true, true);
    setResizeLimits(760, 560, 1920, 1080);

    const auto saved = settings_->windowSize("karaoke");
    if (saved.x >= 760 && saved.y >= 560) {
        setSize(saved.x, saved.y);
    } else {
        setSize(860, 640);
    }

    centreWithSize(getWidth(), getHeight());
    setVisible(true);
}

MainWindow::~MainWindow() {
    AudioSettings::saveState(deviceManager_, settings_);
    settings_->removeChangeListener(this);
    clearContentComponent();
    content_.reset();
    juce::LookAndFeel::setDefaultLookAndFeel(nullptr);
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
