#include "AccountManager.h"
#include "Settings.h"
#include "Strings.h"
#include "Host.h"

#if JUCE_WINDOWS
#include <windows.h>
#include <wincrypt.h>
#pragma comment(lib, "crypt32.lib")
#else
#include <sys/stat.h>
#endif

namespace hearaside {

namespace {
juce::String getEnv(const char* name, const juce::String& def = {}) {
    const auto val = juce::SystemStats::getEnvironmentVariable(name, {});
    return val.trim().isNotEmpty() ? val.trim() : def;
}

// The appearance the account syncs (web/site/shared/schemas.ts syncedSettings.appearance).
juce::var appearanceOf(const Settings& s) {
    auto* o = new juce::DynamicObject();
    o->setProperty("language", s.language() == Language::Thai ? "th" : "en");
    o->setProperty("theme", s.themeMode() == ThemeMode::Light ? "light" : s.themeMode() == ThemeMode::Dark ? "dark" : "system");
    o->setProperty("uiScale", double(s.uiScale()));
    o->setProperty("glassAlpha", double(s.glassAlpha()));
    o->setProperty("hostColours", s.hostColours());
    o->setProperty("reduceMotion", s.reduceMotion());
    return juce::var(o);
}

void applyAppearance(Settings& s, const juce::var& a) {
    if (a.hasProperty("language")) s.setLanguage(a["language"].toString() == "th" ? Language::Thai : Language::English);
    if (a.hasProperty("theme")) {
        const auto t = a["theme"].toString();
        s.setThemeMode(t == "light" ? ThemeMode::Light : t == "dark" ? ThemeMode::Dark : ThemeMode::Auto);
    }
    if (a.hasProperty("uiScale")) s.setUiScale(float(double(a["uiScale"])));
    if (a.hasProperty("glassAlpha")) s.setGlassAlpha(float(double(a["glassAlpha"])));
    if (a.hasProperty("hostColours")) s.setHostColours(bool(a["hostColours"]));
    if (a.hasProperty("reduceMotion")) s.setReduceMotion(bool(a["reduceMotion"]));
}

juce::int64 isoMs(const juce::var& v) {
    return v.isString() && v.toString().isNotEmpty() ? juce::Time::fromISO8601(v.toString()).toMilliseconds() : 0;
}
} // namespace

JUCE_IMPLEMENT_SINGLETON(AccountManager)

AccountManager& AccountManager::get() { return *getInstance(); }

AccountManager::AccountManager() : juce::Thread("AccountWorker") {
    serverBase_ = getEnv("HEARASIDE_API_URL", "https://hearaside.vercel.app");
    if (serverBase_.endsWithChar('/')) serverBase_ = serverBase_.dropLastCharacters(1);

    loadFromDisk();
    startThread(juce::Thread::Priority::normal);
    startTimerHz(1); // 1 Hz timer for file watch & countdown ticks
    Settings::appearanceChanged = [](const Settings& st) { get().localAppearanceChanged(appearanceOf(st)); };
}

AccountManager::~AccountManager() {
    clearSingletonInstance();
    Settings::appearanceChanged = nullptr;
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
    return dir.getChildFile(getEnv("HEARASIDE_ACCOUNT_NAME", "account") + ".dat");   // tests use their own file
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
    return {};   // never write the tokens unprotected: the caller keeps them in memory only
#else
    // no OS key store here yet; saveToDisk() makes the file readable by this user only
    juce::MemoryBlock mb;
    mb.append(text.toRawUTF8(), strlen(text.toRawUTF8()));
    return mb;
#endif
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
    return {};   // not ours or not this user's: treat as signed out, never as plain text
#else
    return juce::String::fromUTF8(static_cast<const char*>(block.getData()), static_cast<int>(block.getSize()));
#endif
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
    if (enc.isEmpty()) return;   // could not protect the tokens: do not store them

    const auto f = storageFile();
    f.replaceWithData(enc.getData(), enc.getSize());
#if !JUCE_WINDOWS
    chmod(f.getFullPathName().toRawUTF8(), S_IRUSR | S_IWUSR);
    chmod(f.getParentDirectory().getFullPathName().toRawUTF8(), S_IRWXU);
#endif
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
        deviceCode_ = {};   // no code yet: the panel says "Getting a code…" and nothing counts down
        pendingAction_.store(Action::StartDeviceFlow, std::memory_order_relaxed);
    }
    startFailed_.store(false);
    setState(State::WaitingForCode);
    notifyDeviceCodeUpdated();
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
    pendingHideEmail_.store(hide ? 1 : 0);   // the worker thread sends it: never wait on the network here
    actionEvent_.signal();
}

