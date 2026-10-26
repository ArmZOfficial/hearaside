#if defined(_WIN32)
 #ifndef NOMINMAX
  #define NOMINMAX
 #endif
 #define WIN32_LEAN_AND_MEAN
 #include <windows.h>
 #include <objbase.h>
 #pragma comment(lib, "ole32.lib")
 #pragma comment(lib, "advapi32.lib")
#endif

#include "HasAsio.h"

#include <atomic>
#include <cmath>
#include <cstring>
#include <vector>

namespace hearaside::karaoke::asio {

int bytesPerSample(long type) noexcept {
    switch (type) {
        case Int16LSB: return 2;
        case Int24LSB: return 3;
        case Int32LSB: case Float32LSB: case Int32LSB16: case Int32LSB18: case Int32LSB20: case Int32LSB24: return 4;
        case Float64LSB: return 8;
        default: return 0;
    }
}

int bitDepth(long type) noexcept {
    switch (type) {
        case Int16LSB: case Int32LSB16: return 16;
        case Int32LSB18: return 18;
        case Int32LSB20: return 20;
        case Int24LSB: case Int32LSB24: return 24;
        case Int32LSB: case Float32LSB: return 32;
        case Float64LSB: return 64;
        default: return 0;
    }
}

// full scale of the integer types (the data sits in the low bits of the 32-bit container for 16/18/20/24)
static double fullScale(long type) noexcept {
    switch (type) {
        case Int16LSB: case Int32LSB16: return 32768.0;
        case Int32LSB18: return 131072.0;
        case Int32LSB20: return 524288.0;
        case Int24LSB: case Int32LSB24: return 8388608.0;
        case Int32LSB: return 2147483648.0;
        default: return 1.0;
    }
}

bool toFloat(long type, const void* src, float* dst, int n) noexcept {
    const auto* b = static_cast<const unsigned char*>(src);
    const float k = float(1.0 / fullScale(type));
    switch (type) {
        case Int16LSB:
            for (int i = 0; i < n; ++i) { int16_t v; std::memcpy(&v, b + 2 * i, 2); dst[i] = float(v) * k; }
            return true;
        case Int24LSB:
            for (int i = 0; i < n; ++i) {
                const unsigned char* p = b + 3 * i;
                const int32_t v = int32_t(uint32_t(p[0]) | (uint32_t(p[1]) << 8) | (uint32_t(p[2]) << 16) | (p[2] & 0x80 ? 0xff000000u : 0u));
                dst[i] = float(v) * k;
            }
            return true;
        case Int32LSB: case Int32LSB16: case Int32LSB18: case Int32LSB20: case Int32LSB24:
            for (int i = 0; i < n; ++i) { int32_t v; std::memcpy(&v, b + 4 * i, 4); dst[i] = float(double(v) / fullScale(type)); }
            return true;
        case Float32LSB:
            std::memcpy(dst, b, size_t(n) * 4);
            return true;
        case Float64LSB:
            for (int i = 0; i < n; ++i) { double v; std::memcpy(&v, b + 8 * i, 8); dst[i] = float(v); }
            return true;
        default:
            return false;
    }
}

bool fromFloat(long type, const float* src, void* dst, int n) noexcept {
    auto* b = static_cast<unsigned char*>(dst);
    const double fs = fullScale(type);
    auto toInt = [fs](float x) {   // clamp, then round to the integer range of the type
        const double c = std::fmin(std::fmax(double(x), -1.0), 1.0);
        return int64_t(std::llround(c * (fs - 1.0)));
    };
    switch (type) {
        case Int16LSB:
            for (int i = 0; i < n; ++i) { const int16_t v = int16_t(toInt(src[i])); std::memcpy(b + 2 * i, &v, 2); }
            return true;
        case Int24LSB:
            for (int i = 0; i < n; ++i) {
                const uint32_t v = uint32_t(int32_t(toInt(src[i])));
                unsigned char* p = b + 3 * i;
                p[0] = uint8_t(v); p[1] = uint8_t(v >> 8); p[2] = uint8_t(v >> 16);
            }
            return true;
        case Int32LSB: case Int32LSB16: case Int32LSB18: case Int32LSB20: case Int32LSB24:
            for (int i = 0; i < n; ++i) { const int32_t v = int32_t(toInt(src[i])); std::memcpy(b + 4 * i, &v, 4); }
            return true;
        case Float32LSB:   // clipped like the integer types: an over never reaches the interface above full scale
            for (int i = 0; i < n; ++i) { const float v = std::fmin(std::fmax(src[i], -1.0f), 1.0f); std::memcpy(b + 4 * i, &v, 4); }
            return true;
        case Float64LSB:
            for (int i = 0; i < n; ++i) { const double v = std::fmin(std::fmax(double(src[i]), -1.0), 1.0); std::memcpy(b + 8 * i, &v, 8); }
            return true;
        default:
            return false;
    }
}

#if defined(_WIN32)
static_assert(sizeof(void*) == 8, "the driver interface below is the 64-bit one");
static_assert(sizeof(long) == 4, "ASIO's long is 32-bit");

// --- the driver ABI ---------------------------------------------------------------------------------
#pragma pack(push, 4)
struct ClockSource { long index, associatedChannel, associatedGroup, isCurrentSource; char name[32]; };
struct ChannelInfo { long channel, isInput, isActive, channelGroup, type; char name[32]; };
struct BufferInfo  { long isInput, channelNum; void* buffers[2]; };
struct Position    { unsigned long hi, lo; };
struct Callbacks {
    void  (*bufferSwitch)(long index, long processNow);
    void  (*sampleRateDidChange)(double rate);
    long  (*message)(long selector, long value, void* message, double* opt);
    void* (*bufferSwitchTimeInfo)(void* params, long index, long processNow);
};
#pragma pack(pop)

struct Driver : public IUnknown {
    virtual long init(void* sysHandle) = 0;
    virtual void getDriverName(char* name) = 0;
    virtual long getDriverVersion() = 0;
    virtual void getErrorMessage(char* text) = 0;
    virtual long start() = 0;
    virtual long stop() = 0;
    virtual long getChannels(long* inputs, long* outputs) = 0;
    virtual long getLatencies(long* input, long* output) = 0;
    virtual long getBufferSize(long* minSize, long* maxSize, long* preferred, long* granularity) = 0;
    virtual long canSampleRate(double rate) = 0;
    virtual long getSampleRate(double* rate) = 0;
    virtual long setSampleRate(double rate) = 0;
    virtual long getClockSources(ClockSource* clocks, long* count) = 0;
    virtual long setClockSource(long index) = 0;
    virtual long getSamplePosition(Position* samples, Position* timeStamp) = 0;
    virtual long getChannelInfo(ChannelInfo* info) = 0;
    virtual long createBuffers(BufferInfo* infos, long count, long bufferSize, Callbacks* callbacks) = 0;
    virtual long disposeBuffers() = 0;
    virtual long controlPanel() = 0;
    virtual long future(long selector, void* opt) = 0;
    virtual long outputReady() = 0;
};

// the name people see: our own, the drivers keep the names their makers gave them
static const char* const kTypeName = "HAS Direct";

static bool ok(long e) noexcept { return e == 0 || e == 0x3f4847a0; }   // OK / SUCCESS

enum : long { kSelectorSupported = 1, kEngineVersion = 2, kResetRequest = 3, kBufferSizeChange = 4,
              kResyncRequest = 5, kLatenciesChanged = 6 };

// --- one device = one driver --------------------------------------------------------------------------
class Device;
static std::atomic<Device*> gActive { nullptr };   // ASIO callbacks carry no context: one running driver at a time

class Device final : public juce::AudioIODevice {
public:
    Device(const juce::String& name, const CLSID& clsid) : AudioIODevice(name, kTypeName), clsid_(clsid) {}   // the driver loads on first real use (see ensureLoaded)

