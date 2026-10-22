#include "ssnet/aec.h"

#include <speex/speex_echo.h>
#include <speex/speex_preprocess.h>

#include <algorithm>
#include <cmath>
#include <cstring>

namespace ssnet {

FriendEcho::FriendEcho() = default;

FriendEcho::~FriendEcho() {
    cleanup();
}

FriendEcho::FriendEcho(FriendEcho&& o) noexcept
    : sampleRate_(o.sampleRate_)
    , frameSize_(o.frameSize_)
    , filterLength_(o.filterLength_)
    , mode_(o.mode_)
    , leakDetected_(o.leakDetected_)
    , erleDb_(o.erleDb_)
    , echoState_(o.echoState_)
    , prepState_(o.prepState_)
    , refRing_(std::move(o.refRing_))
    , refWritePos_(o.refWritePos_)
    , spxRec_(std::move(o.spxRec_))
    , spxPlay_(std::move(o.spxPlay_))
    , spxOut_(std::move(o.spxOut_)) {
    o.echoState_ = nullptr;
    o.prepState_ = nullptr;
}

FriendEcho& FriendEcho::operator=(FriendEcho&& o) noexcept {
    if (this != &o) {
        cleanup();
        sampleRate_ = o.sampleRate_;
        frameSize_ = o.frameSize_;
        filterLength_ = o.filterLength_;
        mode_ = o.mode_;
        leakDetected_ = o.leakDetected_;
        erleDb_ = o.erleDb_;
        echoState_ = o.echoState_;
        prepState_ = o.prepState_;
        refRing_ = std::move(o.refRing_);
        refWritePos_ = o.refWritePos_;
        spxRec_ = std::move(o.spxRec_);
        spxPlay_ = std::move(o.spxPlay_);
        spxOut_ = std::move(o.spxOut_);

        o.echoState_ = nullptr;
        o.prepState_ = nullptr;
    }
    return *this;
}

void FriendEcho::cleanup() {
    if (prepState_) {
        speex_preprocess_state_destroy(prepState_);
        prepState_ = nullptr;
    }
    if (echoState_) {
        speex_echo_state_destroy(echoState_);
        echoState_ = nullptr;
    }
}

bool FriendEcho::init(int sampleRate, int frameSize, int filterLengthMs) {
    cleanup();
    sampleRate_ = std::max(8000, sampleRate);
    frameSize_ = std::max(64, frameSize);
    const int filterSamples = std::max(frameSize_ * 2, int(int64_t(sampleRate_) * std::max(50, filterLengthMs) / 1000));
    filterLength_ = ((filterSamples + frameSize_ - 1) / frameSize_) * frameSize_;

    echoState_ = speex_echo_state_init(frameSize_, filterLength_);
    if (!echoState_) return false;

    int rate = sampleRate_;
    speex_echo_ctl(echoState_, SPEEX_ECHO_SET_SAMPLING_RATE, &rate);

    prepState_ = speex_preprocess_state_init(frameSize_, sampleRate_);
    if (!prepState_) {
        cleanup();
        return false;
    }

    speex_preprocess_ctl(prepState_, SPEEX_PREPROCESS_SET_ECHO_STATE, echoState_);

    int denoise = 0;   // Keep singing harmonics intact (Karaoke)
    speex_preprocess_ctl(prepState_, SPEEX_PREPROCESS_SET_DENOISE, &denoise);

    int echoSuppress = -45;
    speex_preprocess_ctl(prepState_, SPEEX_PREPROCESS_SET_ECHO_SUPPRESS, &echoSuppress);

    int echoSuppressActive = 0;   // Transparent singing voice during double-talk (no gating)
    speex_preprocess_ctl(prepState_, SPEEX_PREPROCESS_SET_ECHO_SUPPRESS_ACTIVE, &echoSuppressActive);

    // 3 seconds of reference ring buffer
    refRing_.assign(size_t(sampleRate_ * 3), 0.0f);
    refWritePos_ = 0;

    spxRec_.resize(size_t(frameSize_));
    spxPlay_.resize(size_t(frameSize_));
    spxOut_.resize(size_t(frameSize_));
    erleDb_ = 0.0f;

    return true;
}

void FriendEcho::reset() {
    if (echoState_) speex_echo_state_reset(echoState_);
    std::fill(refRing_.begin(), refRing_.end(), 0.0f);
    refWritePos_ = 0;
    erleDb_ = 0.0f;
}

void FriendEcho::pushReference(const float* ref, size_t n) {
    if (!ref || n == 0 || refRing_.empty()) return;
    const size_t cap = refRing_.size();
    for (size_t i = 0; i < n; ++i) {
        refRing_[(refWritePos_ + i) % cap] = ref[i];
    }
    refWritePos_ = (refWritePos_ + n) % cap;
}

void FriendEcho::process(const float* micIn, float* cleanOut, size_t n, int delaySamples) {
    if (!micIn || !cleanOut || n == 0) return;

    const bool runAec = (mode_ == AecMode::On) || (mode_ == AecMode::Auto && leakDetected_);
    if (!runAec || !echoState_ || !prepState_ || int(n) != frameSize_ || refRing_.empty()) {
        std::memcpy(cleanOut, micIn, n * sizeof(float));
        return;
    }

    const size_t cap = refRing_.size();
    const int maxDelay = int(cap) - int(n) - 1;
    const int d = std::clamp(delaySamples, 0, maxDelay);

    int64_t startIdx = int64_t(refWritePos_) - int64_t(d) - int64_t(n);
    while (startIdx < 0) startIdx += int64_t(cap);
    startIdx %= int64_t(cap);

    float inPower = 0.0f;
    for (size_t i = 0; i < n; ++i) {
        const float r = refRing_[size_t((startIdx + int64_t(i)) % int64_t(cap))];
        const float m = micIn[i];
        inPower += m * m;
        spxPlay_[i] = int16_t(std::lround(std::clamp(r, -1.0f, 1.0f) * 32767.0f));
        spxRec_[i]  = int16_t(std::lround(std::clamp(m, -1.0f, 1.0f) * 32767.0f));
    }

    speex_echo_cancellation(echoState_, spxRec_.data(), spxPlay_.data(), spxOut_.data());
    speex_preprocess_run(prepState_, spxOut_.data());

    float outPower = 0.0f;
    for (size_t i = 0; i < n; ++i) {
        cleanOut[i] = float(spxOut_[i]) / 32768.0f;
        outPower += cleanOut[i] * cleanOut[i];
    }

    if (inPower > 1e-6f) {
        const float currErle = 10.0f * std::log10((inPower + 1e-9f) / (outPower + 1e-9f));
        erleDb_ = (erleDb_ == 0.0f) ? currErle : (erleDb_ * 0.95f + currErle * 0.05f);
    }
}

} // namespace ssnet
