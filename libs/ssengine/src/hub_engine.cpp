// SPDX-License-Identifier: MIT
#include "ssengine/engine.h"

#include <cmath>
#include <cstring>

namespace ssengine {

using namespace ssbus;

namespace {
constexpr uint64_t kHubStaleNs  = 2000000000ull;   // another Hub may take over after 2 s silence
constexpr uint64_t kSlotStaleNs = 5000000000ull;
}

HubEngine::HubEngine() { token_ = randomToken(); }

HubEngine::~HubEngine() { disconnect(); }

bool HubEngine::connect(const std::string& busName) {
    disconnect();
    busName_ = busName.empty() ? std::string("Main") : busName;
    SharedMemory::Status s{};
    auto shm = SharedMemory::open(busName_, s);
    if (!shm) return false;
    shm_ = std::move(shm);
    bus_.store(&shm_->layout(), std::memory_order_release);
    gen_.fetch_add(1, std::memory_order_release);
    maintain();
    return true;
}

void HubEngine::disconnect() {
    BusLayout* b = bus_.exchange(nullptr, std::memory_order_acq_rel);
    if (b) releaseHub(*b, token_);
    if (shm_) retired_.push_back(std::move(shm_));
    role_.store(Role::Disconnected, std::memory_order_release);
}

void HubEngine::maintain() {
    BusLayout* b = bus();
    if (!b) { role_.store(Role::Disconnected, std::memory_order_release); return; }
    const bool owner = tryClaimHub(*b, token_, kHubStaleNs);
    role_.store(owner ? Role::Owner : Role::Secondary, std::memory_order_release);
    if (owner) reclaimDeadSlots(*b, kSlotStaleNs);
}

void HubEngine::prepare(double sampleRate, int maxBlock) {
    sampleRate_ = sampleRate > 0 ? sampleRate : 48000.0;
    maxBlock_ = std::max(16, std::min(maxBlock, int(kRingFrames / 8)));
    for (int c = 0; c < 2; ++c) {
        tmpA_[c].assign(size_t(maxBlock_), 0.0f);
        tmpB_[c].assign(size_t(maxBlock_), 0.0f);
        for (auto& o : outs_) o[c].assign(size_t(maxBlock_), 0.0f);
    }
    for (auto& l : limiters_) l.prepare(sampleRate_, 1.0, 120.0);
    loudness_.prepare(sampleRate_);
    master_.prepare(sampleRate_, 20.0);
    preview_.prepare(sampleRate_, 20.0);
    xfLen_ = uint32_t(std::max(16.0, sampleRate_ * 0.010));
    for (auto& s : slots_) {
        s = SlotState{};
        s.gain.prepare(sampleRate_, 20.0);
        s.panL.prepare(sampleRate_, 20.0); s.panL.snap(1.0f);
        s.panR.prepare(sampleRate_, 20.0); s.panR.snap(1.0f);
    }
    outUsed_.fill(false);
    wasPlaying_ = false;
    expectedTime_ = kNoTime;
}

void HubEngine::setOutputName(int output, const std::string& name) {
    BusLayout* b = bus();
    if (!b || output < 0 || output >= kNumStreamOuts) return;
    auto& sh = b->streamHeader;
    sh.nameSeq.fetch_add(1, std::memory_order_relaxed);
    std::atomic_thread_fence(std::memory_order_release);
    size_t n = std::min(name.size(), size_t(kNameBytes - 1));
    while (n > 0 && n < name.size() && (static_cast<unsigned char>(name[n]) & 0xC0) == 0x80) --n;
    std::memset(sh.names[output], 0, kNameBytes);
    std::memcpy(sh.names[output], name.data(), n);
    sh.nameSeq.fetch_add(1, std::memory_order_release);
}

int HubEngine::latencyFrames() const noexcept { return limiters_[0].latency(); }

void HubEngine::startCrossfade(SlotState& s, uint64_t oldReadPos) noexcept {
    s.xfFrom = oldReadPos;
    s.xfRemain = xfLen_;
    s.xfActive = true;
}

uint64_t HubEngine::anchorFor(int i, int n, int64_t timeSamples, bool useTimeline, bool& viaTimeline) noexcept {
    BusLayout* b = bus_.load(std::memory_order_relaxed);
    SlotHeader& sh = b->slots[i];
    if (useTimeline) {
        const TagHit hit = findTimelinePosition(sh, b->slotTags[i], timeSamples);
        if (hit.found) { viaTimeline = true; return hit.fifoPos; }
    }
    viaTimeline = false;
    const uint64_t w = sh.writePos.load(std::memory_order_acquire);
    const uint64_t back = uint64_t(n) * uint64_t(1 + std::clamp(syncSafety_, 0, 2));
    return w > back ? w - back : 0;
}

void HubEngine::process(float* const* io, int numCh, int n, const HubParams& p,
                        int64_t timeSamples, bool playing, bool offline) noexcept {
    BusLayout* b = bus_.load(std::memory_order_acquire);
    if (b == nullptr || maxBlock_ == 0 || n <= 0) return;
    BusHeader& h = b->header;

    const bool owner = h.hubOwnerToken.load(std::memory_order_acquire) == token_;
    if (!owner) { wasOwner_ = false; return; }   // secondary Hub: pass through only

    const uint32_t gen = gen_.load(std::memory_order_acquire);
    if (!wasOwner_ || gen != seenGen_) {
        seenGen_ = gen;
        wasOwner_ = true;
        for (auto& s : slots_) { s.known = false; s.needAnchor = true; s.xfActive = false; }
        for (auto& l : limiters_) l.reset();
        loudness_.reset();
    }

    const uint32_t sr = uint32_t(sampleRate_ + 0.5);
    h.hubHeartbeatNs.store(nowNs(), std::memory_order_relaxed);
    h.hubSampleRate.store(sr, std::memory_order_relaxed);
    h.hubBlockSize.store(uint32_t(n), std::memory_order_relaxed);
    syncSafety_ = std::clamp(p.syncSafety, 0, 2);
    StreamInsert* ins = insert_.load(std::memory_order_acquire);
    h.hubLatencyFrames.store(uint32_t(syncSafety_ * n + limiters_[0].latency() + (ins ? std::max(0, ins->latencyFrames()) : 0)),
                             std::memory_order_relaxed);
    const uint32_t flags = (p.preview ? kHubPreview : 0u) | (p.panic ? kHubPanic : 0u)
                         | (offline ? kHubOffline : 0u) | (p.bypassed ? kHubBypassed : 0u)
                         | (p.limiterOn ? kHubLimiter : 0u);
    h.hubFlags.store(flags, std::memory_order_relaxed);

    if (offline || p.bypassed) {
        b->streamHeader.active.store(0, std::memory_order_release);
        wasOffline_ = true;
        return;
    }
    if (wasOffline_) {
        wasOffline_ = false;
        for (auto& s : slots_) s.needAnchor = true;
    }
    b->streamHeader.sampleRate.store(sr, std::memory_order_relaxed);

    for (int off = 0; off < n; off += maxBlock_) {
        const int m = std::min(maxBlock_, n - off);
        float* chunk[kMaxChannels] = { nullptr, nullptr };
        for (int c = 0; c < std::min(numCh, kMaxChannels); ++c) chunk[c] = io[c] + off;
        processChunk(chunk, std::min(numCh, kMaxChannels), m, p,
                     timeSamples == kNoTime ? kNoTime : timeSamples + off, playing);
    }
    b->streamHeader.active.store(1, std::memory_order_release);
    ++blockCounter_;
}

void HubEngine::processChunk(float* const* io, int numCh, int n, const HubParams& p,
                             int64_t timeSamples, bool playing) noexcept {
    BusLayout* b = bus_.load(std::memory_order_relaxed);
    const uint32_t sr = uint32_t(sampleRate_ + 0.5);
    const bool hasTime = timeSamples != kNoTime;
    const bool useTimeline = playing && hasTime;
    // With sync safety the Hub mixes "L samples in the past", so tracks that the host processes
    // after the master bus (or on another thread) have already published that part of the timeline.
    const int64_t mixTime = hasTime ? timeSamples - int64_t(syncSafety_) * n : kNoTime;

    // ---- transport events -> re-anchor every slot ----------------------------------------
    bool reanchorAll = false;
    if (playing != wasPlaying_) reanchorAll = true;
    else if (useTimeline && expectedTime_ != kNoTime && timeSamples != expectedTime_) reanchorAll = true;
    wasPlaying_ = playing;
    expectedTime_ = useTimeline ? timeSamples + n : kNoTime;

    for (auto& o : outs_) for (auto& c : o) std::memset(c.data(), 0, size_t(n) * sizeof(float));
    std::array<bool, kNumStreamOuts> used{};
    used[0] = true;

    // ---- solo ------------------------------------------------------------------------------
    bool soloActive = false;
    for (int i = 0; i < kMaxSlots; ++i) {
        const SlotHeader& sh = b->slots[i];
        if (sh.state.load(std::memory_order_acquire) != kSlotActive) continue;
        const uint32_t f = sh.flags.load(std::memory_order_relaxed);
        if ((f & kFlagSolo) && (f & kFlagStr) && sh.sampleRate.load(std::memory_order_relaxed) == sr) { soloActive = true; break; }
    }

    const uint32_t maxDelay = std::min<uint32_t>(uint32_t(0.5 * sampleRate_ + 0.5),
                                                 kRingFrames - kGuardFrames - 4u * uint32_t(maxBlock_));
    const uint64_t aheadThreshold = uint64_t(syncSafety_ * n) + uint64_t(std::max<double>(2.0 * n, 0.020 * sampleRate_));
    const float pkDecay = std::exp(-float(n) / (0.3f * float(sr)));
    float* A[2] = { tmpA_[0].data(), tmpA_[1].data() };
    float* B[2] = { tmpB_[0].data(), tmpB_[1].data() };

    for (int i = 0; i < kMaxSlots; ++i) {
        SlotHeader& sh = b->slots[i];
        SlotState& s = slots_[i];
        if (sh.state.load(std::memory_order_acquire) != kSlotActive) {
            if (s.known) { s.known = false; s.needAnchor = true; s.xfActive = false; s.gain.snap(0.0f); }
            continue;
        }
        const uint32_t epoch = sh.epoch.load(std::memory_order_acquire);
        const uint64_t claimNs = sh.claimNs.load(std::memory_order_relaxed);
        bool fresh = false;
        if (!s.known || epoch != s.epoch || claimNs != s.claimNs) {
            s.known = true; s.epoch = epoch; s.claimNs = claimNs;
            s.needAnchor = true; s.xfActive = false; s.mismatch = 0; s.aheadBlocks = 0;
            s.gain.snap(0.0f);   // fade in from silence
            s.delay = 0;
            fresh = true;
        }
        if (sh.sampleRate.load(std::memory_order_relaxed) != sr) {
            sh.hubStatus.store(kHubStatusRateMismatch, std::memory_order_relaxed);
            sh.peakStreamBits[0].store(0, std::memory_order_relaxed);
            sh.peakStreamBits[1].store(0, std::memory_order_relaxed);
            s.needAnchor = true;
            continue;
        }
        const uint32_t f = sh.flags.load(std::memory_order_acquire);
        uint32_t status = kHubStatusOk;

        // ---- anchoring / timeline lock ------------------------------------------------------
        if (s.needAnchor || reanchorAll) {
            bool viaTl = false;
            const uint64_t c = anchorFor(i, n, mixTime, useTimeline, viaTl);
            if (!fresh && !s.needAnchor && s.gain.current() > 0.0f) startCrossfade(s, s.cursor >= s.delay ? s.cursor - s.delay : 0);
            s.cursor = c;
            s.timelineLocked = viaTl;
            s.needAnchor = false;
            s.mismatch = 0;
        } else if (useTimeline && (blockCounter_ & 3u) == 0) {
            const TagHit hit = findTimelinePosition(sh, b->slotTags[i], mixTime, 256);
            s.timelineLocked = hit.found;
            if (hit.found && hit.fifoPos != s.cursor) {
                if (++s.mismatch >= 2) {
                    startCrossfade(s, s.cursor >= s.delay ? s.cursor - s.delay : 0);
                    s.cursor = hit.fifoPos;
                    s.mismatch = 0;
                }
            } else {
                s.mismatch = 0;
            }
        } else if (!useTimeline) {
            s.timelineLocked = false;
        }

        // ---- delay (read further back in the FIFO, no extra memory) -------------------------
        const float delayMs = bitsFloat(sh.strDelayBits.load(std::memory_order_relaxed));
        const uint32_t targetDelay = std::min<uint32_t>(maxDelay, uint32_t(std::max(0.0f, delayMs) * float(sampleRate_) * 0.001f + 0.5f));
        if (targetDelay != s.delay) {
            if (!fresh) startCrossfade(s, s.cursor >= s.delay ? s.cursor - s.delay : 0);
            s.delay = targetDelay;
        }

        const uint64_t readPos = s.cursor >= s.delay ? s.cursor - s.delay : 0;
        const ReadResult res = ringReadAt(b->slotAudio[i], sh.writePos, readPos, A, uint32_t(n));
        if (s.xfActive) {
            ringReadAt(b->slotAudio[i], sh.writePos, s.xfFrom, B, uint32_t(n));
            const uint32_t done = xfLen_ - s.xfRemain;
            for (int j = 0; j < n; ++j) {
                const float a = std::min(1.0f, float(done + uint32_t(j)) / float(xfLen_));
                A[0][j] = B[0][j] + (A[0][j] - B[0][j]) * a;
                A[1][j] = B[1][j] + (A[1][j] - B[1][j]) * a;
            }
            s.xfFrom += uint64_t(n);
            s.xfRemain = s.xfRemain > uint32_t(n) ? s.xfRemain - uint32_t(n) : 0;
            if (s.xfRemain == 0) s.xfActive = false;
        }
        s.cursor += uint64_t(n);

        const uint64_t w = sh.writePos.load(std::memory_order_acquire);
        const int64_t lead = int64_t(w - s.cursor);
        if (res == ReadResult::Overrun) {
            s.needAnchor = true;
            status |= kHubStatusUnderrun;
        } else if (lead < 0) {
            // The track was not processed this cycle (suspended / smart-disabled / frozen).
            s.needAnchor = true;
            status |= kHubStatusUnderrun;
        }
        if (!s.timelineLocked && lead > int64_t(aheadThreshold)) {
            s.aheadBlocks += n;
            if (s.aheadBlocks > int(sr)) status |= kHubStatusAhead;
        } else {
            s.aheadBlocks = 0;
        }
        sh.hubLeadFrames.store(uint32_t(std::max<int64_t>(0, std::min<int64_t>(lead, INT32_MAX))), std::memory_order_relaxed);

        // ---- gain / pan / solo / panic -------------------------------------------------------
        float g = (f & kFlagStr) ? ssdsp::dbToGain(bitsFloat(sh.strGainBits.load(std::memory_order_relaxed))) : 0.0f;
        if (soloActive && !(f & kFlagSolo)) g = 0.0f;
        if (p.panic) g = 0.0f;
        s.gain.setTarget(g);
        float gl = 1.0f, gr = 1.0f;
        ssdsp::panGains(bitsFloat(sh.strPanBits.load(std::memory_order_relaxed)), gl, gr);
        s.panL.setTarget(gl);
        s.panR.setTarget(gr);

        const int stem = sh.stemIndex.load(std::memory_order_relaxed);
        const int outIdx = (stem >= 0 && stem < kMaxStems) ? 1 + stem : -1;
        if (outIdx > 0) used[size_t(outIdx)] = true;

        float pk0 = 0.0f, pk1 = 0.0f;
        const bool silent = !s.gain.isSmoothing() && s.gain.current() == 0.0f;
        if (!silent) {
            float* m0 = outs_[0][0].data();
            float* m1 = outs_[0][1].data();
            float* s0 = outIdx > 0 ? outs_[outIdx][0].data() : nullptr;
            float* s1 = outIdx > 0 ? outs_[outIdx][1].data() : nullptr;
            for (int j = 0; j < n; ++j) {
                const float gg = s.gain.next();
                const float l = A[0][j] * gg * s.panL.next();
                const float r = A[1][j] * gg * s.panR.next();
                m0[j] += l; m1[j] += r;
                if (s0) { s0[j] += l; s1[j] += r; }
                pk0 = std::max(pk0, std::abs(l));
                pk1 = std::max(pk1, std::abs(r));
            }
        } else {
            // keep pan smoothers in sync even while silent
            for (int j = 0; j < n; ++j) { s.panL.next(); s.panR.next(); }
        }
        s.peak[0] = std::max(pk0, s.peak[0] * pkDecay);
        s.peak[1] = std::max(pk1, s.peak[1] * pkDecay);
        sh.peakStreamBits[0].store(floatBits(s.peak[0]), std::memory_order_relaxed);
        sh.peakStreamBits[1].store(floatBits(s.peak[1]), std::memory_order_relaxed);
        sh.hubStatus.store(status, std::memory_order_relaxed);
    }

    // ---- master gain, limiter, meters, write ------------------------------------------------
    master_.setTarget(p.panic ? 0.0f : ssdsp::dbToGain(p.masterDb));
    float* mg = tmpB_[0].data();
    for (int j = 0; j < n; ++j) mg[j] = master_.next();

    const float ceiling = ssdsp::dbToGain(std::clamp(p.ceilingDb, -24.0f, 0.0f));
    auto& sh = b->streamHeader;
    for (int o = 0; o < kNumStreamOuts; ++o) {
        float* l = outs_[o][0].data();
        float* r = outs_[o][1].data();
        if (used[size_t(o)]) {
            if (!outUsed_[size_t(o)]) limiters_[size_t(o)].reset();
            for (int j = 0; j < n; ++j) { l[j] *= mg[j]; r[j] *= mg[j]; }
            if (o == 0) {
                if (StreamInsert* ins = insert_.load(std::memory_order_acquire)) ins->processStream(l, r, n);
            }
            limiters_[size_t(o)].setCeiling(ceiling);
            limiters_[size_t(o)].process(l, r, n, p.limiterOn);
        }
        outUsed_[size_t(o)] = used[size_t(o)];
        const float* src[2] = { l, r };
        ringWrite(b->streamAudio[o], sh.writePos[o], used[size_t(o)] ? src : nullptr, 2, uint32_t(n));

        const float decay = std::exp(-float(n) / (0.3f * float(sr)));
        outPeak_[o][0] = std::max(used[size_t(o)] ? ssdsp::blockPeak(l, n) : 0.0f, outPeak_[o][0] * decay);
        outPeak_[o][1] = std::max(used[size_t(o)] ? ssdsp::blockPeak(r, n) : 0.0f, outPeak_[o][1] * decay);
        sh.peakBits[o][0].store(floatBits(outPeak_[o][0]), std::memory_order_relaxed);
        sh.peakBits[o][1].store(floatBits(outPeak_[o][1]), std::memory_order_relaxed);
    }
    loudness_.process(outs_[0][0].data(), outs_[0][1].data(), n);
    sh.loudnessMBits.store(floatBits(loudness_.momentary()), std::memory_order_relaxed);
    sh.loudnessSBits.store(floatBits(loudness_.shortTerm()), std::memory_order_relaxed);

    // ---- preview: replace the master output with the Stream Mix (20 ms crossfade) -----------
    preview_.setTarget(p.preview ? 1.0f : 0.0f);
    if (preview_.isSmoothing() || preview_.current() > 0.0f) {
        for (int j = 0; j < n; ++j) {
            const float a = preview_.next();
            for (int c = 0; c < numCh; ++c) {
                const float src = outs_[0][size_t(std::min(c, 1))][size_t(j)];
                io[c][j] = io[c][j] + (src - io[c][j]) * a;
            }
        }
    }
}

} // namespace ssengine
