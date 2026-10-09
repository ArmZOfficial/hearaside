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
    std::unique_ptr<ssdsp::VarResampler> rsSame, rs48, rs441;
    ssdsp::DriftController drift;
    ssdsp::Smoother mon, str, panL, panR, direct;
    std::vector<float> in[2], out[2], tap[2];
    DelayLine dl;                                  // the viewers' side, delayed by D - own delay
    uint64_t cursor = 0, lastWrite = 0, starvedAt = 0;
    uint32_t key = 0, id = 0;
    bool primed = false, live = false;
    double target = 0, floorF = 0, fillAvg = 0, sinceWrite = 0, minMargin = 1.0e9, windowSec = 0, holdSec = 0;
    float peak = 0;
};

HubEngine::HubEngine() { token_ = randomToken(); }

HubEngine::~HubEngine() { disconnect(); }

void HubEngine::friendsPrepare() {
    const uint32_t r = uint32_t(sampleRate_ + 0.5);
    delayCap_ = uint32_t(1.5 * sampleRate_) + 4u * uint32_t(maxBlock_) + 64u;
    for (int c = 0; c < 2; ++c) {
        fmix_[c].assign(size_t(maxBlock_), 0.0f);
        fmixOld_[c].assign(size_t(maxBlock_), 0.0f);
        delayScratch_[c].assign(size_t(maxBlock_), 0.0f);
    }
    for (auto& d : outDelay_) d.assign(delayCap_);
    for (int j = 0; j < kMaxFriends; ++j) {
        auto p = std::make_unique<FriendPlayer>();
        p->rsSame = std::make_unique<ssdsp::VarResampler>(r, r, 4);
        p->rs48 = std::make_unique<ssdsp::VarResampler>(48000, r, 4);
        p->rs441 = std::make_unique<ssdsp::VarResampler>(44100, r, 4);
        for (int c = 0; c < 2; ++c) {
            p->in[c].assign(size_t(maxBlock_) * 4 + 128, 0.0f);
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
        p->primed = false;
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
        if (id != f.id) { f.id = id; f.primed = false; f.mon.snap(0.0f); f.str.snap(0.0f); f.dl.assign(delayCap_); }
        const uint32_t rate = h.sampleRate.load(std::memory_order_relaxed);
        ssdsp::VarResampler* rs = rate == sr ? f.rsSame.get() : rate == 48000 ? f.rs48.get() : rate == 44100 ? f.rs441.get() : nullptr;
        f.live = false;
        if (!liveState || rs == nullptr) {
            f.primed = false;
            info.playing.store(0, std::memory_order_relaxed);
            for (int c = 0; c < 2; ++c) std::memset(f.out[c].data(), 0, size_t(n) * sizeof(float));
            f.peak *= std::exp(-float(n) / (0.3f * float(sr)));
            continue;
        }
        const double fr = double(rate);
        const uint64_t w = h.writePos.load(std::memory_order_acquire);
        if (f.key != rate) {   // first block or the browser changed its rate: safe start with room for the network
            f.key = rate;
            const double block = double(maxBlock_) * fr / sampleRate_;
            const double packet = fr * 0.03;
            f.target = block + packet + fr * 0.04;
            f.floorF = block + 32.0;
            f.holdSec = 0.0;
            f.primed = false;
            f.starvedAt = f.lastWrite = w;
        }
        f.sinceWrite = w != f.lastWrite ? 0.0 : f.sinceWrite + dt;
        f.lastWrite = w;
        const bool arriving = f.sinceWrite < 0.09 + dt;
        double fill = double(int64_t(w - f.cursor));
        const double maxFill = double(kRingFrames - kGuardFrames);
        bool ok = true;
        if (!f.primed || fill < 0.0 || fill > std::min(maxFill, f.target * 2.0 + fr * 0.05)) {
            if (double(w) < double(f.starvedAt) + f.target + double(n)) ok = false;
            else {
                f.cursor = w - uint64_t(f.target);
                f.drift.reset(f.target);
                rs->reset();
                f.primed = true;
                fill = f.target;
                f.mon.snap(0.0f);
                f.str.snap(0.0f);   // no click when the voice (re)starts
                f.fillAvg = f.target;
                f.minMargin = 1.0e9;
                f.windowSec = 0.0;
            }
        }
        const double need = ok ? double(rs->inputFor(uint32_t(n))) : 0.0;
        if (ok && fill < need) {
            if (arriving) {   // packets come but not fast enough: this line needs more headroom
                f.target = std::min(f.target + fr * 0.002, double(maxBlock_) * fr / sampleRate_ + fr * 0.4);
                f.floorF = f.target;
                f.holdSec = 5.0;
            }
            f.primed = false;
            f.starvedAt = w;
            ok = false;
        }
        if (ok) {   // give back unused headroom every 3 s
            f.fillAvg += (fill - f.fillAvg) * (1.0 - std::exp(-dt / 0.3));
            f.minMargin = std::min(f.minMargin, fill - need);
            f.windowSec += dt;
            f.holdSec -= dt;
            if (f.windowSec >= 3.0) {
                const double spare = f.minMargin - fr * 0.002;
                if (f.holdSec <= 0.0 && spare > fr * 0.0002 && std::abs(f.fillAvg - f.target) < fr * 0.0005) {
                    f.target = std::max(std::max(f.floorF, double(n) * fr / sampleRate_ + 32.0), f.target - std::min(spare * 0.5, fr * 0.001));
                    f.drift.setTarget(f.target);
                }
                f.windowSec = 0.0;
                f.minMargin = 1.0e9;
            }
            const double c = f.drift.update(fill, dt);
            rs->setCorrectionPpm(int(std::lround(c * 1.0e6)));
            float* in[2] = { f.in[0].data(), f.in[1].data() };
            uint32_t made = 0;
            for (int pass = 0; pass < 3 && made < uint32_t(n); ++pass) {
                const uint32_t want = std::min<uint32_t>(rs->inputFor(uint32_t(n) - made), uint32_t(f.in[0].size()));
                uint32_t avail = 0;
                if (ringReadAt(b->friendAudio[j], h.writePos, f.cursor, in, want, &avail) == ReadResult::Overrun) { f.primed = false; f.starvedAt = w; ok = false; break; }
                if (avail == 0) break;
                uint32_t usedIn = avail, got = uint32_t(n) - made;
                float* o[2] = { f.out[0].data() + made, f.out[1].data() + made };
                rs->process(in, usedIn, o, got);
                f.cursor += usedIn;
                made += got;
                if (usedIn == 0 && got == 0) break;
            }
            if (ok) for (int c2 = 0; c2 < 2; ++c2) if (made < uint32_t(n)) std::memset(f.out[c2].data() + made, 0, (uint32_t(n) - made) * sizeof(float));
        }
        if (!ok) {
            for (int c = 0; c < 2; ++c) std::memset(f.out[c].data(), 0, size_t(n) * sizeof(float));
            info.playing.store(0, std::memory_order_relaxed);
            continue;
        }
        f.live = true;
        have[size_t(j)] = true;
        const float bufMs = float(f.fillAvg * 1000.0 / fr);
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
