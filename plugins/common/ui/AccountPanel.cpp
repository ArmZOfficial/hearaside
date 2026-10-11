#include "AccountPanel.h"
#include "Focus.h"

namespace hearaside {

void AccountPanel::BenefitTile::paint(juce::Graphics& g) {
    const auto& p = paletteOf(*this);
    const auto r = getLocalBounds().toFloat();
    const juce::Rectangle<float> tile(r.getX(), r.getCentreY() - 17.0f, 34.0f, 34.0f);
    g.setColour(p.ink);
    g.fillRoundedRectangle(tile, 9.0f);
    icons::draw(g, icon, tile.reduced(8.0f), p.onInk);
    const auto f = uiFont(13.5f, Weight::Medium);
    auto text = r.withTrimmedLeft(34.0f + 14.0f);
    const float h = wrappedHeight(f, this->text, text.getWidth(), 3.0f);
    drawWrapped(g, this->text, f, p.ink, text.withSizeKeepingCentre(text.getWidth(), h), 3.0f, juce::Justification::left, 2);
}

AccountPanel::AccountPanel(Mode mode, const char* plugin)
    : mode_(mode), plugin_(plugin != nullptr ? plugin : (mode == Mode::Full ? "hub" : "track")), mgr_(AccountManager::get()) {
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
        builtExpired_ = dc.expired;
        builtHaveCode_ = dc.userCode.isNotEmpty();
        copyCodeButton_.setButtonText(tr(Str::CopyCode));
        if (dc.expired) {
            addAndMakeVisible(newCodeButton_);
            addAndMakeVisible(cancelButton_);
        } else {
            addAndMakeVisible(openPageButton_);
            addAndMakeVisible(copyCodeButton_);
            addAndMakeVisible(cancelButton_);
            openPageButton_.setEnabled(builtHaveCode_);   // nothing to open or copy until the code is here
            copyCodeButton_.setEnabled(builtHaveCode_);
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
            return 326;
        } else {
            return 440;
        }
    } else if (s == AccountManager::State::WaitingForCode) {
        return mode_ == Mode::Full ? 210 : 330;
    } else { // SignedIn or Offline
        if (mode_ == Mode::Full) {
            return list_.idealHeight(width) + 160;
        } else {
            return 560;
        }
    }
}

