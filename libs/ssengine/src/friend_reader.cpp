// SPDX-License-Identifier: MIT
#include "ssengine/friend_reader.h"

#include <algorithm>
#include <cmath>
#include <cstring>

namespace ssengine {

using namespace ssbus;

FriendReader::FriendReader() = default;
FriendReader::~FriendReader() = default;

void FriendReader::prepare(double sampleRate, int maxBlock) {
    sampleRate_ = sampleRate > 0 ? sampleRate : 48000.0;
    maxBlock_ = maxBlock > 16 ? maxBlock : 16;
    const uint32_t r = uint32_t(sampleRate_ + 0.5);
    rsSame_ = std::make_unique<ssdsp::VarResampler>(r, r, 4);
    rs48_ = std::make_unique<ssdsp::VarResampler>(48000, r, 4);
    rs441_ = std::make_unique<ssdsp::VarResampler>(44100, r, 4);
    for (auto& c : in_) c.assign(size_t(maxBlock_) * 4 + 128, 0.0f);
    reset();
}

// Lateness of each writePos advance against the earliest one in the last ~5 s, in a fixed 1 ms histogram (no allocation).
void FriendReader::observe(uint64_t w, double fr, double dt) noexcept {
    if (w != lastWrite_) {
        const double late = t_ - double(int64_t(w - base_)) / fr;   // when the stream's newest frame showed up, minus when a steady flow would have had it
        if (w < lastWrite_ || sinceWrite_ > 0.25) for (auto& m : lmin_) m = late;   // writer restarted or a long outage: new baseline
        lmin_[cur_] = std::min(lmin_[cur_], late);
        if (w > lastWrite_) advMin_[cur_] = std::min(advMin_[cur_], double(w - lastWrite_));
        double ref = lmin_[0];
        for (int k = 1; k < kSub; ++k) ref = std::min(ref, lmin_[k]);
        uint16_t& bin = hist_[cur_][std::min(kBins - 1, int((late - ref) * 1000.0))];
        if (bin < 65535) ++bin;
        sinceWrite_ = 0.0;
    } else {
        sinceWrite_ += dt;
    }
    lastWrite_ = w;
    t_ += dt;
    subSec_ += dt;
    if (subSec_ < 0.5) return;
    subSec_ = 0.0;
    cur_ = (cur_ + 1) % kSub;
    std::memset(hist_[cur_], 0, sizeof hist_[cur_]);
    lmin_[cur_] = advMin_[cur_] = 1.0e18;
    uint32_t total = 0, sum[kBins] = {};
    for (int k = 0; k < kSub; ++k)
        for (int b = 0; b < kBins; ++b) { sum[b] += hist_[k][b]; total += hist_[k][b]; }
    if (total < 20) { p95Ms_ = -1.0; return; }
    const uint32_t want = (total * 95u + 99u) / 100u;
    uint32_t acc = 0;
    int b = 0;
    for (; b < kBins - 1; ++b) { acc += sum[b]; if (acc >= want) break; }
    p95Ms_ = double(b + 1);   // upper edge of the bin
}

// data arrives in steps: the average fill sits half a step below the fill right after a packet (frames)
double FriendReader::halfPacket(double fr) const noexcept {
    double pkt = 1.0e18;
    for (double a : advMin_) pkt = std::min(pkt, a);
    return pkt < 1.0e17 && pkt > blockFr_ ? pkt * 0.5 : 0.0;
}

// block + measured jitter + 2 ms + half a packet; until there is a measurement a modest guess
double FriendReader::idealTarget(double fr) const noexcept {
    const double half = halfPacket(fr);
    const double ms = p95Ms_ < 0.0 ? 20.0 : p95Ms_ + 2.0;
    return std::clamp(blockFr_ + fr * ms * 0.001 + half, blockFr_ + fr * 0.002, blockFr_ + fr * 0.4);
}

bool FriendReader::pull(const ChannelRing& ring, const std::atomic<uint64_t>& writePos, uint32_t rate, float* const* out, int n) noexcept {
    auto silence = [&] { for (int c = 0; c < 2; ++c) std::memset(out[c], 0, size_t(n) * sizeof(float)); lastOut_[0] = lastOut_[1] = 0.0f; };
    const uint32_t sr = uint32_t(sampleRate_ + 0.5);
    ssdsp::VarResampler* rs = rate == sr ? rsSame_.get() : rate == 48000 ? rs48_.get() : rate == 44100 ? rs441_.get() : nullptr;
    if (rs == nullptr || n <= 0 || n > maxBlock_) { primed_ = false; muted_ = false; silence(); return false; }
    const double dt = double(n) / sampleRate_;
    const double fr = double(rate);
    const uint64_t w = writePos.load(std::memory_order_acquire);
    const int fadeFrames = std::max(1, int(sampleRate_ * 0.002));
    blockFr_ = std::max(blockFr_, double(n) * fr / sampleRate_);
    if (key_ != rate) {   // first block or the browser changed its rate: start with a modest guess, the measurements take over
        key_ = rate;
        blockFr_ = double(n) * fr / sampleRate_;
        for (auto& m : lmin_) m = 1.0e18;
        for (auto& m : advMin_) m = 1.0e18;
        std::memset(hist_, 0, sizeof hist_);
        t_ = subSec_ = 0.0;
        cur_ = 0;
        p95Ms_ = -1.0;
        base_ = starvedAt_ = lastWrite_ = w;
        sinceWrite_ = 0.0;
        target_ = idealTarget(fr);
        holdSec_ = 0.0;
        primed_ = muted_ = false;
    }
    observe(w, fr, dt);
    auto lost = [&](double frames) { if (played_) concealedMs_ += frames * 1000.0 / sampleRate_; };   // silence we play after the first sound
    const int fadeLen = std::min(fadeFrames, n);
    // silence for this block, after a short decay from the last sample played (no step in the output)
    auto decayThenSilence = [&] {
        for (int c = 0; c < 2; ++c) {
            std::memset(out[c], 0, size_t(n) * sizeof(float));
            for (int j = 0; j < fadeLen; ++j) out[c][j] = lastOut_[c] * float(fadeLen - 1 - j) / float(fadeLen);
        }
        lastOut_[0] = lastOut_[1] = 0.0f;
    };
    auto reprime = [&] { primed_ = false; muted_ = false; starvedAt_ = w; ++reprimes_; lost(n); decayThenSilence(); return false; };
    double fill = double(int64_t(w - cursor_));
    const double maxFill = double(kRingFrames - kGuardFrames);
    if (!primed_ || fill < 0.0 || fill > std::min(maxFill, target_ * 2.0 + fr * 0.05)) {
        if (primed_ && played_) ++reprimes_;   // too much data or a nonsense position while playing: jump to the newest
        if (double(w) < double(starvedAt_) + target_ + double(n)) { lost(n); silence(); return false; }
        cursor_ = w - uint64_t(target_ + halfPacket(fr));   // w is at the top of a step now, the controller wants the mean
        drift_.reset(target_);
        rs->reset();
        primed_ = played_ = true;
        muted_ = false;
        fadeLeft_ = fadeFrames;
        fadeFrom_[0] = lastOut_[0]; fadeFrom_[1] = lastOut_[1];   // a fade longer than a block must keep fading from this, not from the mixed block end
        fill = target_;
        fillAvg_ = target_;
        minMargin_ = 1.0e9;
        windowSec_ = 0.0;
    }
    const double need = double(rs->inputFor(uint32_t(n)));
    bool startMute = false;
    if (muted_) {
        if (fill < need) {   // still waiting: a short gap keeps its place, a long one starts over
            if (mutedOut_ >= sampleRate_ * 0.03) {   // long gap: learn from it as well, or a stall every few seconds repeats forever
                target_ = std::min(target_ + missing_ + mutedOut_ * fr / sampleRate_, blockFr_ + fr * 0.4);
                holdSec_ = 5.0;
                return reprime();
            }
            mutedOut_ += n;
            lost(n);
            silence();
            return true;
        }
        // data is back: raise the target by what was missing, so the next one like it does not gap
        target_ = std::min(target_ + missing_ + mutedOut_ * fr / sampleRate_, blockFr_ + fr * 0.4);
        drift_.setTarget(target_);
        holdSec_ = 5.0;
        muted_ = false;
        fadeLeft_ = fadeFrames;
        fadeFrom_[0] = lastOut_[0]; fadeFrom_[1] = lastOut_[1];   // a fade longer than a block must keep fading from this, not from the mixed block end
    } else if (fill < need) {
        startMute = true;
    } else {
        fillAvg_ += (fill - fillAvg_) * (1.0 - std::exp(-dt / 0.3));
        minMargin_ = std::min(minMargin_, fill - need);
        windowSec_ += dt;
        holdSec_ -= dt;
        if (windowSec_ >= 1.0) {   // follow the measured jitter: up at once, down by half the spare margin per second
            const double ideal = idealTarget(fr);
            if (ideal > target_) {
                target_ = ideal;
            } else if (holdSec_ <= 0.0 && std::abs(fillAvg_ - target_) < fr * 0.002) {
                const double spare = minMargin_ - fr * 0.002;
                const double cut = std::min(0.5 * (target_ - ideal), 0.5 * spare);
                if (cut > fr * 0.0002) target_ -= cut;
            }
            target_ = std::max(target_, blockFr_ + fr * 0.002);
            drift_.setTarget(target_);
            windowSec_ = 0.0;
            minMargin_ = 1.0e9;
        }
        const double c = drift_.update(fill, dt);
        rs->setCorrectionPpm(int(std::lround(c * 1.0e6)));
    }
    float* in[2] = { in_[0].data(), in_[1].data() };
    uint32_t made = 0;
    for (int pass = 0; pass < 3 && made < uint32_t(n); ++pass) {
        const uint32_t want = std::min<uint32_t>(rs->inputFor(uint32_t(n) - made), uint32_t(in_[0].size()));
        uint32_t avail = 0;
        if (ringReadAt(ring, writePos, cursor_, in, want, &avail) == ReadResult::Overrun) return reprime();
        if (avail == 0) break;
        uint32_t usedIn = avail, got = uint32_t(n) - made;
        float* o[2] = { out[0] + made, out[1] + made };
        rs->process(in, usedIn, o, got);
        cursor_ += usedIn;
        made += got;
        if (usedIn == 0 && got == 0) break;
    }
    for (int c2 = 0; c2 < 2; ++c2)
        if (made < uint32_t(n)) std::memset(out[c2] + made, 0, (uint32_t(n) - made) * sizeof(float));
    if (fadeLeft_ > 0) {   // no click when the voice (re)starts: the last sample before it (0 after silence) fades out as the new data fades in
        const int m = std::min(n, fadeLeft_);
        for (int c2 = 0; c2 < 2; ++c2)
            for (int j = 0; j < m; ++j) {
                const float g = float(fadeFrames - fadeLeft_ + j + 1) / float(fadeFrames);
                out[c2][j] = out[c2][j] * g + fadeFrom_[c2] * (1.0f - g);
            }
        fadeLeft_ -= m;
    }
    if (startMute && made < uint32_t(n)) {   // short gap: fade out what we have, then hold the last sample's decay, then wait for data
        const int start = made >= uint32_t(fadeLen) ? int(made) - fadeLen : 0;
        for (int c2 = 0; c2 < 2; ++c2) {
            const float hold = made > 0 ? out[c2][made - 1] : lastOut_[c2];
            for (int j = 0; j < fadeLen; ++j) {
                const int i = start + j;
                out[c2][i] = (uint32_t(i) < made ? out[c2][i] : hold) * float(fadeLen - 1 - j) / float(fadeLen);
            }
        }
        muted_ = true;
        fadeLeft_ = 0;
        ++underruns_;
        mutedOut_ = double(n) - double(made);
        missing_ = need - fill;
        lost(mutedOut_);
        lastOut_[0] = lastOut_[1] = 0.0f;
        return true;
    }
    lastOut_[0] = out[0][n - 1];
    lastOut_[1] = out[1][n - 1];
    bufferMs_ = fillAvg_ * 1000.0 / fr;
    return true;
}

} // namespace ssengine