    ~Device() override {
        *alive_ = false;
        close();
        if (drv_ != nullptr) drv_->Release();
    }

    juce::StringArray getOutputChannelNames() override { ensureLoaded(); return outNames_; }
    juce::StringArray getInputChannelNames() override { ensureLoaded(); return inNames_; }

    juce::Array<double> getAvailableSampleRates() override {
        juce::Array<double> r;
        if (!loaded_) return { 44100.0, 48000.0, 88200.0, 96000.0 };   // AudioDeviceManager asks every driver in the list: don't load them all
        if (drv_ != nullptr)
            for (double s : { 44100.0, 48000.0, 88200.0, 96000.0, 176400.0, 192000.0 })
                if (ok(drv_->canSampleRate(s))) r.add(s);
        if (r.isEmpty() && rate_ > 0) r.add(rate_);
        return r;
    }

    juce::Array<int> getAvailableBufferSizes() override {
        ensureLoaded();
        juce::Array<int> r;
        if (gran_ == -1) {
            for (long s = juce::jmax(1L, minBuf_); s <= maxBuf_ && r.size() < 32; s *= 2) r.add(int(s));
        } else if (gran_ > 0) {
            for (long s = minBuf_; s <= maxBuf_ && r.size() < 64; s += gran_) r.add(int(s));
        }
        if (!r.contains(int(prefBuf_)) && prefBuf_ > 0) r.addUsingDefaultSort(int(prefBuf_));
        return r;
    }