void AccountPanel::onSignInClicked() {
    mgr_.startDeviceFlow(plugin_);
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

void AccountPanel::accountDeviceCodeUpdated(const AccountManager::DeviceCode& dc) {
    if (mgr_.state() == AccountManager::State::WaitingForCode
        && (dc.expired != builtExpired_ || dc.userCode.isNotEmpty() != builtHaveCode_)) {
        build();   // the code arrived or ran out: other buttons
        resized();
    }
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
        if (!benefitsBox_.isEmpty()) drawInset(g, benefitsBox_, 14.0f, p);
        if (mgr_.startFailed() && !errorArea_.isEmpty()) {   // "Sign in" couldn't get a code
            drawStatusDot(g, { errorArea_.getX() + 4.0f, errorArea_.getCentreY() }, Dot::Warn, p);
            drawWrapped(g, tr(Str::AccountOffline), uiFont(12.5f), p.warn, errorArea_.withTrimmedLeft(16.0f), 2.0f, juce::Justification::left, 2);
        }
        if (mode_ == Mode::Full) {
            // the Settings page above already shows "Account / Optional. Everything ... without an account."
        } else {
            // Compact subtitle
            auto subArea = r.reduced(18.0f, 0.0f).removeFromTop(60.0f);   // the content's own margins (resized)
            drawWrapped(g, tr(Str::AccountOptional) + " " + tr(Str::SignInHereAll), uiFont(12.5f, Weight::Regular), p.ink2, subArea, 2.0f, juce::Justification::left, 3);
        }
    } else if (s == AccountManager::State::WaitingForCode) {
        const auto& dc = mgr_.deviceCode();
        // Full: left, in line with the buttons and the Settings title above; Compact: centred over full-width buttons
        const auto just = mode_ == Mode::Full ? juce::Justification::centredLeft : juce::Justification::centred;
        g.setFont(uiFont(16.0f, Weight::SemiBold));
        g.setColour(p.ink);
        g.drawText(tr(Str::EnterCode), codeHead_, just, true);

        g.setFont(uiFont(38.0f, Weight::SemiBold));
        g.setColour(dc.userCode.isNotEmpty() ? p.ink : p.ink2);
        g.drawText(dc.userCode.isNotEmpty() ? dc.userCode : juce::String::fromUTF8("\xc2\xb7\xc2\xb7\xc2\xb7\xc2\xb7-\xc2\xb7\xc2\xb7\xc2\xb7\xc2\xb7"), codeBig_, just, true);

        g.setFont(uiFont(13.5f, Weight::Medium));
        g.setColour(p.ink2);
        const auto host = mgr_.serverBaseUrl().replace("https://", "").replace("http://", "");
        g.drawText(trf(Str::CodeAt, { host + "/link" }), codeLink_, just, true);

        juce::String text;
        Dot dot = Dot::Warn;
        juce::Colour colour = p.ink2;
        if (dc.expired) {
            text = tr(Str::CodeExpired);
            colour = p.warn;
        } else if (dc.userCode.isEmpty()) {
            text = tr(Str::GettingCode);
            dot = Dot::Muted;
        } else {
            const int rem = mgr_.getCodeRemainingSeconds();
            text = trf(Str::WaitingAllow, { juce::String::formatted("%d:%02d", rem / 60, rem % 60) });
        }
        const auto f = uiFont(13.0f, dc.expired ? Weight::Medium : Weight::Regular);
        auto stat = codeStatus_;
        if (mode_ == Mode::Compact) stat = stat.withSizeKeepingCentre(juce::jmin(stat.getWidth(), 24.0f + textWidth(f, text)), stat.getHeight());
        drawStatusDot(g, stat.removeFromLeft(14.0f).getCentre(), dot, p);
        stat.removeFromLeft(4.0f);
        g.setFont(f);
        g.setColour(colour);
        g.drawText(text, stat, juce::Justification::centredLeft, true);
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
        r.removeFromTop(14);   // below the Settings page's own title and caption
        layoutBenefits(r, 52);
        r.removeFromTop(20);
        auto btnRow = r.removeFromTop(44);
        signInButton_.setBounds(btnRow.removeFromLeft(240));
        btnRow.removeFromLeft(16);
        createAccountLink_.setBounds(btnRow.removeFromLeft(juce::jmax(160, createAccountLink_.idealWidth())));
        r.removeFromTop(10);
        errorArea_ = r.removeFromTop(38).toFloat();
    } else if (s == AccountManager::State::WaitingForCode) {
        r.removeFromTop(6);
        layoutCodeBlock(r);
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

// heading, the big code, "at hearaside.vercel.app/link", status; the buttons go below r
void AccountPanel::layoutCodeBlock(juce::Rectangle<int>& r) {
    codeHead_ = r.removeFromTop(26).toFloat();
    r.removeFromTop(4);
    codeBig_ = r.removeFromTop(52).toFloat();
    codeLink_ = r.removeFromTop(22).toFloat();
    r.removeFromTop(10);
    codeStatus_ = r.removeFromTop(22).toFloat();
    r.removeFromTop(18);
}

// one box, three rows: ink icon tile + text (prompt 3.12 item 1)
void AccountPanel::layoutBenefits(juce::Rectangle<int>& r, int rowH) {
    auto box = r.removeFromTop(3 * rowH + 2 * 4 + 2 * 12);
    benefitsBox_ = box.toFloat();
    auto in = box.reduced(16, 12);
    for (auto* t : { &tile1_, &tile2_, &tile3_ }) {
        t->setBounds(in.removeFromTop(rowH));
        in.removeFromTop(4);
    }
}

void AccountPanel::layoutCompact(juce::Rectangle<int> r) {
    const auto s = mgr_.state();
    if (s == AccountManager::State::SignedOut) {
        r.removeFromTop(62); // Subtitle painted area
        layoutBenefits(r, 50);
        r.removeFromTop(18);
        signInButton_.setBounds(r.removeFromTop(44));
        r.removeFromTop(8);
        createAccountLink_.setBounds(r.removeFromTop(34));
        r.removeFromTop(8);
        errorArea_ = r.removeFromTop(38).toFloat();
    } else if (s == AccountManager::State::WaitingForCode) {
        layoutCodeBlock(r);
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
