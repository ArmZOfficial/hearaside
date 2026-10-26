#include "VirtualMic.h"

namespace hearaside {

namespace {
std::unique_ptr<juce::AudioIODeviceType> wasapi(juce::WASAPIDeviceMode mode = juce::WASAPIDeviceMode::shared) {
    std::unique_ptr<juce::AudioIODeviceType> t(juce::AudioIODeviceType::createAudioIODeviceType_WASAPI(mode));
    if (t) t->scanForDevices();
    return t;
}

// Smallest period of at least 5 ms (the OBS source's tick): shorter saves little and risks dropouts.
int pickBufferSize(juce::AudioIODevice& dev, double rate) {
    auto sizes = dev.getAvailableBufferSizes();
    sizes.sort();
    for (int s : sizes)
        if (s >= rate / 200.0) return s;
    return dev.getDefaultBufferSize();
}
} // namespace

VirtualMic::~VirtualMic() { stop(); }

juce::StringArray VirtualMic::outputDevices() {
    auto t = wasapi();
    return t ? t->getDeviceNames(false) : juce::StringArray();
}

int VirtualMic::cableIndex(const juce::StringArray& devices) {
    for (int i = 0; i < devices.size(); ++i)
        if (devices[i].startsWithIgnoreCase("CABLE Input")) return i;
    return -1;
}

juce::String VirtualMic::start(const juce::String& device, const juce::String& busName) {
    stop();
    if (device.isEmpty()) return {};
    // Low-latency shared mode (Windows 10+, small periods) first, plain shared mode if the driver refuses.
    std::unique_ptr<juce::AudioIODevice> dev;
    juce::String err = "device not found: " + device;
    for (auto mode : { juce::WASAPIDeviceMode::sharedLowLatency, juce::WASAPIDeviceMode::shared }) {
        auto t = wasapi(mode);
        if (t == nullptr) continue;
        dev.reset(t->createDevice(device, {}));
        if (dev == nullptr) continue;
        // Shared mode runs at the Windows mix rate; prefer 48 kHz when the device offers it.
        const auto rates = dev->getAvailableSampleRates();
        const double rate = rates.contains(48000.0) ? 48000.0 : (rates.isEmpty() ? 48000.0 : rates[0]);
        juce::BigInteger outs;
        outs.setRange(0, 2, true);
        err = dev->open({}, outs, rate, pickBufferSize(*dev, rate));
        if (err.isEmpty()) break;
        dev.reset();
    }
    if (dev == nullptr) return err;

    dev_ = std::move(dev);
    device_ = device;
    bus_ = busName;
    dev_->start(this);
    return {};
}

void VirtualMic::stop() {
    if (dev_ != nullptr) {
        dev_->stop();
        dev_->close();
        dev_.reset();
    }
    device_.clear();
    std::lock_guard<std::mutex> lk(statusMutex_);
    status_ = {};
}

ssdsp::ConsumerStatus VirtualMic::status() const {
    std::lock_guard<std::mutex> lk(statusMutex_);
    return status_;
}

void VirtualMic::audioDeviceAboutToStart(juce::AudioIODevice* d) {
    rate_ = d->getCurrentSampleRate();
    consumer_.setReportToHub(false);
    consumer_.configure(bus_.toStdString(), 0, 0.0, uint32_t(rate_));   // output 0 = Stream Mix, 0 = adaptive buffer
}

void VirtualMic::audioDeviceIOCallbackWithContext(const float* const*, int, float* const* out, int numOut, int numSamples,
                                                  const juce::AudioIODeviceCallbackContext&) {
    if (numOut < 2) {   // a mono device gets nothing rather than half the mix
        for (int c = 0; c < numOut; ++c) juce::FloatVectorOperations::clear(out[c], numSamples);
        return;
    }
    consumer_.pull(out, uint32_t(numSamples), numSamples / rate_);
    for (int c = 2; c < numOut; ++c) juce::FloatVectorOperations::clear(out[c], numSamples);
    // the editor reads the status 4x a second: skip an update rather than wait for it
    std::unique_lock<std::mutex> lk(statusMutex_, std::try_to_lock);
    if (lk.owns_lock()) status_ = consumer_.status();
}

} // namespace hearaside