    int getDefaultBufferSize() override { ensureLoaded(); return int(prefBuf_ > 0 ? prefBuf_ : 512); }

    juce::String open(const juce::BigInteger& inputs, const juce::BigInteger& outputs, double rate, int bufferSize) override {
        close();
        error_.clear();
        ensureLoaded();
        if (drv_ == nullptr) return error_ = loadError_.isNotEmpty() ? loadError_ : "The interface driver could not be loaded";
        lastIn_ = inputs; lastOut_ = outputs; lastBuf_ = bufferSize;

        double cur = 0.0;
        drv_->getSampleRate(&cur);
        if (rate > 0 && std::abs(cur - rate) > 0.5) {
            if (!ok(drv_->canSampleRate(rate)) || !ok(drv_->setSampleRate(rate)))
                return error_ = "The driver can't run at " + juce::String(rate, 0) + " Hz";
        }
        drv_->getSampleRate(&rate_);
        drv_->getBufferSize(&minBuf_, &maxBuf_, &prefBuf_, &gran_);
        bufSize_ = bufferSize > 0 ? juce::jlimit(int(minBuf_), int(maxBuf_), bufferSize) : int(prefBuf_);

        activeIn_.clear(); activeOut_.clear();
        infos_.clear(); types_.clear();
        for (int c = 0; c < inNames_.size(); ++c)
            if (inputs[c]) { activeIn_.setBit(c); infos_.push_back({ 1, c, { nullptr, nullptr } }); }
        for (int c = 0; c < outNames_.size(); ++c)
            if (outputs[c]) { activeOut_.setBit(c); infos_.push_back({ 0, c, { nullptr, nullptr } }); }
        if (infos_.empty()) return error_ = "No channels are switched on";

        static Callbacks cbs { &onBufferSwitch, &onRateChange, &onMessage, &onBufferSwitchTimeInfo };
        Device* expected = nullptr;
        if (!gActive.compare_exchange_strong(expected, this) && expected != this)
            return error_ = "Another interface driver is already running";
        if (!ok(drv_->createBuffers(infos_.data(), long(infos_.size()), long(bufSize_), &cbs))) {
            gActive = nullptr;
            return error_ = "The driver couldn't make its buffers (" + juce::String(bufSize_) + " samples)";
        }
        for (const auto& b : infos_) {
            ChannelInfo ci {};
            ci.channel = b.channelNum;
            ci.isInput = b.isInput;
            drv_->getChannelInfo(&ci);
            if (bytesPerSample(ci.type) == 0) {
                drv_->disposeBuffers();
                gActive = nullptr;
                return error_ = "Unsupported sample format from the driver (" + juce::String(ci.type) + ")";
            }
            types_.push_back(ci.type);
        }
        drv_->getLatencies(&inLat_, &outLat_);
        inBuf_.setSize(juce::jmax(1, activeIn_.countNumberOfSetBits()), bufSize_);
        outBuf_.setSize(juce::jmax(1, activeOut_.countNumberOfSetBits()), bufSize_);
        inBuf_.clear(); outBuf_.clear();
        postOutput_ = ok(drv_->outputReady());
        if (!ok(drv_->start())) {
            drv_->disposeBuffers();
            gActive = nullptr;
            return error_ = "The driver didn't start";
        }
        open_ = true;
        return {};
    }

    void close() override {
        if (!open_) return;
        stop();
        drv_->stop();
        drv_->disposeBuffers();
        Device* self = this;
        gActive.compare_exchange_strong(self, nullptr);
        open_ = false;
    }

    bool isOpen() override { return open_; }

    void start(juce::AudioIODeviceCallback* cb) override {
        if (!open_ || cb == nullptr) return;
        cb->audioDeviceAboutToStart(this);
        const juce::ScopedLock sl(lock_);
        cb_ = cb;
    }

