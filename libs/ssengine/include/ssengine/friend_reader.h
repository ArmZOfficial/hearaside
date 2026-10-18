// Plays one friend's microphone from shared memory (friendAudio[i], written by the Hub's share server)
// into a block of the reader's own sample rate: a small jitter buffer that learns how much headroom this
// connection needs, drift control against the friend's clock, and a resampler. Used by the Hub engine (the
// friends it plays itself) and by HEARASIDE Track in "friend input" mode (the friend on a DAW channel).
// SPDX-License-Identifier: MIT
#pragma once

#include "ssbus/bus.h"
#include "ssdsp/consumer.h"
#include "ssdsp/dsp.h"

#include <memory>
#include <vector>

namespace ssengine {

class FriendReader {
public:
    FriendReader();
    ~FriendReader();

    // message thread: allocates (resamplers for the friend's 48 / 44.1 kHz or our own rate)
    void prepare(double sampleRate, int maxBlock);
    // audio thread: forget the position (a new friend, or after a gap): the next pull starts safe
    void reset() noexcept { primed_ = false; key_ = 0; muted_ = false; played_ = false; underruns_ = 0; concealedMs_ = 0; p95Ms_ = -1.0; }

    // audio thread. Fills out[0..1] with n frames. false = nothing to play (the friend is not sending, or
    // not enough audio has arrived yet): out is silence. `rate` is the friend's sample rate.
    bool pull(const ssbus::ChannelRing& ring, const std::atomic<uint64_t>& writePos, uint32_t rate, float* const* out, int n) noexcept;

    double bufferMs() const noexcept { return bufferMs_; }   // the jitter buffer in use right now
    bool   primed() const noexcept { return primed_; }
    // 0.1 metrics (audio thread, no allocation): short gaps concealed, silence played after the first sound,
    // and the p95 of how late data arrives against a steady flow over the last ~5 s (ms, -1 = not enough data yet)
    uint32_t underruns() const noexcept { return underruns_; }
    double   concealedMs() const noexcept { return concealedMs_; }
    double   jitterP95Ms() const noexcept { return p95Ms_; }

private:
    double sampleRate_ = 48000.0;
    int maxBlock_ = 256;
    std::unique_ptr<ssdsp::VarResampler> rsSame_, rs48_, rs441_;
    ssdsp::DriftController drift_;
    std::vector<float> in_[2];
    void observe(uint64_t w, double fr, double dt) noexcept;
    double idealTarget(double fr) const noexcept;
    double halfPacket(double fr) const noexcept;
    uint64_t cursor_ = 0, lastWrite_ = 0, starvedAt_ = 0, base_ = 0;
    uint32_t key_ = 0;
    bool primed_ = false, muted_ = false, played_ = false;
    double target_ = 0, blockFr_ = 0, fillAvg_ = 0, sinceWrite_ = 0, minMargin_ = 1.0e9, windowSec_ = 0, holdSec_ = 0, bufferMs_ = 0;
    // a short gap: cursor_ stays put, the output is silence (faded in / out) until a whole block is there again
    double mutedOut_ = 0, missing_ = 0;   // silent output frames so far / input frames missing when it began
    int fadeLeft_ = 0;
    float lastOut_[2] = { 0, 0 };
    uint32_t underruns_ = 0;
    double concealedMs_ = 0;
    // arrival jitter: lateness of each writePos advance against the earliest in the window, 1 ms bins, 10 x 0.5 s
    static constexpr int kBins = 129, kSub = 10;
    uint16_t hist_[kSub][kBins] = {};
    double lmin_[kSub] = {}, advMin_[kSub] = {}, t_ = 0, subSec_ = 0, p95Ms_ = -1.0;
    int cur_ = 0;
};

} // namespace ssengine