void AccountManager::setSyncAppearance(bool sync) {
    juce::var local;
    if (sync) { SharedSettings st; local = appearanceOf(*st); }
    {
        const std::lock_guard<std::mutex> lk(mutex_);
        profile_.syncAppearance = sync;
        if (sync) {
            // this computer's look goes up only when the account has none yet; otherwise the account's comes down
            localAppearance_ = local;
            localChangedAt_ = 0;
            localDirty_ = true;
            remoteSeenAt_ = 0;
        }
    }
    saveToDisk();
    notifyProfileChanged();
    lastPeriodicMs_ = 0;   // sync now
    actionEvent_.signal();
}

void AccountManager::localAppearanceChanged(const juce::var& appearance) {
    if (applyingRemote_.load()) return;   // our own copy of the account's appearance, not a change
    {
        const std::lock_guard<std::mutex> lk(mutex_);
        if (!profile_.syncAppearance) return;
        localAppearance_ = appearance;
        localChangedAt_ = juce::Time::currentTimeMillis();
        localDirty_ = true;
    }
    lastPeriodicMs_ = 0;
    actionEvent_.signal();
}

juce::String AccountManager::accessToken() const {
    const std::lock_guard<std::mutex> lk(mutex_);
    return accessTokenExpiresAt_ > juce::Time::getCurrentTime() + juce::RelativeTime::seconds(30) ? accessToken_ : juce::String();
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
    if (deviceCode_.userCode.isEmpty()) return deviceCode_.expiresInSec;   // still asking the server
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
    bool haveCode = false;
    {
        const std::lock_guard<std::mutex> lk(mutex_);
        haveCode = deviceCode_.userCode.isNotEmpty();
    }
    if (state() == State::WaitingForCode && haveCode) {
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
    const auto jsonStr = body.isVoid() ? juce::String("{}") : juce::JSON::toString(body);   // the server takes no bare null
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
    return httpWithBody("PATCH", endpoint, body, responseJson, statusCode, bearerToken);
}

bool AccountManager::httpPut(const juce::String& endpoint, const juce::var& body, juce::var& responseJson, int& statusCode, const juce::String& bearerToken) {
    return httpWithBody("PUT", endpoint, body, responseJson, statusCode, bearerToken);
}

bool AccountManager::httpWithBody(const char* method, const juce::String& endpoint, const juce::var& body, juce::var& responseJson, int& statusCode, const juce::String& bearerToken) {
    juce::String base;
    {
        const std::lock_guard<std::mutex> lk(mutex_);
        base = serverBase_;
    }

    juce::URL url(base + endpoint);
    const auto jsonStr = body.isVoid() ? juce::String("{}") : juce::JSON::toString(body);   // the server takes no bare null
    juce::WebInputStream ws(url.withPOSTData(jsonStr), true);
    ws.withCustomRequestCommand(method);
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
            default: {
                if (const int h = pendingHideEmail_.exchange(-1); h >= 0) doPatchHideEmail(h == 1);
                const auto now = juce::Time::getMillisecondCounter();
                if (isSignedIn() && (lastPeriodicMs_ == 0 || now - lastPeriodicMs_ > 60000)) {
                    lastPeriodicMs_ = juce::jmax(1u, now);
                    doPeriodic();
                } else {
                    actionEvent_.wait(1000);
                }
                break;
            }
        }
    }
}

// Every minute while signed in: a fresh access token before the old one runs out (permanent links
// carry it), the profile (a computer signed out on the website notices here), the appearance.
void AccountManager::doPeriodic() {
    doRefreshToken();
    doFetchProfile();
    doSyncAppearance();
}

void AccountManager::doPatchHideEmail(bool hide) {
    const auto token = accessToken();
    if (token.isEmpty()) return;
    juce::DynamicObject::Ptr body = new juce::DynamicObject();
    body->setProperty("hideEmail", hide);
    int status = 0;
    juce::var res;
    httpPatch("/api/v1/me", juce::var(body.get()), res, status, token);
}

// "Sync appearance": newer wins. Never Peak ceiling, Sync safety, Line-up limit or Bus name.
void AccountManager::doSyncAppearance() {
    juce::var local;
    juce::int64 localAt = 0, seen = 0;
    bool dirty = false;
    {
        const std::lock_guard<std::mutex> lk(mutex_);
        if (!profile_.syncAppearance) return;
        local = localAppearance_;
        localAt = localChangedAt_;
        dirty = localDirty_;
        seen = remoteSeenAt_;
    }
    const auto token = accessToken();
    if (token.isEmpty()) return;
    juce::var res;
    int status = 0;
    if (!httpGet("/api/v1/me/settings", res, status, token) || !res.isObject()) return;
    const auto remoteAt = isoMs(res["updatedAt"]);
    const auto remote = res["appearance"];
    const bool remoteHas = remote.getDynamicObject() != nullptr && remote.getDynamicObject()->getProperties().size() > 0;

    if (dirty && local.isObject() && (localAt > remoteAt || !remoteHas)) {
        juce::DynamicObject::Ptr body = new juce::DynamicObject();
        body->setProperty("appearance", local);
        body->setProperty("stemNames", res["stemNames"].isArray() ? res["stemNames"] : juce::var(juce::Array<juce::var>()));
        juce::var put;
        int st = 0;
        if (httpPut("/api/v1/me/settings", juce::var(body.get()), put, st, token)) {
            const std::lock_guard<std::mutex> lk(mutex_);
            if (localChangedAt_ == localAt) localDirty_ = false;   // no newer change meanwhile
            remoteSeenAt_ = isoMs(put["updatedAt"]);
        }
        return;
    }
    if (remoteHas && remoteAt > seen) {
        {
            const std::lock_guard<std::mutex> lk(mutex_);
            remoteSeenAt_ = remoteAt;
            localDirty_ = false;
        }
        juce::MessageManager::callAsync([this, remote] {
            applyingRemote_.store(true);
            SharedSettings st;
            applyAppearance(*st, remote);
            applyingRemote_.store(false);
        });
    }
}

