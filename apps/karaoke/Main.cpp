#include "MainWindow.h"

#include <juce_gui_extra/juce_gui_extra.h>

namespace hearaside::karaoke {

class KaraokeTrayIcon : public juce::SystemTrayIconComponent {
public:
    KaraokeTrayIcon(std::function<void()> onToggle, std::function<void()> onQuit)
        : onToggle_(std::move(onToggle)), onQuit_(std::move(onQuit)) {
        juce::Image img(juce::Image::ARGB, 32, 32, true);
        {
            juce::Graphics g(img);
            g.setColour(juce::Colour(0xff2d68ff));
            g.fillRoundedRectangle(2.0f, 2.0f, 28.0f, 28.0f, 7.0f);
            g.setColour(juce::Colours::white);
            g.setFont(juce::Font(juce::FontOptions(14.0f, juce::Font::bold)));
            g.drawText("HAS", 2, 4, 28, 24, juce::Justification::centred);
        }
        setIconImage(img, img);
        setIconTooltip("HAS Karaoke");
    }

    void mouseUp(const juce::MouseEvent& e) override {
        if (e.mods.isPopupMenu()) {
            juce::PopupMenu m;
            m.addItem(1, "เปิด HAS Karaoke");
            m.addSeparator();
            m.addItem(2, "ออกจากโปรแกรม");
            m.showMenuAsync(juce::PopupMenu::Options(), [this](int r) {
                if (r == 1 && onToggle_) onToggle_();
                else if (r == 2 && onQuit_) onQuit_();
            });
        } else {
            if (onToggle_) onToggle_();
        }
    }

private:
    std::function<void()> onToggle_;
    std::function<void()> onQuit_;
};

class KaraokeApplication : public juce::JUCEApplication {
public:
    KaraokeApplication() = default;

    const juce::String getApplicationName() override       { return "HAS Karaoke"; }
    const juce::String getApplicationVersion() override    { return HEARASIDE_VERSION; }
    bool moreThanOneInstanceAllowed() override             { return false; }

    void initialise(const juce::String& commandLine) override {
        mainWindow_ = std::make_unique<MainWindow>();
        if (commandLine.isNotEmpty()) {
            mainWindow_->handleCommandLine(commandLine);
        }

        trayIcon_ = std::make_unique<KaraokeTrayIcon>(
            [this] {
                if (mainWindow_ != nullptr) {
                    mainWindow_->setVisible(true);
                    mainWindow_->toFront(true);
                }
            },
            [this] {
                systemRequestedQuit();
            }
        );
    }

    void shutdown() override {
        trayIcon_.reset();
        mainWindow_.reset();
    }

    void systemRequestedQuit() override {
        quit();
    }

    void anotherInstanceStarted(const juce::String& commandLine) override {
        if (mainWindow_ != nullptr) {
            mainWindow_->setVisible(true);
            mainWindow_->toFront(true);
            if (commandLine.isNotEmpty()) {
                mainWindow_->handleCommandLine(commandLine);
            }
        }
    }

private:
    std::unique_ptr<MainWindow> mainWindow_;
    std::unique_ptr<KaraokeTrayIcon> trayIcon_;
};

} // namespace hearaside::karaoke

START_JUCE_APPLICATION(hearaside::karaoke::KaraokeApplication)
