#pragma once

#include <juce_core/juce_core.h>
#include <juce_graphics/juce_graphics.h>
#include <juce_events/juce_events.h>
#include <atomic>
#include <mutex>

namespace hearaside {

class AccountManager : private juce::Thread, private juce::Timer {
public:
    enum class State {
        SignedOut,
        WaitingForCode,
        SignedIn,
        Offline
    };

    struct Profile {
        juce::String id;
        juce::String email;
        juce::String handle;
        juce::String displayName;
        juce::String about;
        juce::String avatarUrl;
        juce::Image avatarImage;
        juce::String locale = "en";
        bool hideEmail = true;
        bool syncAppearance = false;
        juce::String deviceName;
        juce::String plan = "free";
    };

    struct DeviceCode {
        juce::String deviceCode;
        juce::String userCode;
        juce::String verificationUri;
        juce::String verificationUriComplete;
        int expiresInSec = 600;
        int intervalSec = 5;
        juce::Time expiresAt;
        bool expired = false;
    };

    struct Listener {
        virtual ~Listener() = default;
        virtual void accountStateChanged(State newState) = 0;
        virtual void accountProfileChanged(const Profile& profile) {}
        virtual void accountDeviceCodeUpdated(const DeviceCode& code) {}
    };

    static AccountManager& get();

    void addListener(Listener* l);
    void removeListener(Listener* l);

    State state() const noexcept { return state_.load(std::memory_order_relaxed); }
    bool isSignedIn() const noexcept {
        const auto s = state();
        return s == State::SignedIn || s == State::Offline;
    }

    const Profile& profile() const noexcept { return profile_; }
    const DeviceCode& deviceCode() const noexcept { return deviceCode_; }

    juce::String serverBaseUrl() const;
    void setServerBaseUrl(const juce::String& url);

    void startDeviceFlow(const juce::String& pluginName = "HEARASIDE");
    void cancelDeviceFlow();
    void signOut();

    void refreshProfile();
    void updateProfile(const juce::String& displayName, const juce::String& handle, const juce::String& about);
    void uploadAvatar(const juce::File& file);
    void removeAvatar();
    void setHideEmail(bool hide);
    void setSyncAppearance(bool sync);
    void setDeviceName(const juce::String& name);

    juce::String getMaskedEmail() const;
    int getCodeRemainingSeconds() const;

    static bool testDpapiRoundtrip();

private:
    AccountManager();
    ~AccountManager() override;

    void run() override;
    void timerCallback() override;

    juce::File storageFile() const;
    void loadFromDisk();
    void saveToDisk();
    void clearDisk();

    static juce::MemoryBlock encryptData(const juce::String& text);
    static juce::String decryptData(const juce::MemoryBlock& block);

    bool httpPost(const juce::String& endpoint, const juce::var& body, juce::var& responseJson, int& statusCode, const juce::String& bearerToken = {});
    bool httpGet(const juce::String& endpoint, juce::var& responseJson, int& statusCode, const juce::String& bearerToken = {});
    bool httpPatch(const juce::String& endpoint, const juce::var& body, juce::var& responseJson, int& statusCode, const juce::String& bearerToken = {});
    bool httpDelete(const juce::String& endpoint, juce::var& responseJson, int& statusCode, const juce::String& bearerToken = {});
    bool uploadAvatarImage(const juce::File& file, juce::var& responseJson, int& statusCode, const juce::String& bearerToken);

    void doStartDeviceFlow();
    void doPollDeviceCode();
    void doRefreshToken();
    void doFetchProfile();
    void doUpdateProfile();
    void doUploadAvatar();
    void doRemoveAvatar();
    void doUpdateDeviceName();
    void doSignOut();

    void parseProfileJson(const juce::var& json);
    void setState(State s);
    void notifyStateChanged();
    void notifyProfileChanged();
    void notifyDeviceCodeUpdated();

    juce::ListenerList<Listener> listeners_;
    std::atomic<State> state_ { State::SignedOut };
    Profile profile_;
    DeviceCode deviceCode_;

    juce::String serverBase_;
    juce::String accessToken_;
    juce::String refreshToken_;
    juce::Time accessTokenExpiresAt_;
    juce::String deviceId_;

    enum class Action {
        None,
        StartDeviceFlow,
        PollToken,
        FetchProfile,
        UpdateProfile,
        UploadAvatar,
        RemoveAvatar,
        UpdateDeviceName,
        SignOut
    };
    std::atomic<Action> pendingAction_ { Action::None };
    juce::WaitableEvent actionEvent_;

    juce::String pendingPluginName_;
    juce::String pendingDisplayName_, pendingHandle_, pendingAbout_;
    juce::File pendingAvatarFile_;
    juce::String pendingDeviceName_;

    juce::Time lastFileModTime_;
    mutable std::mutex mutex_;
};

} // namespace hearaside
