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
    void reset() noexcept { primed_ = false; key_ = 0; }

    // audio thread. Fills out[0..1] with n frames. false = nothing to play (the friend is not sending, or
    // not enough audio has arrived yet): out is silence. `rate` is the friend's sample rate.
    bool pull(const ssbus::ChannelRing& ring, const std::atomic<uint64_t>& writePos, uint32_t rate, float* const* out, int n) noexcept;

    double bufferMs() const noexcept { return bufferMs_; }   // the jitter buffer in use right now
    bool   primed() const noexcept { return primed_; }

private:
    double sampleRate_ = 48000.0;
    int maxBlock_ = 256;
    std::unique_ptr<ssdsp::VarResampler> rsSame_, rs48_, rs441_;
    ssdsp::DriftController drift_;
    std::vector<float> in_[2];
    uint64_t cursor_ = 0, lastWrite_ = 0, starvedAt_ = 0;
    uint32_t key_ = 0;
    bool primed_ = false;
    double target_ = 0, floor_ = 0, fillAvg_ = 0, sinceWrite_ = 0, minMargin_ = 1.0e9, windowSec_ = 0, holdSec_ = 0, bufferMs_ = 0;
};

} // namespace ssengine