    void stop() override {
        juce::AudioIODeviceCallback* old = nullptr;
        {
            const juce::ScopedLock sl(lock_);
            old = cb_;
            cb_ = nullptr;
        }
        if (old != nullptr) old->audioDeviceStopped();
    }

    bool isPlaying() override { return cb_ != nullptr; }
    juce::String getLastError() override { return error_; }
    int getCurrentBufferSizeSamples() override { return bufSize_; }
    double getCurrentSampleRate() override { return rate_; }
    int getCurrentBitDepth() override { return types_.empty() ? 32 : bitDepth(types_.front()); }
    juce::BigInteger getActiveOutputChannels() const override { return activeOut_; }
    juce::BigInteger getActiveInputChannels() const override { return activeIn_; }
    int getOutputLatencyInSamples() override { return int(outLat_); }
    int getInputLatencyInSamples() override { return int(inLat_); }
    bool hasControlPanel() const override { return true; }
    bool showControlPanel() override {   // the driver asks for a reset itself if something changed
        ensureLoaded();
        if (drv_ != nullptr) drv_->controlPanel();
        return false;
    }

private:
    void ensureLoaded() {
        if (!loaded_) { loaded_ = true; load(); }
    }

    void load() {
        CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);   // the message thread; S_FALSE / RPC_E_CHANGED_MODE are fine
        void* p = nullptr;
        // an ASIO driver takes its own CLSID as the interface id
        if (FAILED(CoCreateInstance(clsid_, nullptr, CLSCTX_INPROC_SERVER, clsid_, &p)) || p == nullptr) {
            loadError_ = "The interface driver isn't installed properly";
            return;
        }
        drv_ = static_cast<Driver*>(p);
        if (drv_->init(GetActiveWindow()) == 0) {   // the app's window on the message thread: parent for driver dialogs
            char msg[128] = {};
            drv_->getErrorMessage(msg);
            loadError_ = juce::String(msg).isNotEmpty() ? juce::String(msg) : juce::String("The interface driver didn't start (is the interface plugged in?)");
            drv_->Release();
            drv_ = nullptr;
            return;
        }
        long ni = 0, no = 0;
        drv_->getChannels(&ni, &no);
        auto names = [this](long count, long isInput) {
            juce::StringArray r;
            for (long c = 0; c < count; ++c) {
                ChannelInfo ci {};
                ci.channel = c;
                ci.isInput = isInput;
                drv_->getChannelInfo(&ci);
                const juce::String n(ci.name, strnlen(ci.name, sizeof ci.name));
                r.add(n.isNotEmpty() ? n : (isInput ? "In " : "Out ") + juce::String(c + 1));
            }
            return r;
        };
        inNames_ = names(ni, 1);
        outNames_ = names(no, 0);
        drv_->getBufferSize(&minBuf_, &maxBuf_, &prefBuf_, &gran_);
        drv_->getSampleRate(&rate_);
    }

    // driver thread
    void process(long index) noexcept {
        if (!open_ || index < 0 || index > 1) return;
        const int n = bufSize_;
        int ii = 0;
        for (size_t i = 0; i < infos_.size(); ++i)
            if (infos_[i].isInput) toFloat(types_[i], infos_[i].buffers[index], inBuf_.getWritePointer(ii++), n);
        {
            const juce::ScopedLock sl(lock_);
            if (cb_ != nullptr) {
                cb_->audioDeviceIOCallbackWithContext(inBuf_.getArrayOfReadPointers(), activeIn_.countNumberOfSetBits(),
                                                      outBuf_.getArrayOfWritePointers(), activeOut_.countNumberOfSetBits(), n, {});
            } else {
                outBuf_.clear();
            }
        }
        int oi = 0;
        for (size_t i = 0; i < infos_.size(); ++i)
            if (!infos_[i].isInput) fromFloat(types_[i], outBuf_.getReadPointer(oi++), infos_[i].buffers[index], n);
        if (postOutput_) drv_->outputReady();
    }

    // the driver changed its buffer size, rate or channels: open again with the same choice, on the message thread
    void restartLater() {
        juce::MessageManager::callAsync([this, alive = alive_] {
            if (!*alive || !open_) return;
            juce::AudioIODeviceCallback* cb = nullptr;
            { const juce::ScopedLock sl(lock_); cb = cb_; }
            close();
            if (open(lastIn_, lastOut_, 0.0, lastBuf_).isEmpty() && cb != nullptr) start(cb);
        });
    }

