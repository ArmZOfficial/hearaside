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
    reset();
}

VarResampler::~VarResampler() {
    if (st_) speex_resampler_destroy(static_cast<SpeexResamplerState*>(st_));
}

void VarResampler::setCorrectionPpm(int ppm) {
    if (ppm == ppm_ || st_ == nullptr) return;
    ppm_ = ppm;
    // num/den = in/out * (1 + ppm/1e6). speex rescales its phase as samp_frac_num * den when the
    // ratio changes and gives up (leaving a stale filter -> out-of-bounds reads) if that overflows
    // 32 bits, so den must stay <= 65535. The resulting 5-15 ppm steps are averaged out by the
    // drift controller.
    const uint64_t k = std::max<uint64_t>(1, 65535 / baseDen_);
    const uint64_t den = uint64_t(baseDen_) * k;
    const double num = double(baseNum_) * double(k) * (1.0 + double(ppm) * 1.0e-6);
    const uint32_t numQ = uint32_t(std::clamp<double>(std::llround(num), 1.0, double(UINT32_MAX)));
    if (numQ == lastNum_) return;
    lastNum_ = numQ;
    speex_resampler_set_rate_frac(static_cast<SpeexResamplerState*>(st_), numQ, uint32_t(den), in_, out_);
}

uint32_t VarResampler::inputFor(uint32_t outFrames) const {
    const double ratio = double(in_) / double(out_) * (1.0 + ppm_ * 1.0e-6);
    return uint32_t(std::ceil(double(outFrames) * ratio)) + 2 + priming_;
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
    priming_ = inUsed >= priming_ ? 0 : priming_ - inUsed;
}

void VarResampler::reset() {
    if (st_) {
        auto* st = static_cast<SpeexResamplerState*>(st_);
        speex_resampler_reset_mem(st);
        speex_resampler_skip_zeros(st);
        // after skip_zeros the first half filter length of input produces no output
        priming_ = uint32_t(std::max(0, speex_resampler_get_input_latency(st)));
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
    auto_ = bufferMs <= 0.0;
    bufferMs_ = auto_ ? 20.0 : std::clamp(bufferMs, 5.0, 500.0);
    autoMs_ = 20.0;
    st_.autoBuffer = auto_;
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
    const double target = auto_ ? std::max(autoMs_ * 0.001 * dawRate, hubBlock * 1.25)
                                : std::max(bufferMs_ * 0.001 * dawRate, hubBlock * 1.5 + 0.012 * dawRate);
    minMargin_ = 1.0e9;
    windowSec_ = 0.0;
    lowerVotes_ = 0;
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
    L.header.consumerBufferMs.store(uint32_t(std::lround(auto_ ? autoMs_ : bufferMs_)), std::memory_order_relaxed);

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
    if (auto_) adapt(fill - double(rs_->inputFor(frames)), dtSec);

    // Resample until the block is full. One pass is normally enough; a second one covers the
    // fractional phase of the resampler. Never pad with zeros mid-stream (= audible click).
    float* in[2] = { inBuf_[0].data(), inBuf_[1].data() };
    uint32_t made = 0;
    for (int pass = 0; pass < 3 && made < frames; ++pass) {
        const uint32_t need = std::min<uint32_t>(rs_->inputFor(frames - made), uint32_t(inBuf_[0].size()));
        uint32_t avail = 0;
        const auto res = ssbus::ringReadAt(L.streamAudio[output_], L.streamHeader.writePos[output_], cursor_, in, need, &avail);
        if (res == ssbus::ReadResult::Overrun) {
            ++st_.overruns; resync(dawRate); fade_ = 0.0f; silence(); return;
        }
        if (avail == 0) break;
        uint32_t inUsed = avail, outMade = frames - made;
        float* o[2] = { out[0] + made, out[1] + made };
        rs_->process(in, inUsed, o, outMade);
        cursor_ += inUsed;
        made += outMade;
        if (outMade == 0 && inUsed == 0) break;
    }
    if (made < frames) {
        // Not enough data: the DAW stalled or the buffer is too small. Rebuild the buffer.
        ++st_.underruns;
        if (auto_) autoMs_ = std::min(120.0, autoMs_ + 5.0);   // be safer from now on
        needResync_ = true;
        silence();
        st_.streaming = false;
        return;
    }

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

// Adaptive buffer: every 2 s look at the worst margin (fill minus what the next read needs) and
// move the target so that this worst case keeps ~3 ms of headroom. Up immediately, down by at
// most 2 ms per window and only once the fill has settled on the current target.
void StreamConsumer::adapt(double marginFrames, double dtSec) {
    minMargin_ = std::min(minMargin_, marginFrames);
    windowSec_ += dtSec;   // stream time, not wall time
    if (windowSec_ < 2.0) return;

    const double fpm = std::max(8.0, double(dawRate_) * 0.001);   // frames per ms
    const double marginMs = minMargin_ / fpm;
    const double fillMs = fillFiltered_ / fpm;
    constexpr double kSafetyMs = 3.0;
    double next = autoMs_;
    if (marginMs < kSafetyMs) {
        next = std::max(autoMs_, fillMs + (kSafetyMs - marginMs) + 1.0);
        lowerVotes_ = 0;
    } else if (marginMs > kSafetyMs + 1.0 && std::abs(fillMs - autoMs_) < 1.5) {
        if (++lowerVotes_ >= 3) next = std::max(fillMs - (marginMs - kSafetyMs), autoMs_ - 2.0);
    } else {
        lowerVotes_ = 0;
    }
    next = std::clamp(next, 4.0, 120.0);
    if (std::abs(next - autoMs_) > 0.05) {
        autoMs_ = next;
        drift_.setTarget(autoMs_ * fpm);
    }
    minMargin_ = 1.0e9;
    windowSec_ = 0.0;
}

ConsumerStatus StreamConsumer::status() const { return st_; }

} // namespace ssdsp
