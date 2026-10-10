// The Hub's friends room, audio side (S2 + S4): every friend's microphone comes in through
// shared memory (friendAudio[i], written by the Hub's share server), is played with a small
// jitter buffer and drift control, and is mixed into
//   * the headphones (you hear x level, not panned, not delayed), and
//   * the Stream Mix (viewers hear x level x pan x solo), AFTER the Line-up delay and BEFORE
//     the limiter, so the limiter sees the final sum.
// Line up: the Stream Mix as the DAW makes it waits D frames (the slowest friend), and each friend
// is delayed D - (how late that friend is), so every voice lands on the beat for the viewers.
// The headphones and the copy of the mix that friends hear (friendMixAudio) are never delayed.
// SPDX-License-Identifier: MIT
#include "ssengine/engine.h"
#include "ssengine/friend_reader.h"

#include <cmath>
#include <cstring>

namespace ssengine {

using namespace ssbus;

namespace {
constexpr uint64_t kFriendStaleNs = 800000000ull;   // no packet for this long: the friend is gone for now
constexpr float kMaxFriendMs = 1000.0f;

// out[i] = in[i - d] over a ring that always holds the newest input. d <= cap - n.
inline void readTap(const std::vector<float>& ring, uint32_t endPos, uint32_t n, uint32_t d, float* out) noexcept {
    const uint32_t cap = uint32_t(ring.size());
    uint32_t at = (endPos + cap - n - d % cap) % cap;
    for (uint32_t i = 0; i < n; ++i) { out[i] = ring[at]; if (++at == cap) at = 0; }
}
} // namespace

struct HubEngine::FriendPlayer {
    FriendReader reader;
    ssdsp::Smoother mon, str, panL, panR, direct;
    std::vector<float> out[2], tap[2];
    DelayLine dl;                                  // the viewers' side, delayed by D - own delay
    uint32_t id = 0;
    bool live = false;
    float peak = 0;
};

HubEngine::HubEngine() { token_ = randomToken(); }

HubEngine::~HubEngine() { disconnect(); }

void HubEngine::friendsPrepare() {
    delayCap_ = uint32_t(1.5 * sampleRate_) + 4u * uint32_t(maxBlock_) + 64u;
    for (int c = 0; c < 2; ++c) {
        fmix_[c].assign(size_t(maxBlock_), 0.0f);
        fmixOld_[c].assign(size_t(maxBlock_), 0.0f);
        delayScratch_[c].assign(size_t(maxBlock_), 0.0f);
    }
    for (auto& d : outDelay_) d.assign(delayCap_);
    for (int j = 0; j < kMaxFriends; ++j) {
        auto p = std::make_unique<FriendPlayer>();
        p->reader.prepare(sampleRate_, maxBlock_);
        for (int c = 0; c < 2; ++c) {
            p->out[c].assign(size_t(maxBlock_), 0.0f);
            p->tap[c].assign(size_t(maxBlock_), 0.0f);
        }
        p->dl.assign(delayCap_);
        p->mon.prepare(sampleRate_, 10.0);
        p->str.prepare(sampleRate_, 20.0);
        p->panL.prepare(sampleRate_, 20.0); p->panL.snap(1.0f);
        p->panR.prepare(sampleRate_, 20.0); p->panR.snap(1.0f);
        p->direct.prepare(sampleRate_, 20.0); p->direct.snap(1.0f);
        friends_[size_t(j)] = std::move(p);
        friendInfo_[size_t(j)].playing.store(0);
    }
    lineD_ = lineOldD_ = 0;
    lineXfRemain_ = lineHold_ = 0;
    usedTot_.fill(0);
    oldTot_.fill(0);
    lineUpMsBits_.store(0);
    lineUpSlowest_.store(-1);
}

void HubEngine::friendsReset() noexcept {
    for (auto& p : friends_) {
        if (!p) continue;
        p->reader.reset();
        p->live = false;
        p->mon.snap(0.0f);
        p->str.snap(0.0f);
        p->peak = 0.0f;
    }
    lineD_ = lineOldD_ = 0;
    lineXfRemain_ = lineHold_ = 0;
    lineUpMsBits_.store(0, std::memory_order_relaxed);
}

bool HubEngine::friendsSoloActive() const noexcept {
    BusLayout* b = bus_.load(std::memory_order_relaxed);
    if (!b) return false;
    for (int j = 0; j < kMaxFriends; ++j)
        if (b->friends[j].state.load(std::memory_order_relaxed) == kFriendLive
            && (friendCtl_[size_t(j)].flags.load(std::memory_order_relaxed) & kFriendSolo)
            && (friendCtl_[size_t(j)].flags.load(std::memory_order_relaxed) & kFriendStr))
            return true;
    return false;
}

void HubEngine::friendsMix(int n, const HubParams& p, std::array<bool, kNumStreamOuts>& used, bool soloActive) noexcept {
    BusLayout* b = bus_.load(std::memory_order_relaxed);
    const uint32_t sr = uint32_t(sampleRate_ + 0.5);
    const double dt = double(n) / sampleRate_;
    if (friends_[0] == nullptr) return;

    // ---- what friends hear: the Stream Mix as the DAW made it, never delayed --------------------
    {
        const float* src[2] = { outs_[0][0].data(), outs_[0][1].data() };
        ringWrite(b->friendMixAudio, b->friendMixWrite, src, 2, uint32_t(n));
    }

    // ---- play every friend ---------------------------------------------------------------------
    std::array<bool, kMaxFriends> have{};
    const uint64_t now = nowNs();
    for (int j = 0; j < kMaxFriends; ++j) {
        FriendPlayer& f = *friends_[size_t(j)];
        FriendHeader& h = b->friends[j];
        FriendInfo& info = friendInfo_[size_t(j)];
        const uint32_t id = h.id.load(std::memory_order_acquire);
        const bool liveState = h.state.load(std::memory_order_acquire) == kFriendLive && id != 0
                            && now - h.heartbeatNs.load(std::memory_order_relaxed) < kFriendStaleNs;
        if (id != f.id) { f.id = id; f.reader.reset(); f.mon.snap(0.0f); f.str.snap(0.0f); f.dl.assign(delayCap_); }
        const uint32_t rate = h.sampleRate.load(std::memory_order_relaxed);
        f.live = false;
        if (!liveState) {
            f.reader.reset();
            info.playing.store(0, std::memory_order_relaxed);
            for (int c = 0; c < 2; ++c) std::memset(f.out[c].data(), 0, size_t(n) * sizeof(float));
            f.peak *= std::exp(-float(n) / (0.3f * float(sr)));
            continue;
        }
        float* o[2] = { f.out[0].data(), f.out[1].data() };
        const bool wasPrimed = f.reader.primed();
        const bool ok = f.reader.pull(b->friendAudio[j], h.writePos, rate, o, n);
        if (!wasPrimed && f.reader.primed()) {
            f.mon.snap(0.0f);
            f.str.snap(0.0f);   // no click when the voice (re)starts
        }
        if (!ok) {
            info.playing.store(0, std::memory_order_relaxed);
            continue;
        }
        f.live = true;
        have[size_t(j)] = true;
        const float bufMs = float(f.reader.bufferMs());
        info.bufferMsBits.store(floatBits(bufMs), std::memory_order_relaxed);
        info.playing.store(1, std::memory_order_relaxed);
    }

    // ---- Line up: D = the slowest friend, a crossfade whenever it really changes ------------------
    const bool lineOn = lineUpOn_.load(std::memory_order_acquire);
    const float limitMs = std::min(kMaxFriendMs, ssbus::bitsFloat(lineUpLimitBits_.load(std::memory_order_relaxed)));
    std::array<uint32_t, kMaxFriends> tot{};   // frames: how late each friend is against the music (own delay + the Hub's buffer)
    uint32_t maxTot = 0;
    int slowest = -1;
    for (int j = 0; j < kMaxFriends; ++j) {
        FriendInfo& info = friendInfo_[size_t(j)];
        if (!have[size_t(j)]) { info.overLimit.store(0, std::memory_order_relaxed); continue; }
        const float d = bitsFloat(b->friends[j].delayBits.load(std::memory_order_relaxed));
        const float tms = std::max(0.0f, d) + bitsFloat(info.bufferMsBits.load(std::memory_order_relaxed));
        info.totalMsBits.store(floatBits(tms), std::memory_order_relaxed);
        tot[size_t(j)] = uint32_t(tms * float(sampleRate_) * 0.001f + 0.5f);
        const bool measured = d >= 0.0f;
        const bool over = tms > limitMs;
        info.overLimit.store(over ? 1 : 0, std::memory_order_relaxed);
        if (lineOn && measured && !over && tot[size_t(j)] >= maxTot) { maxTot = tot[size_t(j)]; slowest = j; }
    }
    const uint32_t capD = delayCap_ - 4u * uint32_t(maxBlock_) - 32u;
    const uint32_t target = lineOn ? std::min(maxTot, capD) : 0u;
    lineUpSlowest_.store(lineOn ? slowest : -1, std::memory_order_relaxed);
    {
        const uint32_t tol = std::max<uint32_t>(uint32_t(0.008 * sampleRate_), target / 20u);   // 8 ms or 5 %
        bool changed = (target > lineD_ ? target - lineD_ : lineD_ - target) > tol;
        if (!lineOn && lineD_ != 0) changed = true;   // switched off: back to live
        for (int j = 0; j < kMaxFriends && !changed; ++j) {
            if (!have[size_t(j)]) continue;
            const uint32_t a = tot[size_t(j)], u = usedTot_[size_t(j)];
            if ((a > u ? a - u : u - a) > uint32_t(0.008 * sampleRate_)) changed = true;
        }
        const bool urgent = !lineOn;
        if (changed && lineXfRemain_ == 0) {
            lineHold_ += uint32_t(n);
            if (urgent || lineHold_ >= uint32_t(0.5 * sampleRate_)) {   // stable for half a second
                lineHold_ = 0;
                lineOldD_ = lineD_;
                oldTot_ = usedTot_;
                lineD_ = target;
                usedTot_ = tot;
                lineXfRemain_ = std::max<uint32_t>(16u, uint32_t(0.020 * sampleRate_));
                lineUpMsBits_.store(floatBits(float(double(lineD_) * 1000.0 / sampleRate_)), std::memory_order_relaxed);
                lineUpChanges_.fetch_add(1, std::memory_order_relaxed);
            }
        } else if (!changed) {
            lineHold_ = 0;
        }
    }
    const uint32_t xfTotal = std::max<uint32_t>(16u, uint32_t(0.020 * sampleRate_));
    const uint32_t xfStart = lineXfRemain_;   // crossfade progress at the start of this block

    // ---- the Stream Mix and the stems wait D (history is always written, so D can start any time) -
    auto delayOut = [&](int o) {
        DelayLine& d = outDelay_[size_t(o)];
        const uint32_t cap = uint32_t(d.buf[0].size());
        for (int c = 0; c < 2; ++c) {
            float* x = outs_[size_t(o)][size_t(c)].data();
            for (int i = 0; i < n; ++i) { d.buf[size_t(c)][(d.pos + uint32_t(i)) % cap] = x[i]; }
        }
        const uint32_t endPos = (d.pos + uint32_t(n)) % cap;
        for (int c = 0; c < 2; ++c) {
            float* x = outs_[size_t(o)][size_t(c)].data();
            if (lineD_ == 0 && xfStart == 0) continue;   // live: the block is already right
            readTap(d.buf[size_t(c)], endPos, uint32_t(n), lineD_, delayScratch_[0].data());
            if (xfStart > 0) {
                readTap(d.buf[size_t(c)], endPos, uint32_t(n), lineOldD_, delayScratch_[1].data());
                for (int i = 0; i < n; ++i) {
                    const uint32_t done = xfTotal - std::min(xfTotal, xfStart) + uint32_t(i);
                    const float a = std::min(1.0f, float(done) / float(xfTotal));
                    x[i] = delayScratch_[1][size_t(i)] + (delayScratch_[0][size_t(i)] - delayScratch_[1][size_t(i)]) * a;
                }
            } else {
                std::memcpy(x, delayScratch_[0].data(), size_t(n) * sizeof(float));
            }
        }
        d.pos = endPos;
    };
    for (int o = 0; o < kNumStreamOuts; ++o) if (used[size_t(o)]) delayOut(o);
    lineXfRemain_ = xfStart > uint32_t(n) ? xfStart - uint32_t(n) : 0;

    // ---- mix every friend ----------------------------------------------------------------------
    float* phoneL = phones_[0].data();
    float* phoneR = phones_[1].data();
    float* sL = outs_[0][0].data();
    float* sR = outs_[0][1].data();
    const float pkDecay = std::exp(-float(n) / (0.3f * float(sr)));
    for (int j = 0; j < kMaxFriends; ++j) {
        FriendPlayer& f = *friends_[size_t(j)];
        FriendHeader& h = b->friends[j];
        FriendControl& ctl = friendCtl_[size_t(j)];
        const bool live = f.live;
        const uint32_t flags = ctl.flags.load(std::memory_order_relaxed);
        const float gdb = bitsFloat(ctl.gainBits.load(std::memory_order_relaxed));
        const float g = ssdsp::dbToGain(gdb);
        // direct path only while the Hub plays this friend (not while a DAW track carries them)
        const bool direct = h.route.load(std::memory_order_relaxed) == kRouteDirect;
        f.direct.setTarget(direct ? 1.0f : 0.0f);
        f.mon.setTarget(live && (flags & kFriendMon) ? g : 0.0f);
        float gv = live && (flags & kFriendStr) ? g : 0.0f;
        if (soloActive && !(flags & kFriendSolo)) gv = 0.0f;
        if (p.panic) gv = 0.0f;
        f.str.setTarget(gv);
        float gl = 1.0f, gr = 1.0f;
        ssdsp::panGains(bitsFloat(ctl.panBits.load(std::memory_order_relaxed)), gl, gr);
        f.panL.setTarget(gl);
        f.panR.setTarget(gr);

        const bool silentNow = !live && !f.mon.isSmoothing() && !f.str.isSmoothing() && f.mon.current() == 0.0f && f.str.current() == 0.0f;
        // the friend's own history goes into its delay line even while the viewers don't hear it (it can start any time)
        DelayLine& d = f.dl;
        const uint32_t cap = uint32_t(d.buf[0].size());
        float pk = 0.0f;
        if (!silentNow) {
            for (int i = 0; i < n; ++i) {
                const float dg = f.direct.next();
                const float gm = f.mon.next() * dg, gs = f.str.next() * dg;
                const float l = f.out[0][size_t(i)], r = f.out[1][size_t(i)];
                phoneL[i] += l * gm;
                phoneR[i] += r * gm;
                const float vl = l * gs * f.panL.next(), vr = r * gs * f.panR.next();
                d.buf[0][(d.pos + uint32_t(i)) % cap] = vl;
                d.buf[1][(d.pos + uint32_t(i)) % cap] = vr;
                pk = std::max(pk, std::max(std::abs(vl), std::abs(vr)));
            }
        } else {
            for (int i = 0; i < n; ++i) { d.buf[0][(d.pos + uint32_t(i)) % cap] = 0.0f; d.buf[1][(d.pos + uint32_t(i)) % cap] = 0.0f; f.direct.next(); f.panL.next(); f.panR.next(); }
        }
        const uint32_t endPos = (d.pos + uint32_t(n)) % cap;
        const uint32_t dNew = lineD_ > usedTot_[size_t(j)] ? lineD_ - usedTot_[size_t(j)] : 0u;
        const uint32_t dOld = lineOldD_ > oldTot_[size_t(j)] ? lineOldD_ - oldTot_[size_t(j)] : 0u;
        friendInfo_[size_t(j)].lineMsBits.store(floatBits(float(double(dNew) * 1000.0 / sampleRate_)), std::memory_order_relaxed);
        for (int c = 0; c < 2; ++c) {
            float* dst = c == 0 ? sL : sR;
            readTap(d.buf[size_t(c)], endPos, uint32_t(n), dNew, delayScratch_[0].data());
            if (xfStart > 0) {
                readTap(d.buf[size_t(c)], endPos, uint32_t(n), dOld, delayScratch_[1].data());
                for (int i = 0; i < n; ++i) {
                    const uint32_t done = xfTotal - std::min(xfTotal, xfStart) + uint32_t(i);
                    const float a = std::min(1.0f, float(done) / float(xfTotal));
                    dst[i] += delayScratch_[1][size_t(i)] + (delayScratch_[0][size_t(i)] - delayScratch_[1][size_t(i)]) * a;
                }
            } else {
                for (int i = 0; i < n; ++i) dst[i] += delayScratch_[0][size_t(i)];
            }
        }
        d.pos = endPos;
        f.peak = std::max(pk, f.peak * pkDecay);
    }
    (void) used;
}

} // namespace ssengine
