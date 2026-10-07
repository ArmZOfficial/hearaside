// SPDX-License-Identifier: MIT
#include "ssdsp/consumer.h"

#include "speex_resampler.h"

#include <climits>
#include <cmath>
#include <cstring>
#include <numeric>

namespace ssdsp {

// ---------------------------------------------------------------------------------------------
// VarResampler

VarResampler::VarResampler(uint32_t inRate, uint32_t outRate, int quality) : in_(inRate), out_(outRate) {
    const uint32_t g = std::gcd(inRate, outRate);
    baseNum_ = inRate / g;
    baseDen_ = outRate / g;
    int err = 0;
    st_ = speex_resampler_init_frac(2, baseNum_, baseDen_, inRate, outRate, quality, &err);
    speex_resampler_skip_zeros(static_cast<SpeexResamplerState*>(st_));
}

VarResampler::~VarResampler() {
    if (st_) speex_resampler_destroy(static_cast<SpeexResamplerState*>(st_));
}

void VarResampler::setCorrectionPpm(int ppm) {
    if (ppm == ppm_ || st_ == nullptr) return;
    ppm_ = ppm;
    // num/den = in/out * (1 + ppm/1e6). Pick the finest scale that fits in 32 bits.
    uint64_t scale = 1000000;
    while (scale > 1 && (uint64_t(baseNum_) * (scale + scale / 50) > UINT32_MAX || uint64_t(baseDen_) * scale > UINT32_MAX))
        scale /= 10;
    const int64_t adj = int64_t(std::llround(double(ppm) * double(scale) / 1.0e6));
    const uint64_t num = uint64_t(baseNum_) * uint64_t(int64_t(scale) + adj);
    const uint64_t den = uint64_t(baseDen_) * scale;
    speex_resampler_set_rate_frac(static_cast<SpeexResamplerState*>(st_), uint32_t(num), uint32_t(den), in_, out_);
}

uint32_t VarResampler::inputFor(uint32_t outFrames) const {
    const double ratio = double(in_) / double(out_) * (1.0 + ppm_ * 1.0e-6);
    return uint32_t(std::ceil(double(outFrames) * ratio)) + 2;
}

void VarResampler::process(const float* const* in, uint32_t& inFrames, float* const* out, uint32_t& outFrames) {
    auto* st = static_cast<SpeexResamplerState*>(st_);
    uint32_t inUsed = 0, outMade = 0;
    for (uint32_t c = 0; c < 2; ++c) {
        spx_uint32_t il = inFrames, ol = outFrames;
        speex_resampler_process_float(st, c, in[c], &il, out[c], &ol);
        inUsed = il; outMade = ol;   // identical for both channels
    }
    inFrames = inUsed;
    outFrames = outMade;
}

void VarResampler::reset() {
    if (st_) {
        speex_resampler_reset_mem(static_cast<SpeexResamplerState*>(st_));
        speex_resampler_skip_zeros(static_cast<SpeexResamplerState*>(st_));
    }
}

// ---------------------------------------------------------------------------------------------
// StreamConsumer

StreamConsumer::StreamConsumer() {
    for (auto& b : inBuf_) b.resize(ssbus::kRingFrames / 4);
}
StreamConsumer::~StreamConsumer() = default;

void StreamConsumer::configure(const std::string& busName, int outputIndex, double bufferMs, uint32_t outRate) {
    const bool busChanged = busName != busName_;
    busName_ = busName.empty() ? std::string("Main") : busName;
    output_ = std::clamp(outputIndex, 0, ssbus::kNumStreamOuts - 1);
    bufferMs_ = std::clamp(bufferMs, 5.0, 500.0);
    if (outRate != outRate_) rs_.reset();
    outRate_ = outRate;
    if (busChanged) shm_.reset();
    needResync_ = true;
    reconnectTimer_ = 1.0e9;   // connect on next pull
}

void StreamConsumer::tryConnect() {
    ssbus::SharedMemory::Status s{};
    shm_ = ssbus::SharedMemory::openExisting(busName_, s);
    st_.connected = shm_ != nullptr;
    st_.error = shm_ ? std::string() : (s == ssbus::SharedMemory::Status::CreateFailed ? std::string() : ssbus::statusText(s));
    needResync_ = true;
}

void StreamConsumer::resync(uint32_t dawRate) {
    auto& L = shm_->layout();
    if (!rs_ || rs_->inRate() != dawRate || rs_->outRate() != outRate_) rs_ = std::make_unique<VarResampler>(dawRate, outRate_);
    else rs_->reset();
    rs_->setCorrectionPpm(0);
    dawRate_ = dawRate;

    const double hubBlock = L.header.hubBlockSize.load(std::memory_order_relaxed);
    const double target = std::max(bufferMs_ * 0.001 * dawRate, hubBlock * 1.5 + 0.012 * dawRate);
    drift_.reset(target);
    const uint64_t w = L.streamHeader.writePos[output_].load(std::memory_order_acquire);
    cursor_ = w > uint64_t(target) ? w - uint64_t(target) : 0;
    fillFiltered_ = target;
    needResync_ = false;
    ++st_.resyncs;
}

void StreamConsumer::pull(float* const* out, uint32_t frames, double dtSec) {
    auto silence = [&] { std::memset(out[0], 0, frames * sizeof(float)); std::memset(out[1], 0, frames * sizeof(float)); };

    if (!shm_) {
        reconnectTimer_ += dtSec;
        if (reconnectTimer_ >= 1.0) { reconnectTimer_ = 0; tryConnect(); }
        if (!shm_) { st_.connected = false; st_.hubAlive = false; st_.streaming = false; silence(); return; }
    }

    auto& L = shm_->layout();
    const uint64_t nowNs = ssbus::nowNs();
    L.header.consumerHeartbeatNs.store(nowNs, std::memory_order_relaxed);
    L.header.consumerBufferMs.store(uint32_t(bufferMs_), std::memory_order_relaxed);

    const bool hubOk = ssbus::hubAlive(L, 500000000ull)
                    && L.streamHeader.active.load(std::memory_order_acquire) != 0;
    uint32_t dawRate = L.streamHeader.sampleRate.load(std::memory_order_acquire);
    const uint32_t flags = L.header.hubFlags.load(std::memory_order_relaxed);
    st_.connected = true;
    st_.hubAlive = hubOk;
    st_.hubFlags = flags;
    st_.dawRate = dawRate;

    const bool rateOk = dawRate >= 8000 && dawRate <= 384000;
    if (!hubOk || !rateOk) {
        // Fade out whatever is left (10 ms), then go silent and resync when the Hub comes back.
        if (!(fade_ > 0.0f && rs_ && !needResync_)) {
            fade_ = 0.0f;
            needResync_ = true;
            st_.streaming = false;
            silence();
            return;
        }
    }
    const uint32_t useRate = (hubOk && rateOk) ? dawRate : dawRate_;
    if (needResync_ || useRate != dawRate_) { resync(useRate); fade_ = 0.0f; }
    dawRate = useRate;

    const uint64_t w = L.streamHeader.writePos[output_].load(std::memory_order_acquire);
    const double fill = double(int64_t(w - cursor_));
    if (fill < 0 || fill > double(ssbus::kRingFrames - ssbus::kGuardFrames)) {
        ++st_.overruns;
        resync(dawRate);
        fade_ = 0.0f;
        silence();
        return;
    }

    const double c = drift_.update(fill, dtSec);
    rs_->setCorrectionPpm(int(std::lround(c * 1.0e6)));
    fillFiltered_ += (fill - fillFiltered_) * (1.0 - std::exp(-dtSec / 0.5));

    uint32_t need = std::min<uint32_t>(rs_->inputFor(frames), uint32_t(inBuf_[0].size()));
    float* in[2] = { inBuf_[0].data(), inBuf_[1].data() };
    uint32_t avail = 0;
    const auto res = ssbus::ringReadAt(L.streamAudio[output_], L.streamHeader.writePos[output_], cursor_, in, need, &avail);
    if (res == ssbus::ReadResult::Overrun) {
        ++st_.overruns; resync(dawRate); fade_ = 0.0f; silence(); return;
    }
    if (avail + 2 < need) {
        // Not enough data: the DAW stalled or the buffer is too small. Rebuild the buffer.
        ++st_.underruns;
        needResync_ = true;
        silence();
        st_.streaming = false;
        return;
    }

    uint32_t inUsed = avail, outMade = frames;
    rs_->process(in, inUsed, out, outMade);
    cursor_ += inUsed;
    for (uint32_t ch = 0; ch < 2; ++ch)
        if (outMade < frames) std::memset(out[ch] + outMade, 0, (frames - outMade) * sizeof(float));

    // 10 ms fades for connect / disconnect
    const float fadeTarget = hubOk ? 1.0f : 0.0f;
    const float step = 1.0f / (0.010f * float(outRate_));
    if (fade_ != fadeTarget || fadeTarget < 1.0f) {
        for (uint32_t i = 0; i < frames; ++i) {
            fade_ = fadeTarget > fade_ ? std::min(fadeTarget, fade_ + step) : std::max(fadeTarget, fade_ - step);
            out[0][i] *= fade_;
            out[1][i] *= fade_;
        }
    }
    if (!hubOk && fade_ <= 0.0f) needResync_ = true;

    st_.streaming = hubOk;
    st_.bufferMs = fillFiltered_ * 1000.0 / dawRate;
    st_.targetMs = drift_.target() * 1000.0 / dawRate;
    st_.driftPpm = c * 1.0e6;

    // output name
    {
        const uint32_t s1 = L.streamHeader.nameSeq.load(std::memory_order_acquire);
        if (!(s1 & 1u)) {
            char buf[ssbus::kNameBytes];
            std::memcpy(buf, L.streamHeader.names[output_], sizeof buf);
            buf[sizeof buf - 1] = 0;
            if (L.streamHeader.nameSeq.load(std::memory_order_acquire) == s1) st_.outputName = buf;
        }
    }
}

ConsumerStatus StreamConsumer::status() const { return st_; }

} // namespace ssdsp
