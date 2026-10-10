#pragma once

#include "Components.h"
#include "Controls.h"
#include "SettingsPanel.h"
#include "../AccountManager.h"

namespace hearaside {

class AccountPanel : public juce::Component, public AccountManager::Listener, private juce::Timer {
public:
    enum class Mode {
        Full,    // Hub Settings › Account (wide card)
        Compact  // Track Fine settings sheet & App Audio (narrow, 340-380px)
    };

    explicit AccountPanel(Mode mode = Mode::Full);
    ~AccountPanel() override;

    void setMode(Mode m);
    Mode mode() const noexcept { return mode_; }

    std::function<void()> onBack;
    std::function<void(const juce::String&)> onToast;

    void refreshTexts();
    int idealHeight(int width) const;

    void paint(juce::Graphics&) override;
    void resized() override;

    // AccountManager::Listener
    void accountStateChanged(AccountManager::State newState) override;
    void accountProfileChanged(const AccountManager::Profile& profile) override;
    void accountDeviceCodeUpdated(const AccountManager::DeviceCode& code) override;

private:
    void timerCallback() override;
    void build();
    void layoutFull(juce::Rectangle<int> r);
    void layoutCompact(juce::Rectangle<int> r);

    void onSignInClicked();
    void onOpenPageClicked();
    void onCopyCodeClicked();
    void onSignOutClicked();
    void onChangePhotoClicked();
    void onRemovePhotoClicked();

    Mode mode_ = Mode::Full;
    AccountManager& mgr_;

    // Navigation / Header
    BackButton back_;

    // Signed out controls
    struct BenefitTile : juce::Component {
        icons::Icon icon = icons::Icon::Link;
        juce::String text;
        void paint(juce::Graphics&) override;
    } tile1_, tile2_, tile3_;
    PrimaryButton signInButton_ { icons::Icon::Count, 44.0f };
    GhostButton createAccountLink_ { tr(Str::CreateAccount) };

    // Waiting for code controls
    GhostButton copyCodeButton_ { tr(Str::CopyCode) };
    PrimaryButton openPageButton_ { icons::Icon::Count, 40.0f };
    GhostButton cancelButton_ { tr(Str::Cancel) };
    GhostButton newCodeButton_ { tr(Str::GetNewCode) };

    // Signed in controls
    Banner offlineBanner_;
    GhostButton changePhoto_ { tr(Str::ChangeEllipsis) };
    GhostButton removePhoto_ { tr(Str::RemoveWord) };
    GhostButton photoCompact_ { tr(Str::PhotoEllipsis) };
    TextField displayNameField_, usernameField_, aboutField_, deviceField_;
    Switch syncSwitch_, hideEmailSwitch_;
    GhostButton webLink_ { tr(Str::ManageOnWeb) };
    GhostButton signOutButton_ { tr(Str::SignOut) };
    GhostButton confirmSignOut_ { tr(Str::SignOut), GhostButton::Style::Danger };
    GhostButton keepButton_ { tr(Str::Keep) };
    bool confirmingSignOut_ = false;

    // Compact layout fields
    Field nameField_ { {}, &displayNameField_, 36 };
    Field userField_ { {}, &usernameField_, 36 };
    Field aboutRowField_ { {}, &aboutField_, 36 };

    // Full layout rows
    SettingList list_;

    juce::Rectangle<float> avatarArea_;
    juce::Rectangle<float> syncRowArea_, hideEmailRowArea_;
};

} // namespace hearaside