void AccountManager::doStartDeviceFlow() {
    juce::String pluginName;
    {
        const std::lock_guard<std::mutex> lk(mutex_);
        pluginName = pendingPluginName_.toLowerCase();
    }
    // the server takes exactly hub / track / app (and short strings): anything else is a 400 and no code
    const juce::String plugin = pluginName.contains("hub") ? "hub" : pluginName.contains("app") ? "app" : "track";

    juce::DynamicObject::Ptr clientInfo = new juce::DynamicObject();
    clientInfo->setProperty("plugin", plugin);
    clientInfo->setProperty("version", juce::String(HEARASIDE_VERSION).substring(0, 20));
    clientInfo->setProperty("os", juce::SystemStats::getOperatingSystemName().substring(0, 40));
    clientInfo->setProperty("daw", juce::String(juce::PluginHostType().getHostDescription()).substring(0, 40));
    clientInfo->setProperty("computer", juce::SystemStats::getComputerName().substring(0, 64));

    juce::DynamicObject::Ptr req = new juce::DynamicObject();
    req->setProperty("client_info", juce::var(clientInfo.get()));

    juce::var res;
    int status = 0;
    if (!httpPost("/api/v1/device/start", juce::var(req.get()), res, status)) {
        // not signed in, whatever the reason: "Offline" would mean "signed in, can't reach the server"
        startFailed_.store(true);
        setState(State::SignedOut);
        notifyStateChanged();   // the panel shows why, even when the state was already SignedOut
        return;
    }
    if (state() != State::WaitingForCode) return;   // cancelled while asking
    const auto* o = res.getDynamicObject();
    if (o == nullptr || o->getProperty("user_code").toString().isEmpty()) {
        startFailed_.store(true);
        setState(State::SignedOut);
        notifyStateChanged();
        return;
    }
    startFailed_.store(false);

    {
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

        // poll next: run() picks it up straight away. No signal: the poll loop would take it for a cancel
        // and stop before asking once (the browser said "signed in", the plug-in kept waiting)
        pendingAction_.store(Action::PollToken, std::memory_order_relaxed);
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
            // woken: stop for a cancel or another action; any other wake-up (a setting changed) keeps polling
            if (pendingAction_.load(std::memory_order_relaxed) != Action::None || state() != State::WaitingForCode) return;
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

void AccountManager::doRefreshToken(const juce::String& failed) {
    // Every DAW (process) with a HEARASIDE plug-in shares account.dat. A refresh token works once:
    // one process at a time, and only when the token on disk still needs it (another one may have
    // just refreshed), or the server takes the reuse for a stolen token and signs this computer out.
    juce::InterProcessLock lock("HEARASIDE_account_refresh");
    const juce::InterProcessLock::ScopedLockType held(lock);
    if (storageFile().getLastModificationTime() != lastFileModTime_) loadFromDisk();
    juce::String ref;
    {
        const std::lock_guard<std::mutex> lk(mutex_);
        const bool fresh = accessTokenExpiresAt_ > juce::Time::getCurrentTime() + juce::RelativeTime::minutes(2);
        if (accessToken_.isNotEmpty() && accessToken_ != failed && fresh) return;
        ref = refreshToken_;
    }
    if (ref.isEmpty()) return;

    juce::DynamicObject::Ptr req = new juce::DynamicObject();
    req->setProperty("refresh_token", ref);

    juce::var res;
    int status = 0;
    if (httpPost("/api/v1/token/refresh", juce::var(req.get()), res, status)) {
        if (const auto* o = res.getDynamicObject()) {
            {
                const std::lock_guard<std::mutex> lk(mutex_);
                accessToken_ = o->getProperty("access_token").toString();
                refreshToken_ = o->getProperty("refresh_token").toString();
                const int ttl = int(o->getProperty("expires_in"));
                accessTokenExpiresAt_ = juce::Time::getCurrentTime() + juce::RelativeTime::seconds(double(ttl));
            }
            saveToDisk();   // outside mutex_: saveToDisk takes it (holding it here deadlocked the worker)
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
            doRefreshToken(token);
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
