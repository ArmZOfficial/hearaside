#include "AccountPanel.h"
#include "Focus.h"

namespace hearaside {

void AccountPanel::BenefitTile::paint(juce::Graphics& g) {
    const auto& p = paletteOf(*this);
    const auto r = getLocalBounds().toFloat();
    drawInset(g, r, 14.0f, p);
    auto in = r.reduced(14.0f, 12.0f);
    const auto iconArea = in.removeFromTop(24.0f).removeFromLeft(24.0f);
    icons::draw(g, icon, iconArea, p.ink);
    in.removeFromTop(8.0f);
    drawWrapped(g, text, uiFont(12.5f, Weight::Medium), p.ink, in, 3.0f, juce::Justification::left, 3);
}

AccountPanel::AccountPanel(Mode mode)
    : mode_(mode), mgr_(AccountManager::get()) {
    mgr_.addListener(this);

    tile1_.icon = icons::Icon::Link;
    tile2_.icon = icons::Icon::Person;
    tile3_.icon = icons::Icon::Sliders;

    signInButton_.setNoIcon();
    signInButton_.setButtonText(tr(Str::SignInBrowser));
    signInButton_.onClick = [this] { onSignInClicked(); };

    createAccountLink_.setStyle(GhostButton::Style::Ghost);
    createAccountLink_.setButtonText(tr(Str::CreateAccount));
    createAccountLink_.onClick = [this] {
        juce::URL(mgr_.serverBaseUrl() + "/signup").launchInDefaultBrowser();
    };

    openPageButton_.setNoIcon();
    openPageButton_.setButtonText(tr(Str::OpenThePage));
    openPageButton_.onClick = [this] { onOpenPageClicked(); };

    copyCodeButton_.setButtonText(tr(Str::CopyCode));
    copyCodeButton_.onClick = [this] { onCopyCodeClicked(); };

    cancelButton_.setButtonText(tr(Str::Cancel));
    cancelButton_.onClick = [this] { mgr_.cancelDeviceFlow(); };

    newCodeButton_.setButtonText(tr(Str::GetNewCode));
    newCodeButton_.onClick = [this] { onSignInClicked(); };

    changePhoto_.onClick = [this] { onChangePhotoClicked(); };
    removePhoto_.onClick = [this] { onRemovePhotoClicked(); };
    photoCompact_.onClick = [this] { onChangePhotoClicked(); };

    displayNameField_.onReturnKey = displayNameField_.onFocusLost = [this] {
        mgr_.updateProfile(displayNameField_.getText(), usernameField_.getText(), aboutField_.getText());
    };
    usernameField_.onReturnKey = usernameField_.onFocusLost = [this] {
        mgr_.updateProfile(displayNameField_.getText(), usernameField_.getText(), aboutField_.getText());
    };
    aboutField_.onReturnKey = aboutField_.onFocusLost = [this] {
        mgr_.updateProfile(displayNameField_.getText(), usernameField_.getText(), aboutField_.getText());
    };
    deviceField_.onReturnKey = deviceField_.onFocusLost = [this] {
        mgr_.setDeviceName(deviceField_.getText());
    };

    syncSwitch_.onClick = [this] {
        mgr_.setSyncAppearance(syncSwitch_.isOn());
    };
    hideEmailSwitch_.onClick = [this] {
        mgr_.setHideEmail(hideEmailSwitch_.isOn());
    };

    webLink_.onClick = [this] {
        juce::URL(mgr_.serverBaseUrl() + "/account").launchInDefaultBrowser();
    };

    signOutButton_.setStyle(GhostButton::Style::Danger);
    signOutButton_.onClick = [this] {
        confirmingSignOut_ = true;
        resized();
        repaint();
    };

    confirmSignOut_.setStyle(GhostButton::Style::DangerSolid);
    confirmSignOut_.onClick = [this] { onSignOutClicked(); };

    keepButton_.setStyle(GhostButton::Style::Ghost);
    keepButton_.onClick = [this] {
        confirmingSignOut_ = false;
        resized();
        repaint();
    };

    back_.setTitle(tr(Str::BackTip));
    back_.setTooltip(tr(Str::BackTip));
    back_.onClick = [this] {
        if (onBack) onBack();
    };
    getProperties().set("hsCovers", true);

    startTimerHz(4);
    build();
}

AccountPanel::~AccountPanel() {
    stopTimer();
    mgr_.removeListener(this);
}

void AccountPanel::setMode(Mode m) {
    if (m != mode_) {
        mode_ = m;
        build();
        resized();
        repaint();
    }
}

void AccountPanel::refreshTexts() {
    signInButton_.setButtonText(tr(Str::SignInBrowser));
    createAccountLink_.setButtonText(tr(Str::CreateAccount));
    openPageButton_.setButtonText(tr(Str::OpenThePage));
    copyCodeButton_.setButtonText(tr(Str::CopyCode));
    cancelButton_.setButtonText(tr(Str::Cancel));
    newCodeButton_.setButtonText(tr(Str::GetNewCode));
    changePhoto_.setButtonText(tr(Str::ChangeEllipsis));
    removePhoto_.setButtonText(tr(Str::RemoveWord));
    photoCompact_.setButtonText(tr(Str::PhotoEllipsis));
    webLink_.setButtonText(mode_ == Mode::Full ? tr(Str::ManageOnWeb) : tr(Str::MoreOnWeb));
    signOutButton_.setButtonText(tr(Str::SignOut));
    confirmSignOut_.setButtonText(tr(Str::SignOut));
    keepButton_.setButtonText(tr(Str::Keep));

    tile1_.text = mode_ == Mode::Full ? tr(Str::BenefitLinks) : tr(Str::BenefitLinksShort);
    tile2_.text = mode_ == Mode::Full ? tr(Str::BenefitFriends) : tr(Str::BenefitFriendsShort);
    tile3_.text = mode_ == Mode::Full ? tr(Str::BenefitAppearance) : tr(Str::BenefitAppearanceShort);

    build();
    repaint();
}

void AccountPanel::build() {
    removeAllChildren();
    list_.clear();

    const auto s = mgr_.state();
    const auto& prof = mgr_.profile();

    if (mode_ == Mode::Compact) {
        addAndMakeVisible(back_);
    }

    if (s == AccountManager::State::SignedOut) {
        tile1_.text = mode_ == Mode::Full ? tr(Str::BenefitLinks) : tr(Str::BenefitLinksShort);
        tile2_.text = mode_ == Mode::Full ? tr(Str::BenefitFriends) : tr(Str::BenefitFriendsShort);
        tile3_.text = mode_ == Mode::Full ? tr(Str::BenefitAppearance) : tr(Str::BenefitAppearanceShort);

        addAndMakeVisible(tile1_);
        addAndMakeVisible(tile2_);
        addAndMakeVisible(tile3_);
        addAndMakeVisible(signInButton_);
        addAndMakeVisible(createAccountLink_);
    } else if (s == AccountManager::State::WaitingForCode) {
        const auto& dc = mgr_.deviceCode();
        if (dc.expired) {
            addAndMakeVisible(newCodeButton_);
            addAndMakeVisible(cancelButton_);
        } else {
            addAndMakeVisible(openPageButton_);
            addAndMakeVisible(copyCodeButton_);
            addAndMakeVisible(cancelButton_);
        }
    } else { // SignedIn or Offline
        if (s == AccountManager::State::Offline) {
            offlineBanner_.set(Banner::Style::Warning, icons::Icon::Warning, trf(Str::AccountOffline, { mgr_.serverBaseUrl().replace("https://", "") }));
            addAndMakeVisible(offlineBanner_);
        }

        displayNameField_.setText(prof.displayName, false);
        usernameField_.setText(prof.handle, false);
        aboutField_.setText(prof.about, false);
        deviceField_.setText(prof.deviceName, false);

        syncSwitch_.setOn(prof.syncAppearance, false);
        hideEmailSwitch_.setOn(prof.hideEmail, false);

        if (mode_ == Mode::Full) {
            // Photo row
            struct PhotoRow : juce::Component {
                GhostButton* ch = nullptr;
                GhostButton* rm = nullptr;
                void resized() override {
                    if (ch) ch->setBounds(getWidth() - 150, (getHeight() - 30) / 2, 70, 30);
                    if (rm) rm->setBounds(getWidth() - 72, (getHeight() - 30) / 2, 72, 30);
                }
            };
            auto photoComp = std::make_unique<PhotoRow>();
            photoComp->ch = &changePhoto_;
            photoComp->rm = &removePhoto_;
            photoComp->addAndMakeVisible(changePhoto_);
            photoComp->addAndMakeVisible(removePhoto_);
            list_.add(std::make_unique<SettingRow>(tr(Str::AccountPhoto), tr(Str::AccountPhotoCap), photoComp.release(), 160, 34));

            // Name row
            list_.add(std::make_unique<SettingRow>(tr(Str::AccountDisplayName), tr(Str::AccountDisplayNameCap), &displayNameField_, 280, 36));

            // Username row
            list_.add(std::make_unique<SettingRow>(tr(Str::AccountUsername), tr(Str::UsernameAvailable), &usernameField_, 280, 36));

            // About you row
            list_.add(std::make_unique<SettingRow>(tr(Str::AccountAbout), trf(Str::AboutCounter, { juce::String(prof.about.length()) }), &aboutField_, 280, 36));

            // Sync appearance row
            list_.add(std::make_unique<SettingRow>(tr(Str::SyncAppearance), tr(Str::SyncAppearanceCap), &syncSwitch_, 44, 26));

            // Hide email row
            list_.add(std::make_unique<SettingRow>(tr(Str::HideEmailOnScreen), tr(Str::HideEmailCap), &hideEmailSwitch_, 44, 26));

            // This computer row
            list_.add(std::make_unique<SettingRow>(tr(Str::ThisComputer), tr(Str::ThisComputerCap), &deviceField_, 280, 36));

            addAndMakeVisible(list_);
        } else {
            // Compact mode
            addAndMakeVisible(photoCompact_);
            addAndMakeVisible(nameField_);
            addAndMakeVisible(userField_);
            addAndMakeVisible(aboutRowField_);
            addAndMakeVisible(syncSwitch_);
            addAndMakeVisible(hideEmailSwitch_);
        }

        addAndMakeVisible(webLink_);
        if (confirmingSignOut_) {
            addAndMakeVisible(confirmSignOut_);
            addAndMakeVisible(keepButton_);
        } else {
            addAndMakeVisible(signOutButton_);
        }
    }
}

int AccountPanel::idealHeight(int width) const {
    const auto s = mgr_.state();
    if (s == AccountManager::State::SignedOut) {
        if (mode_ == Mode::Full) {
            return 380;
        } else {
            return 440;
        }
    } else if (s == AccountManager::State::WaitingForCode) {
        return 320;
    } else { // SignedIn or Offline
        if (mode_ == Mode::Full) {
            return list_.idealHeight(width) + 160;
        } else {
            return 560;
        }
    }
}

void AccountPanel::onSignInClicked() {
    mgr_.startDeviceFlow(mode_ == Mode::Full ? "HEARASIDE Hub" : "HEARASIDE Track");
}

void AccountPanel::onOpenPageClicked() {
    const auto& dc = mgr_.deviceCode();
    juce::URL url(dc.verificationUriComplete.isNotEmpty() ? dc.verificationUriComplete : (mgr_.serverBaseUrl() + "/link?code=" + dc.userCode));
    url.launchInDefaultBrowser();
}

void AccountPanel::onCopyCodeClicked() {
    const auto code = mgr_.deviceCode().userCode;
    juce::SystemClipboard::copyTextToClipboard(code);
    copyCodeButton_.setButtonText(tr(Str::Copied));
    if (onToast) onToast(tr(Str::Copied));
}

void AccountPanel::onSignOutClicked() {
    confirmingSignOut_ = false;
    mgr_.signOut();
    if (onToast) onToast(tr(Str::SignedOutToast));
}

void AccountPanel::onChangePhotoClicked() {
    auto chooser = std::make_shared<juce::FileChooser>(tr(Str::AccountPhoto), juce::File::getSpecialLocation(juce::File::userPicturesDirectory), "*.jpg;*.jpeg;*.png;*.webp");
    chooser->launchAsync(juce::FileBrowserComponent::openMode | juce::FileBrowserComponent::canSelectFiles,
                         [chooser, this](const juce::FileChooser& fc) {
        const auto f = fc.getResult();
        if (f.existsAsFile()) {
            mgr_.uploadAvatar(f);
        }
    });
}

void AccountPanel::onRemovePhotoClicked() {
    mgr_.removeAvatar();
}

void AccountPanel::timerCallback() {
    if (mgr_.state() == AccountManager::State::WaitingForCode) {
        repaint();
    }
}

void AccountPanel::accountStateChanged(AccountManager::State) {
    build();
    resized();
    repaint();
}

void AccountPanel::accountProfileChanged(const AccountManager::Profile&) {
    build();
    resized();
    repaint();
}

void AccountPanel::accountDeviceCodeUpdated(const AccountManager::DeviceCode&) {
    repaint();
}

void AccountPanel::paint(juce::Graphics& g) {
    const auto& p = paletteOf(*this);
    const auto s = mgr_.state();
    const auto& prof = mgr_.profile();
    auto r = getLocalBounds().toFloat();

    if (mode_ == Mode::Compact) {
        g.setColour(p.paper.overlaidWith(p.sheet));
        g.fillRoundedRectangle(getLocalBounds().toFloat(), 20.0f);
        g.setColour(p.ink);
        g.setFont(uiFont(15.0f, Weight::SemiBold));
        g.drawText(tr(Str::AccountTitle), juce::Rectangle<float>(float(back_.getRight() + 10), 14.0f, float(getWidth() - back_.getRight() - 28), 34.0f),
                   juce::Justification::centredLeft, true);
        r.removeFromTop(54.0f); // Back button row
    }

    if (s == AccountManager::State::SignedOut) {
        if (mode_ == Mode::Full) {
            // Header: Account / Optional. Everything in HEARASIDE works without an account.
            auto headArea = r.removeFromTop(60.0f);
            g.setFont(uiFont(22.0f, Weight::SemiBold));
            g.setColour(p.ink);
            g.drawText(tr(Str::AccountTitle), headArea.removeFromTop(28.0f), juce::Justification::left, true);
            g.setFont(uiFont(13.5f, Weight::Regular));
            g.setColour(p.ink2);
            g.drawText(tr(Str::AccountOptional), headArea, juce::Justification::left, true);
        } else {
            // Compact subtitle
            auto subArea = r.removeFromTop(44.0f);
            g.setFont(uiFont(12.5f, Weight::Regular));
            g.setColour(p.ink2);
            drawWrapped(g, tr(Str::SignInHereAll), uiFont(12.5f, Weight::Regular), p.ink2, subArea, 2.0f, juce::Justification::left, 3);
        }
    } else if (s == AccountManager::State::WaitingForCode) {
        const auto& dc = mgr_.deviceCode();
        auto headArea = r.removeFromTop(40.0f);
        g.setFont(uiFont(16.0f, Weight::SemiBold));
        g.setColour(p.ink);
        g.drawText(tr(Str::EnterCode), headArea, juce::Justification::centred, true);

        // Code display area
        auto codeArea = r.removeFromTop(60.0f);
        g.setFont(uiFont(38.0f, Weight::SemiBold));
        g.setColour(p.ink);
        g.drawText(dc.userCode, codeArea, juce::Justification::centred, true);

        // Subtitle: at hearaside.app/link
        auto linkArea = r.removeFromTop(26.0f);
        g.setFont(uiFont(13.5f, Weight::Medium));
        g.setColour(p.ink2);
        const auto host = mgr_.serverBaseUrl().replace("https://", "").replace("http://", "");
        g.drawText(trf(Str::CodeAt, { host + "/link" }), linkArea, juce::Justification::centred, true);

        // Status countdown
        r.removeFromTop(12.0f);
        auto statArea = r.removeFromTop(24.0f);
        const int rem = mgr_.getCodeRemainingSeconds();
        const int mins = rem / 60;
        const int secs = rem % 60;
        const juce::String timeStr = juce::String::formatted("%d:%02d", mins, secs);

        if (dc.expired) {
            drawStatusDot(g, statArea.removeFromLeft(14.0f).getCentre(), Dot::Warn, p);
            g.setFont(uiFont(13.0f, Weight::Medium));
            g.setColour(p.warn);
            g.drawText(tr(Str::CodeExpired), statArea, juce::Justification::left, true);
        } else {
            drawStatusDot(g, statArea.removeFromLeft(14.0f).getCentre(), Dot::Warn, p);
            g.setFont(uiFont(13.0f, Weight::Regular));
            g.setColour(p.ink2);
            g.drawText(trf(Str::WaitingAllow, { timeStr }), statArea, juce::Justification::left, true);
        }
    } else { // SignedIn or Offline
        if (avatarArea_.getWidth() > 0) {
            drawAvatar(g, avatarArea_, prof.displayName.isNotEmpty() ? prof.displayName : prof.handle, true, p, prof.avatarImage);

            auto textR = juce::Rectangle<float>(avatarArea_.getRight() + 14.0f, avatarArea_.getY(), r.getRight() - avatarArea_.getRight() - 14.0f, avatarArea_.getHeight());
            auto top = textR.removeFromTop(24.0f);
            g.setFont(uiFont(17.0f, Weight::SemiBold));
            g.setColour(p.ink);
            g.drawText(prof.displayName.isNotEmpty() ? prof.displayName : prof.handle, top, juce::Justification::centredLeft, true);

            auto bottom = textR;
            g.setFont(uiFont(13.0f, Weight::Regular));
            g.setColour(p.ink2);
            juce::String sub = "@" + prof.handle;
            const auto masked = mgr_.getMaskedEmail();
            if (masked.isNotEmpty()) sub += " · " + masked;
            g.drawText(sub, bottom, juce::Justification::centredLeft, true);
        }

        if (mode_ == Mode::Compact) {
            if (!syncRowArea_.isEmpty()) {
                g.setFont(uiFont(13.0f, Weight::Medium));
                g.setColour(p.ink);
                g.drawText(tr(Str::SyncAppearance), syncRowArea_, juce::Justification::centredLeft, true);
            }
            if (!hideEmailRowArea_.isEmpty()) {
                g.setFont(uiFont(13.0f, Weight::Medium));
                g.setColour(p.ink);
                g.drawText(tr(Str::HideEmailOnScreen), hideEmailRowArea_, juce::Justification::centredLeft, true);
            }
        }

        if (confirmingSignOut_) {
            auto confArea = juce::Rectangle<float>(0.0f, float(getHeight()) - 84.0f, float(getWidth()), 24.0f);
            g.setFont(uiFont(13.0f, Weight::Medium));
            g.setColour(p.danger);
            g.drawText(tr(Str::SignOutConfirm), confArea, juce::Justification::centredLeft, true);
        }
    }
}

void AccountPanel::resized() {
    auto r = getLocalBounds();
    if (mode_ == Mode::Compact) {
        r.reduce(18, 0);
        r.removeFromTop(14);
        auto top = r.removeFromTop(34);
        back_.setBounds(top.removeFromLeft(back_.idealWidth()));
        r.removeFromTop(14);
        r.removeFromBottom(16);
    }

    if (mode_ == Mode::Full) {
        layoutFull(r);
    } else {
        layoutCompact(r);
    }
}

void AccountPanel::layoutFull(juce::Rectangle<int> r) {
    const auto s = mgr_.state();
    if (s == AccountManager::State::SignedOut) {
        r.removeFromTop(68); // Title & Subtitle painted area
        auto tilesR = r.removeFromTop(120);
        const int gap = 12;
        const int tileW = (tilesR.getWidth() - 2 * gap) / 3;
        tile1_.setBounds(tilesR.removeFromLeft(tileW));
        tilesR.removeFromLeft(gap);
        tile2_.setBounds(tilesR.removeFromLeft(tileW));
        tilesR.removeFromLeft(gap);
        tile3_.setBounds(tilesR);

        r.removeFromTop(24);
        auto btnRow = r.removeFromTop(44);
        signInButton_.setBounds(btnRow.removeFromLeft(240));
        btnRow.removeFromLeft(16);
        createAccountLink_.setBounds(btnRow.removeFromLeft(160));
    } else if (s == AccountManager::State::WaitingForCode) {
        r.removeFromTop(140); // Code header, big user code, domain link, status
        const auto& dc = mgr_.deviceCode();
        auto btnRow = r.removeFromTop(40);
        if (dc.expired) {
            newCodeButton_.setBounds(btnRow.removeFromLeft(160));
            btnRow.removeFromLeft(12);
            cancelButton_.setBounds(btnRow.removeFromLeft(100));
        } else {
            openPageButton_.setBounds(btnRow.removeFromLeft(160));
            btnRow.removeFromLeft(12);
            copyCodeButton_.setBounds(btnRow.removeFromLeft(120));
            btnRow.removeFromLeft(12);
            cancelButton_.setBounds(btnRow.removeFromLeft(100));
        }
    } else { // SignedIn or Offline
        if (s == AccountManager::State::Offline) {
            offlineBanner_.setBounds(r.removeFromTop(42));
            r.removeFromTop(12);
        }

        avatarArea_ = r.removeFromTop(64).removeFromLeft(64).toFloat();
        r.removeFromTop(20);

        const int listH = list_.idealHeight(r.getWidth());
        list_.setBounds(r.removeFromTop(listH));

        r.removeFromTop(16);
        auto botRow = r.removeFromTop(36);
        webLink_.setBounds(botRow.removeFromLeft(280));
        if (confirmingSignOut_) {
            confirmSignOut_.setBounds(botRow.removeFromRight(100));
            botRow.removeFromRight(8);
            keepButton_.setBounds(botRow.removeFromRight(80));
        } else {
            signOutButton_.setBounds(botRow.removeFromRight(100));
        }
    }
}

void AccountPanel::layoutCompact(juce::Rectangle<int> r) {
    const auto s = mgr_.state();
    if (s == AccountManager::State::SignedOut) {
        r.removeFromTop(48); // Subtitle painted area
        tile1_.setBounds(r.removeFromTop(68));
        r.removeFromTop(8);
        tile2_.setBounds(r.removeFromTop(68));
        r.removeFromTop(8);
        tile3_.setBounds(r.removeFromTop(68));

        r.removeFromTop(18);
        signInButton_.setBounds(r.removeFromTop(44));
        r.removeFromTop(8);
        createAccountLink_.setBounds(r.removeFromTop(34));
    } else if (s == AccountManager::State::WaitingForCode) {
        r.removeFromTop(140);
        const auto& dc = mgr_.deviceCode();
        if (dc.expired) {
            newCodeButton_.setBounds(r.removeFromTop(40));
            r.removeFromTop(8);
            cancelButton_.setBounds(r.removeFromTop(34));
        } else {
            openPageButton_.setBounds(r.removeFromTop(40));
            r.removeFromTop(8);
            auto row2 = r.removeFromTop(36);
            copyCodeButton_.setBounds(row2.removeFromLeft((row2.getWidth() - 8) / 2));
            row2.removeFromLeft(8);
            cancelButton_.setBounds(row2);
        }
    } else { // SignedIn or Offline
        if (s == AccountManager::State::Offline) {
            offlineBanner_.setBounds(r.removeFromTop(42));
            r.removeFromTop(10);
        }

        auto topR = r.removeFromTop(56);
        avatarArea_ = topR.removeFromLeft(52).toFloat();
        photoCompact_.setBounds(topR.removeFromRight(80).withSizeKeepingCentre(80, 30));

        r.removeFromTop(16);
        nameField_.setTexts(tr(Str::AccountDisplayName), {});
        nameField_.setBounds(r.removeFromTop(62));
        r.removeFromTop(6);

        userField_.setTexts(tr(Str::AccountUsername), {});
        userField_.setBounds(r.removeFromTop(62));
        r.removeFromTop(6);

        aboutRowField_.setTexts(tr(Str::AccountAbout), {});
        aboutRowField_.setBounds(r.removeFromTop(62));
        r.removeFromTop(10);

        // Sync switch row
        auto sw1Row = r.removeFromTop(32);
        syncSwitch_.setBounds(sw1Row.removeFromRight(44).withSizeKeepingCentre(44, 26));
        syncRowArea_ = sw1Row.toFloat();

        // Hide email switch row
        auto sw2Row = r.removeFromTop(32);
        hideEmailSwitch_.setBounds(sw2Row.removeFromRight(44).withSizeKeepingCentre(44, 26));
        hideEmailRowArea_ = sw2Row.toFloat();

        r.removeFromTop(14);
        webLink_.setBounds(r.removeFromTop(32));
        r.removeFromTop(6);
        if (confirmingSignOut_) {
            auto cRow = r.removeFromTop(36);
            confirmSignOut_.setBounds(cRow.removeFromLeft((cRow.getWidth() - 8) / 2));
            cRow.removeFromLeft(8);
            keepButton_.setBounds(cRow);
        } else {
            signOutButton_.setBounds(r.removeFromTop(36));
        }
    }
}

} // namespace hearaside