    static void onBufferSwitch(long index, long) { if (auto* d = gActive.load()) d->process(index); }
    static void* onBufferSwitchTimeInfo(void*, long index, long) { onBufferSwitch(index, 1); return nullptr; }
    static void onRateChange(double) { if (auto* d = gActive.load()) d->restartLater(); }
    static long onMessage(long selector, long value, void*, double*) {
        switch (selector) {
            case kSelectorSupported:
                return value == kEngineVersion || value == kResetRequest || value == kBufferSizeChange
                       || value == kResyncRequest || value == kLatenciesChanged ? 1 : 0;
            case kEngineVersion: return 2;
            case kResetRequest: case kBufferSizeChange:
                if (auto* d = gActive.load()) d->restartLater();
                return 1;
            case kResyncRequest: case kLatenciesChanged: return 1;
            default: return 0;
        }
    }

    CLSID clsid_;
    Driver* drv_ = nullptr;
    juce::String error_, loadError_;
    juce::StringArray inNames_, outNames_;
    long minBuf_ = 0, maxBuf_ = 0, prefBuf_ = 0, gran_ = 0, inLat_ = 0, outLat_ = 0;
    double rate_ = 0.0;
    int bufSize_ = 0, lastBuf_ = 0;
    juce::BigInteger activeIn_, activeOut_, lastIn_, lastOut_;
    std::vector<BufferInfo> infos_;
    std::vector<long> types_;
    juce::AudioBuffer<float> inBuf_, outBuf_;
    bool postOutput_ = false, loaded_ = false;
    std::atomic<bool> open_ { false };
    juce::CriticalSection lock_;
    juce::AudioIODeviceCallback* cb_ = nullptr;
    std::shared_ptr<bool> alive_ = std::make_shared<bool>(true);
};

// --- the device type: drivers registered under HKLM\SOFTWARE\ASIO -------------------------------------
class Type final : public juce::AudioIODeviceType {
public:
    Type() : AudioIODeviceType(kTypeName) { scanForDevices(); }   // AudioDeviceManager only scans the types it made itself

    void scanForDevices() override {
        names_.clear();
        clsids_.clear();
        HKEY root = nullptr;
        if (RegOpenKeyExW(HKEY_LOCAL_MACHINE, L"SOFTWARE\\ASIO", 0, KEY_READ, &root) != ERROR_SUCCESS) return;
        wchar_t sub[256];
        for (DWORD i = 0;; ++i) {
            DWORD len = 256;
            if (RegEnumKeyExW(root, i, sub, &len, nullptr, nullptr, nullptr, nullptr) != ERROR_SUCCESS) break;
            wchar_t clsidText[64] = {};
            DWORD bytes = sizeof clsidText;
            CLSID clsid {};
            if (RegGetValueW(root, sub, L"CLSID", RRF_RT_REG_SZ, nullptr, clsidText, &bytes) == ERROR_SUCCESS
                && SUCCEEDED(CLSIDFromString(clsidText, &clsid))) {
                names_.add(juce::String(sub));
                clsids_.push_back(clsid);
            }
        }
        RegCloseKey(root);
    }

    juce::StringArray getDeviceNames(bool) const override { return names_; }
    // none: loading a driver can pop up its own dialogs (wrappers ask which device), so only the one the user picks
    int getDefaultDeviceIndex(bool) const override { return -1; }
    int getIndexOfDevice(juce::AudioIODevice* d, bool) const override { return d == nullptr ? -1 : names_.indexOf(d->getName()); }
    bool hasSeparateInputsAndOutputs() const override { return false; }

    juce::AudioIODevice* createDevice(const juce::String& outName, const juce::String& inName) override {
        const int i = names_.indexOf(outName.isNotEmpty() ? outName : inName);
        return i < 0 ? nullptr : new Device(names_[i], clsids_[size_t(i)]);
    }

private:
    juce::StringArray names_;
    std::vector<CLSID> clsids_;
};

std::unique_ptr<juce::AudioIODeviceType> createDeviceType() { return std::make_unique<Type>(); }
#else
std::unique_ptr<juce::AudioIODeviceType> createDeviceType() { return nullptr; }
#endif

} // namespace hearaside::karaoke::asio
