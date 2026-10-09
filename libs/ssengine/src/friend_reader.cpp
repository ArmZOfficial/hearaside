// SPDX-License-Identifier: MIT
#include "ssengine/friend_reader.h"

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

bool FriendReader::pull(const ChannelRing& ring, const std::atomic<uint64_t>& writePos, uint32_t rate, float* const* out, int n) noexcept {
    auto silence = [&] { for (int c = 0; c < 2; ++c) std::memset(out[c], 0, size_t(n) * sizeof(float)); };
    const uint32_t sr = uint32_t(sampleRate_ + 0.5);
    ssdsp::VarResampler* rs = rate == sr ? rsSame_.get() : rate == 48000 ? rs48_.get() : rate == 44100 ? rs441_.get() : nullptr;
    if (rs == nullptr || n <= 0 || n > maxBlock_) { primed_ = false; silence(); return false; }
    const double dt = double(n) / sampleRate_;
    const double fr = double(rate);
    const uint64_t w = writePos.load(std::memory_order_acquire);
    if (key_ != rate) {   // first block or the browser changed its rate: safe start with room for the network
        key_ = rate;
        const double block = double(maxBlock_) * fr / sampleRate_;
        const double packet = fr * 0.03;
        target_ = block + packet + fr * 0.04;
        floor_ = block + 32.0;
        holdSec_ = 0.0;
        primed_ = false;
        starvedAt_ = lastWrite_ = w;
    }
    sinceWrite_ = w != lastWrite_ ? 0.0 : sinceWrite_ + dt;
    lastWrite_ = w;
    const bool arriving = sinceWrite_ < 0.09 + dt;
    double fill = double(int64_t(w - cursor_));
    const double maxFill = double(kRingFrames - kGuardFrames);
    if (!primed_ || fill < 0.0 || fill > std::min(maxFill, target_ * 2.0 + fr * 0.05)) {
        if (double(w) < double(starvedAt_) + target_ + double(n)) { silence(); return false; }
        cursor_ = w - uint64_t(target_);
        drift_.reset(target_);
        rs->reset();
        primed_ = true;
        fill = target_;
        fillAvg_ = target_;
        minMargin_ = 1.0e9;
        windowSec_ = 0.0;
    }
    const double need = double(rs->inputFor(uint32_t(n)));
    if (fill < need) {
        if (arriving) {   // packets come but not fast enough: this line needs more headroom
            target_ = std::min(target_ + fr * 0.002, double(maxBlock_) * fr / sampleRate_ + fr * 0.4);
            floor_ = target_;
            holdSec_ = 5.0;
        }
        primed_ = false;
        starvedAt_ = w;
        silence();
        return false;
    }
    fillAvg_ += (fill - fillAvg_) * (1.0 - std::exp(-dt / 0.3));
    minMargin_ = std::min(minMargin_, fill - need);
    windowSec_ += dt;
    holdSec_ -= dt;
    if (windowSec_ >= 3.0) {   // give back unused headroom every 3 s
        const double spare = minMargin_ - fr * 0.002;
        if (holdSec_ <= 0.0 && spare > fr * 0.0002 && std::abs(fillAvg_ - target_) < fr * 0.0005) {
            target_ = std::max(std::max(floor_, double(n) * fr / sampleRate_ + 32.0), target_ - std::min(spare * 0.5, fr * 0.001));
            drift_.setTarget(target_);
        }
        windowSec_ = 0.0;
        minMargin_ = 1.0e9;
    }
    const double c = drift_.update(fill, dt);
    rs->setCorrectionPpm(int(std::lround(c * 1.0e6)));
    float* in[2] = { in_[0].data(), in_[1].data() };
    uint32_t made = 0;
    for (int pass = 0; pass < 3 && made < uint32_t(n); ++pass) {
        const uint32_t want = std::min<uint32_t>(rs->inputFor(uint32_t(n) - made), uint32_t(in_[0].size()));
        uint32_t avail = 0;
        if (ringReadAt(ring, writePos, cursor_, in, want, &avail) == ReadResult::Overrun) { primed_ = false; starvedAt_ = w; silence(); return false; }
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
    bufferMs_ = fillAvg_ * 1000.0 / fr;
    return true;
}

} // namespace ssengine
