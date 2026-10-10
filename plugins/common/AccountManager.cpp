#include "AccountManager.h"
#include "Strings.h"
#include "Host.h"

#if JUCE_WINDOWS
#include <windows.h>
#include <wincrypt.h>
#pragma comment(lib, "crypt32.lib")
#endif

namespace hearaside {

namespace {
juce::String getEnv(const char* name, const juce::String& def = {}) {
    const auto val = juce::SystemStats::getEnvironmentVariable(name, {});
    return val.trim().isNotEmpty() ? val.trim() : def;
}
} // namespace

AccountManager& AccountManager::get() {
    static AccountManager instance;
    return instance;
}

AccountManager::AccountManager() : juce::Thread("AccountWorker") {
    serverBase_ = getEnv("HEARASIDE_API_URL", "https://hearaside.vercel.app");
    if (serverBase_.endsWithChar('/')) serverBase_ = serverBase_.dropLastCharacters(1);

    loadFromDisk();
    startThread(juce::Thread::Priority::normal);
    startTimerHz(1); // 1 Hz timer for file watch & countdown ticks
}

AccountManager::~AccountManager() {
    stopTimer();
    signalThreadShouldExit();
    actionEvent_.signal();
    stopThread(3000);
}

void AccountManager::addListener(Listener* l) {
    listeners_.add(l);
}

void AccountManager::removeListener(Listener* l) {
    listeners_.remove(l);
}

juce::String AccountManager::serverBaseUrl() const {
    const std::lock_guard<std::mutex> lk(mutex_);
    return serverBase_;
}

void AccountManager::setServerBaseUrl(const juce::String& url) {
    juce::String s = url.trim();
    if (s.endsWithChar('/')) s = s.dropLastCharacters(1);
    const std::lock_guard<std::mutex> lk(mutex_);
    serverBase_ = s;
}

juce::File AccountManager::storageFile() const {
    const auto dir = juce::File::getSpecialLocation(juce::File::userApplicationDataDirectory).getChildFile("HEARASIDE");
    if (!dir.exists()) dir.createDirectory();
    return dir.getChildFile("account.dat");
}

juce::MemoryBlock AccountManager::encryptData(const juce::String& text) {
    if (text.isEmpty()) return {};
#if JUCE_WINDOWS
    const auto raw = text.toRawUTF8();
    DATA_BLOB in;
    in.pbData = reinterpret_cast<BYTE*>(const_cast<char*>(raw));
    in.cbData = static_cast<DWORD>(strlen(raw));

    DATA_BLOB out;
    if (CryptProtectData(&in, L"HEARASIDE", nullptr, nullptr, nullptr, CRYPTPROTECT_UI_FORBIDDEN, &out)) {
        juce::MemoryBlock mb(out.pbData, out.cbData);
        LocalFree(out.pbData);
        return mb;
    }
#endif
    // Fallback: simple copy
    juce::MemoryBlock mb;
    mb.append(text.toRawUTF8(), strlen(text.toRawUTF8()));
    return mb;
}

juce::String AccountManager::decryptData(const juce::MemoryBlock& block) {
    if (block.isEmpty()) return {};
#if JUCE_WINDOWS
    DATA_BLOB in;
    in.pbData = reinterpret_cast<BYTE*>(const_cast<char*>(static_cast<const char*>(block.getData())));
    in.cbData = static_cast<DWORD>(block.getSize());

    DATA_BLOB out;
    if (CryptUnprotectData(&in, nullptr, nullptr, nullptr, nullptr, CRYPTPROTECT_UI_FORBIDDEN, &out)) {
        juce::String str = juce::String::fromUTF8(reinterpret_cast<const char*>(out.pbData), static_cast<int>(out.cbData));
        LocalFree(out.pbData);
        return str;
    }
#endif
    return juce::String::fromUTF8(static_cast<const char*>(block.getData()), static_cast<int>(block.getSize()));
}

void AccountManager::loadFromDisk() {
    const auto f = storageFile();
    if (!f.existsAsFile()) {
        state_.store(State::SignedOut, std::memory_order_relaxed);
        return;
    }

    lastFileModTime_ = f.getLastModificationTime();
    juce::MemoryBlock mb;
    if (!f.loadFileAsData(mb) || mb.isEmpty()) {
        state_.store(State::SignedOut, std::memory_order_relaxed);
        return;
    }

    const auto jsonStr = decryptData(mb);
    const auto parsed = juce::JSON::parse(jsonStr);
    if (!parsed.isObject()) {
        state_.store(State::SignedOut, std::memory_order_relaxed);
        return;
    }

    const auto* obj = parsed.getDynamicObject();
    if (obj == nullptr) return;

    {
        const std::lock_guard<std::mutex> lk(mutex_);
        accessToken_ = obj->getProperty("access_token").toString();
        refreshToken_ = obj->getProperty("refresh_token").toString();
        deviceId_ = obj->getProperty("device_id").toString();
        accessTokenExpiresAt_ = juce::Time(static_cast<juce::int64>(obj->getProperty("expires_at")));

        const auto userVal = obj->getProperty("user");
        if (userVal.isObject()) {
            parseProfileJson(userVal);
        }
    }

    if (accessToken_.isNotEmpty()) {
        setState(State::SignedIn);
    } else {
        setState(State::SignedOut);
    }
}

void AccountManager::saveToDisk() {
    auto* obj = new juce::DynamicObject();
    {
        const std::lock_guard<std::mutex> lk(mutex_);
        obj->setProperty("access_token", accessToken_);
        obj->setProperty("refresh_token", refreshToken_);
        obj->setProperty("device_id", deviceId_);
        obj->setProperty("expires_at", accessTokenExpiresAt_.toMilliseconds());

        auto* u = new juce::DynamicObject();
        u->setProperty("id", profile_.id);
        u->setProperty("email", profile_.email);
        u->setProperty("handle", profile_.handle);
        u->setProperty("displayName", profile_.displayName);
        u->setProperty("about", profile_.about);
        u->setProperty("avatarUrl", profile_.avatarUrl);
        u->setProperty("locale", profile_.locale);
        u->setProperty("hideEmail", profile_.hideEmail);
        u->setProperty("plan", profile_.plan);
        u->setProperty("syncAppearance", profile_.syncAppearance);
        u->setProperty("deviceName", profile_.deviceName);
        obj->setProperty("user", juce::var(u));
    }

    const auto jsonStr = juce::JSON::toString(juce::var(obj));
    const auto enc = encryptData(jsonStr);

    const auto f = storageFile();
    f.replaceWithData(enc.getData(), enc.getSize());
    lastFileModTime_ = f.getLastModificationTime();
}

void AccountManager::clearDisk() {
    const auto f = storageFile();
    if (f.existsAsFile()) f.deleteFile();
    lastFileModTime_ = {};
}

void AccountManager::startDeviceFlow(const juce::String& pluginName) {
    {
        const std::lock_guard<std::mutex> lk(mutex_);
        pendingPluginName_ = pluginName;
        pendingAction_.store(Action::StartDeviceFlow, std::memory_order_relaxed);
    }
    setState(State::WaitingForCode);
    actionEvent_.signal();
}

void AccountManager::cancelDeviceFlow() {
    pendingAction_.store(Action::None, std::memory_order_relaxed);
    actionEvent_.signal();
    setState(State::SignedOut);
}

void AccountManager::signOut() {
    pendingAction_.store(Action::SignOut, std::memory_order_relaxed);
    actionEvent_.signal();
}

void AccountManager::refreshProfile() {
    pendingAction_.store(Action::FetchProfile, std::memory_order_relaxed);
    actionEvent_.signal();
}

void AccountManager::updateProfile(const juce::String& displayName, const juce::String& handle, const juce::String& about) {
    {
        const std::lock_guard<std::mutex> lk(mutex_);
        pendingDisplayName_ = displayName;
        pendingHandle_ = handle;
        pendingAbout_ = about;
        pendingAction_.store(Action::UpdateProfile, std::memory_order_relaxed);
    }
    actionEvent_.signal();
}

void AccountManager::uploadAvatar(const juce::File& file) {
    {
        const std::lock_guard<std::mutex> lk(mutex_);
        pendingAvatarFile_ = file;
        pendingAction_.store(Action::UploadAvatar, std::memory_order_relaxed);
    }
    actionEvent_.signal();
}

void AccountManager::removeAvatar() {
    pendingAction_.store(Action::RemoveAvatar, std::memory_order_relaxed);
    actionEvent_.signal();
}

void AccountManager::setHideEmail(bool hide) {
    {
        const std::lock_guard<std::mutex> lk(mutex_);
        profile_.hideEmail = hide;
    }
    saveToDisk();
    notifyProfileChanged();
    // Also patch remote
    juce::DynamicObject::Ptr body = new juce::DynamicObject();
    body->setProperty("hideEmail", hide);
    int status = 0;
    juce::var res;
    juce::String token;
    {
        const std::lock_guard<std::mutex> lk(mutex_);
        token = accessToken_;
    }
    httpPatch("/api/v1/me", juce::var(body.get()), res, status, token);
}

void AccountManager::setSyncAppearance(bool sync) {
    {
        const std::lock_guard<std::mutex> lk(mutex_);
        profile_.syncAppearance = sync;
    }
    saveToDisk();
    notifyProfileChanged();
}

void AccountManager::setDeviceName(const juce::String& name) {
    {
        const std::lock_guard<std::mutex> lk(mutex_);
        pendingDeviceName_ = name;
        pendingAction_.store(Action::UpdateDeviceName, std::memory_order_relaxed);
    }
    actionEvent_.signal();
}

juce::String AccountManager::getMaskedEmail() const {
    const std::lock_guard<std::mutex> lk(mutex_);
    if (profile_.email.isEmpty()) return {};
    if (!profile_.hideEmail) return profile_.email;

    const int at = profile_.email.indexOfChar('@');
    if (at <= 1) return profile_.email;

    return profile_.email.substring(0, 1) + juce::String::fromUTF8("\xe2\x80\xa2\xe2\x80\xa2\xe2\x80\xa2") + profile_.email.substring(at);
}

int AccountManager::getCodeRemainingSeconds() const {
    const std::lock_guard<std::mutex> lk(mutex_);
    if (deviceCode_.expired) return 0;
    const auto rem = deviceCode_.expiresAt - juce::Time::getCurrentTime();
    return juce::jmax(0, static_cast<int>(rem.inSeconds()));
}

void AccountManager::setState(State s) {
    const auto prev = state_.exchange(s, std::memory_order_relaxed);
    if (prev != s) {
        notifyStateChanged();
    }
}

void AccountManager::notifyStateChanged() {
    const auto s = state();
    juce::MessageManager::callAsync([this, s] {
        listeners_.call([s](Listener& l) { l.accountStateChanged(s); });
    });
}

void AccountManager::notifyProfileChanged() {
    Profile p;
    {
        const std::lock_guard<std::mutex> lk(mutex_);
        p = profile_;
    }
    juce::MessageManager::callAsync([this, p] {
        listeners_.call([&p](Listener& l) { l.accountProfileChanged(p); });
    });
}

void AccountManager::notifyDeviceCodeUpdated() {
    DeviceCode dc;
    {
        const std::lock_guard<std::mutex> lk(mutex_);
        dc = deviceCode_;
    }
    juce::MessageManager::callAsync([this, dc] {
        listeners_.call([&dc](Listener& l) { l.accountDeviceCodeUpdated(dc); });
    });
}

void AccountManager::timerCallback() {
    // 1. File watch: did another plug-in log in or out?
    const auto f = storageFile();
    if (f.existsAsFile()) {
        const auto mod = f.getLastModificationTime();
        if (mod != lastFileModTime_) {
            loadFromDisk();
            notifyProfileChanged();
        }
    } else if (isSignedIn()) {
        // file removed externally
        loadFromDisk();
    }

    // 2. Countdown tick for device code
    if (state() == State::WaitingForCode) {
        const int rem = getCodeRemainingSeconds();
        if (rem <= 0) {
            bool wasExpired = false;
            {
                const std::lock_guard<std::mutex> lk(mutex_);
                wasExpired = deviceCode_.expired;
                deviceCode_.expired = true;
            }
            if (!wasExpired) notifyDeviceCodeUpdated();
        } else {
            notifyDeviceCodeUpdated();
        }
    }
}

// ---- HTTP Helpers (called from background thread only) -------------------------------------

bool AccountManager::httpPost(const juce::String& endpoint, const juce::var& body, juce::var& responseJson, int& statusCode, const juce::String& bearerToken) {
    juce::String base;
    {
        const std::lock_guard<std::mutex> lk(mutex_);
        base = serverBase_;
    }

    juce::URL url(base + endpoint);
    const auto jsonStr = juce::JSON::toString(body);
    juce::WebInputStream ws(url.withPOSTData(jsonStr), true);
    juce::String headers = "Content-Type: application/json";
    if (bearerToken.isNotEmpty()) headers += "\r\nAuthorization: Bearer " + bearerToken;

    ws.withExtraHeaders(headers).withConnectionTimeout(10000).withNumRedirectsToFollow(2);
    if (!ws.connect(nullptr)) {
        statusCode = 0;
        return false;
    }

    statusCode = ws.getStatusCode();
    const auto respStr = ws.readEntireStreamAsString();
    responseJson = juce::JSON::parse(respStr);
    return statusCode >= 200 && statusCode < 300;
}

bool AccountManager::httpGet(const juce::String& endpoint, juce::var& responseJson, int& statusCode, const juce::String& bearerToken) {
    juce::String base;
    {
        const std::lock_guard<std::mutex> lk(mutex_);
        base = serverBase_;
    }

    juce::URL url(base + endpoint);
    juce::WebInputStream ws(url, false);
    juce::String headers;
    if (bearerToken.isNotEmpty()) headers = "Authorization: Bearer " + bearerToken;

    ws.withExtraHeaders(headers).withConnectionTimeout(10000).withNumRedirectsToFollow(2);
    if (!ws.connect(nullptr)) {
        statusCode = 0;
        return false;
    }

    statusCode = ws.getStatusCode();
    const auto respStr = ws.readEntireStreamAsString();
    responseJson = juce::JSON::parse(respStr);
    return statusCode >= 200 && statusCode < 300;
}

bool AccountManager::httpPatch(const juce::String& endpoint, const juce::var& body, juce::var& responseJson, int& statusCode, const juce::String& bearerToken) {
    juce::String base;
    {
        const std::lock_guard<std::mutex> lk(mutex_);
        base = serverBase_;
    }

    juce::URL url(base + endpoint);
    const auto jsonStr = juce::JSON::toString(body);
    juce::WebInputStream ws(url.withPOSTData(jsonStr), true);
    ws.withCustomRequestCommand("PATCH");
    juce::String headers = "Content-Type: application/json";
    if (bearerToken.isNotEmpty()) headers += "\r\nAuthorization: Bearer " + bearerToken;

    ws.withExtraHeaders(headers).withConnectionTimeout(10000).withNumRedirectsToFollow(2);
    if (!ws.connect(nullptr)) {
        statusCode = 0;
        return false;
    }

    statusCode = ws.getStatusCode();
    const auto respStr = ws.readEntireStreamAsString();
    responseJson = juce::JSON::parse(respStr);
    return statusCode >= 200 && statusCode < 300;
}

bool AccountManager::httpDelete(const juce::String& endpoint, juce::var& responseJson, int& statusCode, const juce::String& bearerToken) {
    juce::String base;
    {
        const std::lock_guard<std::mutex> lk(mutex_);
        base = serverBase_;
    }

    juce::URL url(base + endpoint);
    juce::WebInputStream ws(url, false);
    ws.withCustomRequestCommand("DELETE");
    juce::String headers;
    if (bearerToken.isNotEmpty()) headers = "Authorization: Bearer " + bearerToken;

    ws.withExtraHeaders(headers).withConnectionTimeout(10000).withNumRedirectsToFollow(2);
    if (!ws.connect(nullptr)) {
        statusCode = 0;
        return false;
    }

    statusCode = ws.getStatusCode();
    const auto respStr = ws.readEntireStreamAsString();
    responseJson = juce::JSON::parse(respStr);
    return statusCode >= 200 && statusCode < 300;
}

bool AccountManager::uploadAvatarImage(const juce::File& file, juce::var& responseJson, int& statusCode, const juce::String& bearerToken) {
    juce::String base;
    {
        const std::lock_guard<std::mutex> lk(mutex_);
        base = serverBase_;
    }

    juce::MemoryBlock data;
    if (!file.loadFileAsData(data) || data.isEmpty()) {
        statusCode = 400;
        return false;
    }

    juce::URL url(base + "/api/v1/me/avatar");
    url = url.withFileToUpload("photo", file, "image/jpeg");

    juce::String extraHeaders;
    if (bearerToken.isNotEmpty()) extraHeaders = "Authorization: Bearer " + bearerToken;

    juce::String pairMap;
    auto in = url.createInputStream(juce::URL::InputStreamOptions(juce::URL::ParameterHandling::inPostData)
                                        .withExtraHeaders(extraHeaders)
                                        .withConnectionTimeoutMs(15000));
    if (in == nullptr) {
        statusCode = 0;
        return false;
    }

    const auto respStr = in->readEntireStreamAsString();
    responseJson = juce::JSON::parse(respStr);
    statusCode = 200;
    return true;
}

void AccountManager::parseProfileJson(const juce::var& json) {
    if (!json.isObject()) return;
    const auto* o = json.getDynamicObject();
    if (o == nullptr) return;

    profile_.id = o->getProperty("id").toString();
    profile_.email = o->getProperty("email").toString();
    profile_.handle = o->getProperty("handle").toString();
    profile_.displayName = o->getProperty("displayName").toString();
    profile_.about = o->getProperty("about").toString();
    profile_.avatarUrl = o->getProperty("avatarUrl").toString();
    profile_.locale = o->getProperty("locale").toString();
    profile_.hideEmail = bool(o->getProperty("hideEmail"));
    profile_.plan = o->getProperty("plan").toString();

    if (o->hasProperty("syncAppearance")) profile_.syncAppearance = bool(o->getProperty("syncAppearance"));
    if (o->hasProperty("deviceName")) profile_.deviceName = o->getProperty("deviceName").toString();
}

// ---- Background Worker Thread --------------------------------------------------------------

void AccountManager::run() {
    while (!threadShouldExit()) {
        Action a = pendingAction_.exchange(Action::None, std::memory_order_relaxed);
        switch (a) {
            case Action::StartDeviceFlow:
                doStartDeviceFlow();
                break;
            case Action::PollToken:
                doPollDeviceCode();
                break;
            case Action::FetchProfile:
                doFetchProfile();
                break;
            case Action::UpdateProfile:
                doUpdateProfile();
                break;
            case Action::UploadAvatar:
                doUploadAvatar();
                break;
            case Action::RemoveAvatar:
                doRemoveAvatar();
                break;
            case Action::UpdateDeviceName:
                doUpdateDeviceName();
                break;
            case Action::SignOut:
                doSignOut();
                break;
            case Action::None:
            default:
                actionEvent_.wait(1000);
                break;
        }
    }
}

void AccountManager::doStartDeviceFlow() {
    juce::String pluginName;
    {
        const std::lock_guard<std::mutex> lk(mutex_);
        pluginName = pendingPluginName_.isNotEmpty() ? pendingPluginName_ : "HEARASIDE";
    }

    juce::DynamicObject::Ptr clientInfo = new juce::DynamicObject();
    clientInfo->setProperty("plugin", pluginName);
    clientInfo->setProperty("version", HEARASIDE_VERSION);
    clientInfo->setProperty("os", juce::SystemStats::getOperatingSystemName());
    clientInfo->setProperty("daw", juce::PluginHostType().getHostDescription());
    clientInfo->setProperty("computer", juce::SystemStats::getComputerName());

    juce::DynamicObject::Ptr req = new juce::DynamicObject();
    req->setProperty("client_info", juce::var(clientInfo.get()));

    juce::var res;
    int status = 0;
    if (!httpPost("/api/v1/device/start", juce::var(req.get()), res, status)) {
        if (status == 0) {
            // Network unreachable
            setState(State::Offline);
        } else {
            setState(State::SignedOut);
        }
        return;
    }

    if (const auto* o = res.getDynamicObject()) {
        {
            const std::lock_guard<std::mutex> lk(mutex_);
            deviceCode_.deviceCode = o->getProperty("device_code").toString();
            deviceCode_.userCode = o->getProperty("user_code").toString();
            deviceCode_.verificationUri = o->getProperty("verification_uri").toString();
            deviceCode_.verificationUriComplete = o->getProperty("verification_uri_complete").toString();
            deviceCode_.expiresInSec = int(o->getProperty("expires_in"));
            deviceCode_.intervalSec = juce::jmax(2, int(o->getProperty("interval")));
            deviceCode_.expiresAt = juce::Time::getCurrentTime() + juce::RelativeTime::seconds(double(deviceCode_.expiresInSec));
            deviceCode_.expired = false;
        }

        setState(State::WaitingForCode);
        notifyDeviceCodeUpdated();

        // Queue poll loop
        pendingAction_.store(Action::PollToken, std::memory_order_relaxed);
        actionEvent_.signal();
    }
}

void AccountManager::doPollDeviceCode() {
    juce::String devCode;
    int intervalSec = 5;
    {
        const std::lock_guard<std::mutex> lk(mutex_);
        devCode = deviceCode_.deviceCode;
        intervalSec = deviceCode_.intervalSec;
    }

    while (!threadShouldExit() && state() == State::WaitingForCode) {
        if (actionEvent_.wait(intervalSec * 1000)) {
            // Interrupted by cancel or another action
            return;
        }

        if (state() != State::WaitingForCode || devCode.isEmpty()) return;

        juce::DynamicObject::Ptr req = new juce::DynamicObject();
        req->setProperty("device_code", devCode);

        juce::var res;
        int status = 0;
        httpPost("/api/v1/device/token", juce::var(req.get()), res, status);

        if (status == 200 && res.isObject()) {
            const auto* o = res.getDynamicObject();
            if (o != nullptr) {
                {
                    const std::lock_guard<std::mutex> lk(mutex_);
                    accessToken_ = o->getProperty("access_token").toString();
                    refreshToken_ = o->getProperty("refresh_token").toString();
                    deviceId_ = o->getProperty("device_id").toString();
                    const int ttl = int(o->getProperty("expires_in"));
                    accessTokenExpiresAt_ = juce::Time::getCurrentTime() + juce::RelativeTime::seconds(double(ttl));

                    const auto userVal = o->getProperty("user");
                    if (userVal.isObject()) parseProfileJson(userVal);
                }

                saveToDisk();
                setState(State::SignedIn);
                notifyProfileChanged();
                return;
            }
        } else if (status == 400 && res.isObject()) {
            const auto err = res["error"].toString();
            if (err == "authorization_pending") {
                continue;
            } else if (err == "slow_down") {
                intervalSec += 5;
                continue;
            } else if (err == "access_denied") {
                setState(State::SignedOut);
                return;
            } else if (err == "expired_token") {
                {
                    const std::lock_guard<std::mutex> lk(mutex_);
                    deviceCode_.expired = true;
                }
                notifyDeviceCodeUpdated();
                return;
            }
        } else if (status == 0) {
            // Server offline / can't reach
            continue;
        }
    }
}

void AccountManager::doRefreshToken() {
    juce::String ref;
    {
        const std::lock_guard<std::mutex> lk(mutex_);
        ref = refreshToken_;
    }
    if (ref.isEmpty()) return;

    juce::DynamicObject::Ptr req = new juce::DynamicObject();
    req->setProperty("refresh_token", ref);

    juce::var res;
    int status = 0;
    if (httpPost("/api/v1/token/refresh", juce::var(req.get()), res, status)) {
        if (const auto* o = res.getDynamicObject()) {
            const std::lock_guard<std::mutex> lk(mutex_);
            accessToken_ = o->getProperty("access_token").toString();
            refreshToken_ = o->getProperty("refresh_token").toString();
            const int ttl = int(o->getProperty("expires_in"));
            accessTokenExpiresAt_ = juce::Time::getCurrentTime() + juce::RelativeTime::seconds(double(ttl));
            saveToDisk();
        }
    } else if (status == 401) {
        // Token revoked
        doSignOut();
    }
}

void AccountManager::doFetchProfile() {
    juce::String token;
    {
        const std::lock_guard<std::mutex> lk(mutex_);
        token = accessToken_;
    }
    if (token.isEmpty()) return;

    juce::var res;
    int status = 0;
    if (!httpGet("/api/v1/me", res, status, token)) {
        if (status == 401) {
            doRefreshToken();
            {
                const std::lock_guard<std::mutex> lk(mutex_);
                token = accessToken_;
            }
            if (!httpGet("/api/v1/me", res, status, token)) return;
        } else if (status == 0) {
            setState(State::Offline);
            return;
        }
    }

    if (status == 200 && res.isObject()) {
        {
            const std::lock_guard<std::mutex> lk(mutex_);
            parseProfileJson(res);
        }
        saveToDisk();
        setState(State::SignedIn);
        notifyProfileChanged();
    }
}

void AccountManager::doUpdateProfile() {
    juce::String token, name, handle, about;
    {
        const std::lock_guard<std::mutex> lk(mutex_);
        token = accessToken_;
        name = pendingDisplayName_;
        handle = pendingHandle_;
        about = pendingAbout_;
    }
    if (token.isEmpty()) return;

    juce::DynamicObject::Ptr req = new juce::DynamicObject();
    if (name.isNotEmpty()) req->setProperty("displayName", name);
    if (handle.isNotEmpty()) req->setProperty("handle", handle);
    if (about.isNotEmpty()) req->setProperty("about", about);

    juce::var res;
    int status = 0;
    if (httpPatch("/api/v1/me", juce::var(req.get()), res, status, token)) {
        if (res.isObject()) {
            {
                const std::lock_guard<std::mutex> lk(mutex_);
                parseProfileJson(res);
            }
            saveToDisk();
            notifyProfileChanged();
        }
    }
}

void AccountManager::doUploadAvatar() {
    juce::File f;
    juce::String token;
    {
        const std::lock_guard<std::mutex> lk(mutex_);
        f = pendingAvatarFile_;
        token = accessToken_;
    }
    if (!f.existsAsFile() || token.isEmpty()) return;

    juce::var res;
    int status = 0;
    if (uploadAvatarImage(f, res, status, token)) {
        if (res.isObject()) {
            {
                const std::lock_guard<std::mutex> lk(mutex_);
                parseProfileJson(res);
            }
            saveToDisk();
            notifyProfileChanged();
        }
    }
}

void AccountManager::doRemoveAvatar() {
    juce::String token;
    {
        const std::lock_guard<std::mutex> lk(mutex_);
        token = accessToken_;
    }
    if (token.isEmpty()) return;

    juce::var res;
    int status = 0;
    if (httpDelete("/api/v1/me/avatar", res, status, token)) {
        if (res.isObject()) {
            {
                const std::lock_guard<std::mutex> lk(mutex_);
                parseProfileJson(res);
            }
            saveToDisk();
            notifyProfileChanged();
        }
    }
}

void AccountManager::doUpdateDeviceName() {
    juce::String token, devName;
    {
        const std::lock_guard<std::mutex> lk(mutex_);
        token = accessToken_;
        devName = pendingDeviceName_;
    }
    if (token.isEmpty() || devName.isEmpty()) return;

    juce::DynamicObject::Ptr req = new juce::DynamicObject();
    req->setProperty("name", devName);

    juce::var res;
    int status = 0;
    if (httpPatch("/api/v1/device", juce::var(req.get()), res, status, token)) {
        {
            const std::lock_guard<std::mutex> lk(mutex_);
            profile_.deviceName = devName;
        }
        saveToDisk();
        notifyProfileChanged();
    }
}

void AccountManager::doSignOut() {
    juce::String token;
    {
        const std::lock_guard<std::mutex> lk(mutex_);
        token = accessToken_;
    }
    if (token.isNotEmpty()) {
        juce::var res;
        int status = 0;
        httpPost("/api/v1/device/revoke", {}, res, status, token);
    }

    {
        const std::lock_guard<std::mutex> lk(mutex_);
        accessToken_.clear();
        refreshToken_.clear();
        deviceId_.clear();
        profile_ = {};
        deviceCode_ = {};
    }

    clearDisk();
    setState(State::SignedOut);
    notifyProfileChanged();
}

bool AccountManager::testDpapiRoundtrip() {
#if JUCE_WINDOWS
    DATA_BLOB inBlob;
    char secret[] = "test_refresh_token_256bit_hearaside_secret";
    inBlob.pbData = reinterpret_cast<BYTE*>(secret);
    inBlob.cbData = static_cast<DWORD>(strlen(secret));
    DATA_BLOB outBlob;
    BOOL encOk = CryptProtectData(&inBlob, L"HearasideToken", nullptr, nullptr, nullptr, 0, &outBlob);
    if (!encOk) return false;
    if (memcmp(outBlob.pbData, secret, inBlob.cbData) == 0) {
        LocalFree(outBlob.pbData);
        return false;
    }
    DATA_BLOB decBlob;
    BOOL decOk = CryptUnprotectData(&outBlob, nullptr, nullptr, nullptr, nullptr, 0, &decBlob);
    if (!decOk) {
        LocalFree(outBlob.pbData);
        return false;
    }
    const bool match = (decBlob.cbData == inBlob.cbData && memcmp(decBlob.pbData, secret, inBlob.cbData) == 0);
    LocalFree(decBlob.pbData);
    LocalFree(outBlob.pbData);
    return match;
#else
    return true;
#endif
}

} // namespace hearaside
