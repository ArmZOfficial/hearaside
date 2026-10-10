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

// HubEngine::HubEngine / ~HubEngine live in hub_friends.cpp (they need the friend players' full type)

bool HubEngine::connect(const std::string& busName) {
    disconnect();
    busName_ = busName.empty() ? std::string("Main") : busName;
    SharedMemory::Status s{};
    auto shm = SharedMemory::open(busName_, s);
    if (!shm) return false;
    shm_ = std::move(shm);
    beacon_ = BeaconMap::open(busName_);   // best effort: without it only the version hint is missing
    bus_.store(&shm_->layout(), std::memory_order_release);
    gen_.fetch_add(1, std::memory_order_release);
    maintain();
    return true;
}

void HubEngine::disconnect() {
    BusLayout* b = bus_.exchange(nullptr, std::memory_order_acq_rel);
    if (b) releaseHub(*b, token_);
    if (shm_) retired_.push_back(std::move(shm_));
    beacon_.reset();
    role_.store(Role::Disconnected, std::memory_order_release);
}

void HubEngine::maintain() {
    BusLayout* b = bus();
    if (!b) { role_.store(Role::Disconnected, std::memory_order_release); return; }
    const bool owner = tryClaimHub(*b, token_, kHubStaleNs);
    role_.store(owner ? Role::Owner : Role::Secondary, std::memory_order_release);
    if (owner) {
        reclaimDeadSlots(*b, kSlotStaleNs);
        if (beacon_) beaconPublish(beacon_->header());
    }
}

void HubEngine::prepare(double sampleRate, int maxBlock) {
    sampleRate_ = sampleRate > 0 ? sampleRate : 48000.0;
    maxBlock_ = std::max(16, std::min(maxBlock, int(kRingFrames / 8)));
    for (int c = 0; c < 2; ++c) {
        tmpA_[c].assign(size_t(maxBlock_), 0.0f);
        tmpB_[c].assign(size_t(maxBlock_), 0.0f);
        phones_[c].assign(size_t(maxBlock_), 0.0f);
        for (auto& o : outs_) o[c].assign(size_t(maxBlock_), 0.0f);
    }
    {
        const std::lock_guard<std::mutex> lock(measureMutex_);
        capLen_ = uint32_t(sampleRate_ * 0.625) + 1;   // 2.5 s at sample rate / 4
        slotBlock_.assign(size_t(kMaxSlots) * size_t(maxBlock_), 0.0f);
        capSlots_.assign(size_t(kMaxSlots) * capLen_, 0.0f);
        capIn_.assign(capLen_, 0.0f);
        capSources_.assign(size_t(kMaxSources) * capLen_, 0.0f);
        srcKnown_.fill(false);
        decSrc_.fill(0.0f);
        srcQuiet_.fill(capLen_);
        capWrite_.store(0, std::memory_order_release);
        decSlot_.fill(0.0f);
        slotLive_.fill(false);
        quiet_.fill(capLen_);   // the history starts out all zero
        decIn_ = 0.0f;
        decCount_ = 0;
    }
    for (auto& l : limiters_) l.prepare(sampleRate_, 1.0, 120.0);
    loudness_.prepare(sampleRate_);
    master_.prepare(sampleRate_, 20.0);
    preview_.prepare(sampleRate_, 20.0);
    xfLen_ = uint32_t(std::max(16.0, sampleRate_ * 0.010));
    friendsPrepare();
    for (auto& s : slots_) {
        s = SlotState{};
        s.gain.prepare(sampleRate_, 20.0);
        s.panL.prepare(sampleRate_, 20.0); s.panL.snap(1.0f);
        s.panR.prepare(sampleRate_, 20.0); s.panR.snap(1.0f);
        s.mon.prepare(sampleRate_, 10.0);
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

namespace {

// Lag (in history frames, kmin..kmax) at which x best matches y, by normalised correlation over
// y[yStart, yStart + N). cum = prefix sums of x^2. Coarse pass on every 4th lag and sample, then
// a full pass around it and a parabolic refinement. Returns false below minScore.
bool bestLag(const float* y, const float* x, int N, int yStart, int kmin, int kmax,
             const std::vector<double>& cum, double ey, double minScore, double& lag, bool anyPolarity = false,
             double* scoreOut = nullptr, int lagStep = 4) {
    auto score = [&](int k, int step) {
        double c = 0;
        for (int i = 0; i < N; i += step) c += double(y[yStart + i]) * x[yStart + i - k];
        const double ex = cum[size_t(yStart - k + N)] - cum[size_t(yStart - k)];
        const double v = ex > 0 ? c * step / std::sqrt(ex * ey) : 0.0;
        return anyPolarity ? std::abs(v) : v;
    };
    int kc = kmin;
    double bc = -2;
    for (int k = kmin; k <= kmax; k += lagStep)   // whitened signals have a narrow peak: lagStep 1
        if (const double v = score(k, 4); v > bc) { bc = v; kc = k; }
    const int lo = std::max(kmin, kc - 4), hi = std::min(kmax, kc + 4);
    std::vector<double> v(size_t(hi - lo + 1));
    int best = 0;
    for (int k = lo; k <= hi; ++k) {
        v[size_t(k - lo)] = score(k, 1);
        if (v[size_t(k - lo)] > v[size_t(best)]) best = k - lo;
    }
    if (scoreOut) *scoreOut = v[size_t(best)];
    if (v[size_t(best)] < minScore) return false;
    lag = lo + best;
    if (best > 0 && best < int(v.size()) - 1) {
        const double p = v[size_t(best) - 1], q = v[size_t(best)], r = v[size_t(best) + 1];
        const double den = p - 2.0 * q + r;
        if (den < 0) lag += 0.5 * (p - r) / den;
    }
    return true;
}

} // namespace

HubEngine::FxLatency HubEngine::measureLatencies() {
    const std::lock_guard<std::mutex> lock(measureMutex_);
    FxLatency out;
    BusLayout* b = bus();
    if (b == nullptr || capLen_ == 0) return out;
    const double rate = sampleRate_ / 4.0;
    const double msPerLag = 4000.0 / sampleRate_;
    // the Hub reads the Tracks' signals behind the master bus by the sync-safety blocks
    const int lead = int(std::lround(double(syncSafety_) * lastBlock_ / 4.0));
    const int N = int(rate * 0.25);                 // 250 ms window
    const int K = int(rate * 0.5);                  // up to 500 ms of latency
    const int Kneg = lead + 8;
    const int M = N + K + Kneg;
    const uint32_t w = capWrite_.load(std::memory_order_acquire);
    std::vector<float> x;
    auto copyLast = [&](const float* ring, std::vector<float>& dst) {
        dst.resize(size_t(M));
        for (int i = 0; i < M; ++i) dst[size_t(i)] = ring[(w - uint32_t(M) + uint32_t(i)) % capLen_];
    };

    if (w >= uint32_t(M) && uint32_t(M) + 4096u <= capLen_) {
        std::vector<float> y;
        copyLast(capIn_.data(), y);
        const int yStart = M - Kneg - N;
        double ey = 0;
        for (int i = yStart; i < yStart + N; ++i) ey += double(y[size_t(i)]) * y[size_t(i)];
        const bool sound = ey > double(N) * 1e-6;       // about -60 dBFS

        std::array<double, kMaxSlots> lagMs;          // how late each track reaches the master now, -1 = unknown
        lagMs.fill(-1.0);
        std::vector<double> cum(size_t(M) + 1);
        int measured = 0;
        for (int i = 0; i < kMaxSlots && sound; ++i) {
            if (b->slots[i].state.load(std::memory_order_acquire) != kSlotActive) continue;
            copyLast(capSlots_.data() + size_t(i) * capLen_, x);
            for (int j = 0; j < M; ++j) cum[size_t(j) + 1] = cum[size_t(j)] + double(x[size_t(j)]) * x[size_t(j)];
            if (cum[size_t(M)] < double(M) * 1e-7) continue;   // this track is silent for the viewers right now
            double lag = 0;
            // a single track inside a whole mix correlates less than the mix itself
            if (bestLag(y.data(), x.data(), N, yStart, -Kneg, K, cum, ey, 0.3, lag)) { lagMs[size_t(i)] = std::max(0.0, (lag + lead) * msPerLag); ++measured; }
        }

        // two or more tracks: the latest one has no plug-in latency, the others are that much earlier
        if (measured >= 2) {
            double latest = 0;
            for (double v : lagMs) latest = std::max(latest, v);
            for (int i = 0; i < kMaxSlots; ++i)
                if (lagMs[size_t(i)] >= 0.0) b->slots[i].chainLatencyBits.store(floatBits(float(latest - lagMs[size_t(i)])), std::memory_order_relaxed);
        }
        // every active track's remembered chain latency (live: a lone vocal keeps its measured value)
        double slowest = 0;
        for (int i = 0; i < kMaxSlots; ++i)
            if (b->slots[i].state.load(std::memory_order_acquire) == kSlotActive)
                slowest = std::max(slowest, double(bitsFloat(b->slots[i].chainLatencyBits.load(std::memory_order_relaxed))));
        out.tracksMs = slowest;
        // master plug-ins = how late a track arrives minus the line-up delay the DAW gave it
        for (int i = 0; i < kMaxSlots; ++i) {
            if (lagMs[size_t(i)] < 0.0) continue;
            const double lineUp = slowest - double(bitsFloat(b->slots[i].chainLatencyBits.load(std::memory_order_relaxed)));
            const double m = std::max(0.0, lagMs[size_t(i)] - lineUp);
            out.masterMs = out.masterMs < 0.0 ? m : std::min(out.masterMs, m);
        }
    }

    // ---- S7: Match active feeders with end-of-track slots & measure Li ---------
    const uint64_t now = nowNs();
    for (int j = 0; j < kMaxFriends; ++j) {
        FriendHeader& fh = b->friends[j];
        const uint32_t fid = fh.id.load(std::memory_order_acquire);
        if (fid == 0) continue;

        const int fidx = fh.feeder.load(std::memory_order_acquire);
        if (fidx < 0 || fidx >= kMaxFeeders) {
            const int outS = fh.outSlot.load(std::memory_order_relaxed);
            if (outS >= 0 && outS < kMaxSlots) {
                if (b->slots[outS].fedBy.load(std::memory_order_relaxed) == fid) {
                    b->slots[outS].fedBy.store(0, std::memory_order_release);
                    b->slots[outS].fxLatencyBits.store(0, std::memory_order_relaxed);
                    b->slots[outS].flags.fetch_and(~kFlagViewersViaHub, std::memory_order_release);
                }
                fh.outSlot.store(-1, std::memory_order_release);
            }
            continue;
        }

        FeederRecord& feeder = b->feeders[fidx];
        if (feeder.state.load(std::memory_order_acquire) != kSlotActive
            || feeder.friendId.load(std::memory_order_relaxed) != fid
            || now - feeder.heartbeatNs.load(std::memory_order_relaxed) > 800'000'000ull) {
            const int outS = fh.outSlot.load(std::memory_order_relaxed);
            if (outS >= 0 && outS < kMaxSlots) {
                if (b->slots[outS].fedBy.load(std::memory_order_relaxed) == fid) {
                    b->slots[outS].fedBy.store(0, std::memory_order_release);
                    b->slots[outS].fxLatencyBits.store(0, std::memory_order_relaxed);
                    b->slots[outS].flags.fetch_and(~kFlagViewersViaHub, std::memory_order_release);
                }
                fh.outSlot.store(-1, std::memory_order_release);
            }
            continue;
        }

        int matchedSlot = -1;
        std::string feederName;
        uint32_t feederColor = 0;
        readFeederIdentity(feeder, feederName, feederColor);

        // 1. Fast match by track name
        if (!feederName.empty()) {
            for (int s = 0; s < kMaxSlots; ++s) {
                if (b->slots[s].state.load(std::memory_order_acquire) != kSlotActive) continue;
                if (b->slots[s].flags.load(std::memory_order_relaxed) & kFlagApp) continue;
                std::string sName, sUuid;
                uint32_t sColor = 0;
                if (readSlotIdentity(b->slots[s], sName, sUuid, sColor) && sName == feederName) {
                    matchedSlot = s;
                    break;
                }
            }
        }

        // 2. Correlation match if not matched by name
        const uint64_t tapW = feeder.tapWrite.load(std::memory_order_acquire);
        const int tapN = N;
        const int Ktap = std::min(K, int(rate * 0.2));
        const int fStart = M - Ktap - tapN;
        bool hasTapSound = false;
        std::vector<float> fTap;
        std::vector<double> cumTap;

        if (tapW >= uint64_t(M)) {
            const auto& tap = b->feederTap[fidx];
            fTap.resize(size_t(M));
            for (int k = 0; k < M; ++k)
                fTap[size_t(k)] = tap.mono[(tapW - uint64_t(M) + uint64_t(k)) % kTapFrames];
            cumTap.assign(size_t(M) + 1, 0.0);
            for (int k = 0; k < M; ++k)
                cumTap[size_t(k) + 1] = cumTap[size_t(k)] + double(fTap[size_t(k)]) * fTap[size_t(k)];

            double eTap = cumTap[size_t(fStart + tapN)] - cumTap[size_t(fStart)];
            hasTapSound = eTap > double(tapN) * 1e-5;

            if (matchedSlot < 0 && hasTapSound && fStart > 0) {
                int tried = 0;
                for (int s = 0; s < kMaxSlots && tried < 8; ++s) {
                    if (b->slots[s].state.load(std::memory_order_acquire) != kSlotActive) continue;
                    if (b->slots[s].flags.load(std::memory_order_relaxed) & kFlagApp) continue;
                    ++tried;

                    copyLast(capSlots_.data() + size_t(s) * capLen_, x);
                    double eySlot = 0.0;
                    for (int k = fStart; k < fStart + tapN; ++k) eySlot += double(x[size_t(k)]) * x[size_t(k)];
                    if (eySlot < double(tapN) * 1e-6) continue;

                    double lag = 0, score = 0;
                    if (bestLag(x.data(), fTap.data(), tapN, fStart, 0, Ktap, cumTap, eySlot, 0.6, lag, false, &score)) {
                        matchedSlot = s;
                        break;
                    }
                }
            }
        }

        // 3. Measure Li and publish pairing
        if (matchedSlot >= 0) {
            SlotHeader& slot = b->slots[matchedSlot];
            double li = 0.0;
            if (hasTapSound && fStart > 0 && !fTap.empty()) {
                copyLast(capSlots_.data() + size_t(matchedSlot) * capLen_, x);
                double eySlot = 0.0;
                for (int k = fStart; k < fStart + tapN; ++k) eySlot += double(x[size_t(k)]) * x[size_t(k)];
                double lag = 0, score = 0;
                if (bestLag(x.data(), fTap.data(), tapN, fStart, 0, Ktap, cumTap, eySlot, 0.5, lag, false, &score)) {
                    li = std::max(0.0, lag * msPerLag);
                } else {
                    li = double(bitsFloat(slot.chainLatencyBits.load(std::memory_order_relaxed)));
                }
            } else {
                li = double(bitsFloat(slot.chainLatencyBits.load(std::memory_order_relaxed)));
            }

            const int oldS = fh.outSlot.load(std::memory_order_relaxed);
            if (oldS >= 0 && oldS < kMaxSlots && oldS != matchedSlot) {
                if (b->slots[oldS].fedBy.load(std::memory_order_relaxed) == fid) {
                    b->slots[oldS].fedBy.store(0, std::memory_order_release);
                    b->slots[oldS].fxLatencyBits.store(0, std::memory_order_relaxed);
                    b->slots[oldS].flags.fetch_and(~kFlagViewersViaHub, std::memory_order_release);
                }
            }

            fh.outSlot.store(matchedSlot, std::memory_order_release);
            slot.fedBy.store(fid, std::memory_order_release);
            slot.fxLatencyBits.store(floatBits(float(li)), std::memory_order_relaxed);
            if (lineUpOn_.load(std::memory_order_relaxed))
                slot.flags.fetch_or(kFlagViewersViaHub, std::memory_order_release);
            else
                slot.flags.fetch_and(~kFlagViewersViaHub, std::memory_order_release);
        } else {
            const int oldS = fh.outSlot.load(std::memory_order_relaxed);
            if (oldS >= 0 && oldS < kMaxSlots) {
                if (b->slots[oldS].fedBy.load(std::memory_order_relaxed) == fid) {
                    b->slots[oldS].fedBy.store(0, std::memory_order_release);
                    b->slots[oldS].fxLatencyBits.store(0, std::memory_order_relaxed);
                    b->slots[oldS].flags.fetch_and(~kFlagViewersViaHub, std::memory_order_release);
                }
                fh.outSlot.store(-1, std::memory_order_release);
            }
        }
    }

    return out;
}

std::optional<double> HubEngine::measureLag(int slot, double minScore, bool anyPolarity, bool source, double* score, bool faint) {
    const std::lock_guard<std::mutex> lock(measureMutex_);
    if (score) *score = 0.0;
    if (bus() == nullptr || capLen_ == 0 || slot < 0 || slot >= (source ? kMaxSources : kMaxSlots)) return std::nullopt;
    const float* hist = (source ? capSources_.data() : capSlots_.data()) + size_t(slot) * capLen_;
    const double rate = sampleRate_ / 4.0;
    const int lead = int(std::lround(double(syncSafety_) * lastBlock_ / 4.0));
    const int N = int(rate * (faint ? 1.0 : 0.25)), K = int(rate * 0.5);
    const int Kneg = lead + 8 + (faint ? int(rate * 0.4) : 0), M = N + K + Kneg;
    const uint32_t w = capWrite_.load(std::memory_order_acquire);
    if (w < uint32_t(M) || uint32_t(M) + 4096u > capLen_) return std::nullopt;
    std::vector<float> x(static_cast<size_t>(M)), y(static_cast<size_t>(M));
    for (int i = 0; i < M; ++i) {
        const uint32_t at = (w - uint32_t(M) + uint32_t(i)) % capLen_;
        y[size_t(i)] = capIn_[at];
        x[size_t(i)] = hist[at];
    }
    if (faint)   // first difference: music is mostly bass, a mic held to headphones hears mostly the rest
        for (int i = M - 1; i > 0; --i) { y[size_t(i)] -= y[size_t(i) - 1]; x[size_t(i)] -= x[size_t(i) - 1]; }
    const int yStart = M - Kneg - N;
    double ey = 0;
    for (int i = yStart; i < yStart + N; ++i) ey += double(y[size_t(i)]) * y[size_t(i)];
    std::vector<double> cum(size_t(M) + 1);
    for (int j = 0; j < M; ++j) cum[size_t(j) + 1] = cum[size_t(j)] + double(x[size_t(j)]) * x[size_t(j)];
    const double floor = faint ? 1e-9 : 1e-7;   // the difference of a quiet bleed is quieter still
    if (ey < double(N) * floor || cum[size_t(M)] < double(M) * floor) return std::nullopt;
    double lag = 0;
    if (!bestLag(y.data(), x.data(), N, yStart, -Kneg, K, cum, ey, minScore, lag, anyPolarity, score, faint ? 1 : 4)) return std::nullopt;
    const double ms = (lag + lead) * 4000.0 / sampleRate_;
    return faint ? ms : std::max(0.0, ms);
}

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
        friendsReset();
    }

    const uint32_t sr = uint32_t(sampleRate_ + 0.5);
    h.hubHeartbeatNs.store(nowNs(), std::memory_order_relaxed);
    h.hubSampleRate.store(sr, std::memory_order_relaxed);
    h.hubBlockSize.store(uint32_t(n), std::memory_order_relaxed);
    syncSafety_ = std::clamp(p.syncSafety, 0, 2);
    h.hubLatencyFrames.store(uint32_t(limiters_[0].latency()), std::memory_order_relaxed);   // the stream itself is live
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

    for (auto& c : phones_) std::memset(c.data(), 0, size_t(n) * sizeof(float));
    int live[kMaxSlots];   // slots that feed the latency history this chunk
    int numLive = 0;
    lastBlock_ = n;
    std::array<bool, kNumStreamOuts> used{};   // a stem is zeroed the first time a slot writes to it
    used[0] = true;
    // The Stream Mix is the master bus itself: every Track sends what viewers hear to the DAW,
    // and the plug-ins above the Hub have already processed it.
    for (int c = 0; c < 2; ++c) std::memcpy(outs_[0][size_t(c)].data(), io[std::min(c, numCh - 1)], size_t(n) * sizeof(float));

    // ---- solo (stems follow the viewers' solo like the Tracks do) ----------------------------
    bool soloActive = false;
    for (int i = 0; i < kMaxSlots; ++i) {
        const SlotHeader& sh = b->slots[i];
        if (sh.state.load(std::memory_order_acquire) != kSlotActive) continue;
        const uint32_t f = sh.flags.load(std::memory_order_relaxed);
        if ((f & kFlagSolo) && (f & kFlagStr) && sh.sampleRate.load(std::memory_order_relaxed) == sr) { soloActive = true; break; }
    }
    soloActive = soloActive || friendsSoloActive();   // a friend soloed for the viewers counts too

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
            if (s.known) { s.known = false; s.needAnchor = true; s.xfActive = false; s.gain.snap(0.0f); s.mon.snap(0.0f); }
            continue;
        }
        const uint32_t epoch = sh.epoch.load(std::memory_order_acquire);
        const uint64_t claimNs = sh.claimNs.load(std::memory_order_relaxed);
        bool fresh = false;
        if (!s.known || epoch != s.epoch || claimNs != s.claimNs) {
            s.known = true; s.epoch = epoch; s.claimNs = claimNs;
            s.needAnchor = true; s.xfActive = false; s.mismatch = 0; s.aheadBlocks = 0;
            s.gain.snap(0.0f);   // fade in from silence
            s.mon.snap(0.0f);
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
            if (!fresh && !s.needAnchor && s.mon.current() > 0.0f) startCrossfade(s, s.cursor);
            s.cursor = c;
            s.timelineLocked = viaTl;
            s.needAnchor = false;
            s.mismatch = 0;
        } else if (useTimeline && (blockCounter_ & 3u) == 0) {
            const TagHit hit = findTimelinePosition(sh, b->slotTags[i], mixTime, 256);
            s.timelineLocked = hit.found;
            if (hit.found && hit.fifoPos != s.cursor) {
                if (++s.mismatch >= 2) {
                    startCrossfade(s, s.cursor);
                    s.cursor = hit.fifoPos;
                    s.mismatch = 0;
                }
            } else {
                s.mismatch = 0;
            }
        } else if (!useTimeline) {
            s.timelineLocked = false;
        }

        const float delayMs = bitsFloat(sh.strDelayBits.load(std::memory_order_relaxed));
        s.delay = std::min<uint32_t>(maxDelay, uint32_t(std::max(0.0f, delayMs) * float(sampleRate_) * 0.001f + 0.5f));
        if (f & kFlagViewersViaHub) {
            const uint32_t fid = sh.fedBy.load(std::memory_order_relaxed);
            int friendSlot = -1;
            for (int j = 0; j < kMaxFriends; ++j) {
                if (b->friends[j].id.load(std::memory_order_relaxed) == fid) { friendSlot = j; break; }
            }
            if (friendSlot >= 0) {
                const float liMs = bitsFloat(sh.fxLatencyBits.load(std::memory_order_relaxed));
                const uint32_t liFrames = uint32_t(std::max(0.0f, liMs) * float(sampleRate_) * 0.001f + 0.5f);
                const uint32_t friendTot = usedTot_[size_t(friendSlot)] + liFrames;
                const uint32_t lineDelay = lineD_ > friendTot ? lineD_ - friendTot : 0u;
                s.delay = std::min<uint32_t>(maxDelay, lineDelay);
            }
        }

        const ReadResult res = ringReadAt(b->slotAudio[i], sh.writePos, s.cursor, A, uint32_t(n));
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

        // ---- headphones: You Hear + Headphone Level (a bypassed Track passes its audio) -------
        s.mon.setTarget((f & kFlagBypassed) ? 1.0f
                        : (f & kFlagMon) ? ssdsp::dbToGain(bitsFloat(sh.monTrimBits.load(std::memory_order_relaxed))) : 0.0f);
        if (s.mon.isSmoothing() || s.mon.current() > 0.0f) {
            for (int j = 0; j < n; ++j) {
                const float gm = s.mon.next();
                phones_[0][size_t(j)] += A[0][j] * gm;
                phones_[1][size_t(j)] += A[1][j] * gm;
            }
        }

        // ---- stems + per-track viewers meter: gain / pan / solo / panic ----------------------
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
        if (outIdx > 0 && !used[size_t(outIdx)]) {
            used[size_t(outIdx)] = true;
            for (auto& c : outs_[outIdx]) std::memset(c.data(), 0, size_t(n) * sizeof(float));
        }
        // ponytail: stems jump (no crossfade) when the viewers delay changes
        float** V = A;
        if (s.delay > 0) {
            ringReadAt(b->slotAudio[i], sh.writePos, s.cursor - uint64_t(n) >= s.delay ? s.cursor - uint64_t(n) - s.delay : 0, B, uint32_t(n));
            V = B;
        }

        float pk0 = 0.0f, pk1 = 0.0f;
        const bool silent = !s.gain.isSmoothing() && s.gain.current() == 0.0f;
        float* mono = slotBlock_.data() + size_t(i) * size_t(maxBlock_);   // for measureLatencies()
        live[numLive++] = i;
        if (silent) std::memset(mono, 0, size_t(n) * sizeof(float));
        if (!silent) {
            float* s0 = outIdx > 0 ? outs_[outIdx][0].data() : nullptr;
            float* s1 = outIdx > 0 ? outs_[outIdx][1].data() : nullptr;
            for (int j = 0; j < n; ++j) {
                const float gg = s.gain.next();
                const float l = V[0][j] * gg * s.panL.next();
                const float r = V[1][j] * gg * s.panR.next();
                if (s0) { s0[j] += l; s1[j] += r; }
                if ((f & kFlagViewersViaHub) && !p.panic) {
                    outs_[0][0][j] += l;
                    outs_[0][1][j] += r;
                }
                mono[j] = l + r;
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

    // ---- history for measuring the master plug-ins' latency ----------------------------------
    if (capLen_ > 0) {
        // Each history sample sums 4 input samples. The work is per live slot rather than
        // every slot x every sample (64 x 2048 branchy iterations per block before).
        const uint32_t w0 = capWrite_.load(std::memory_order_relaxed);
        const int phase = decCount_;
        uint32_t w = w0;
        {
            const float* l = outs_[0][0].data();
            const float* r = outs_[0][1].data();
            float acc = decIn_;
            int k = phase;
            for (int j = 0; j < n; ++j) {
                acc += l[j] + r[j];
                if (++k == 4) { capIn_[w % capLen_] = acc; ++w; acc = 0.0f; k = 0; }
            }
            decIn_ = acc;
            decCount_ = k;
        }
        const std::array<bool, kMaxSlots> wasLive = slotLive_;
        slotLive_.fill(false);
        for (int q = 0; q < numLive; ++q) {
            const int i = live[q];
            slotLive_[size_t(i)] = true;
            quiet_[size_t(i)] = 0;
            const float* x = slotBlock_.data() + size_t(i) * size_t(maxBlock_);
            float* ring = capSlots_.data() + size_t(i) * capLen_;
            float acc = wasLive[size_t(i)] ? decSlot_[size_t(i)] : 0.0f;
            uint32_t at = w0;
            int k = phase;
            for (int j = 0; j < n; ++j) {
                acc += x[j];
                if (++k == 4) { ring[at % capLen_] = acc; ++at; acc = 0.0f; k = 0; }
            }
            decSlot_[size_t(i)] = acc;
        }
        // App Audio signals: read each one's newest block (it runs in the same DAW cycle as the Hub)
        for (int i = 0; i < kMaxSources; ++i) {
            SourceHeader& src = b->sources[i];
            float* ring = capSources_.data() + size_t(i) * capLen_;
            if (src.state.load(std::memory_order_acquire) != kSlotActive) {
                srcKnown_[size_t(i)] = false;
                if (srcQuiet_[size_t(i)] < capLen_) {
                    for (uint32_t at = w0; at != w; ++at) ring[at % capLen_] = 0.0f;
                    srcQuiet_[size_t(i)] += w - w0;
                }
                continue;
            }
            const uint64_t sw = src.writePos.load(std::memory_order_acquire);
            uint64_t& cur = srcCursor_[size_t(i)];
            const int64_t lead = int64_t(sw - cur);
            if (!srcKnown_[size_t(i)] || lead < n || lead > std::max<int64_t>(8 * n, int64_t(sampleRate_ * 0.2))) {
                cur = sw >= uint64_t(n) ? sw - uint64_t(n) : 0;
                srcKnown_[size_t(i)] = true;
            }
            float* T[2] = { tmpB_[0].data(), tmpB_[1].data() };
            ringReadAt(b->sourceAudio[i], src.writePos, cur, T, uint32_t(n));
            cur += uint64_t(n);
            srcQuiet_[size_t(i)] = 0;
            float acc = decSrc_[size_t(i)];
            uint32_t at = w0;
            int k = phase;
            for (int j = 0; j < n; ++j) {
                acc += T[0][j] + T[1][j];
                if (++k == 4) { ring[at % capLen_] = acc; ++at; acc = 0.0f; k = 0; }
            }
            decSrc_[size_t(i)] = acc;
        }
        // a slot that went quiet gets zero history until its whole ring is clear, then costs nothing
        for (int i = 0; i < kMaxSlots; ++i) {
            if (slotLive_[size_t(i)] || quiet_[size_t(i)] >= capLen_) continue;
            float* ring = capSlots_.data() + size_t(i) * capLen_;
            for (uint32_t at = w0; at != w; ++at) ring[at % capLen_] = 0.0f;
            quiet_[size_t(i)] += w - w0;
            decSlot_[size_t(i)] = 0.0f;
        }
        capWrite_.store(w, std::memory_order_release);
    }

    // ---- friends: headphones + Stream Mix (after the Line-up delay, before the limiter) ------------
    friendsMix(n, p, used, soloActive);

    // ---- master gain, limiter, meters, write ------------------------------------------------
    master_.setTarget(p.panic || p.silence ? 0.0f : ssdsp::dbToGain(p.masterDb));
    float* mg = tmpB_[0].data();
    for (int j = 0; j < n; ++j) mg[j] = master_.next();

    const float ceiling = ssdsp::dbToGain(std::clamp(p.ceilingDb, -24.0f, 0.0f));
    auto& sh = b->streamHeader;
    const float decay = std::exp(-float(n) / (0.3f * float(sr)));
    for (int o = 0; o < kNumStreamOuts; ++o) {
        float* l = outs_[o][0].data();
        float* r = outs_[o][1].data();
        if (used[size_t(o)]) {
            if (!outUsed_[size_t(o)]) limiters_[size_t(o)].reset();
            for (int j = 0; j < n; ++j) { l[j] *= mg[j]; r[j] *= mg[j]; }
            limiters_[size_t(o)].setCeiling(ceiling);
            limiters_[size_t(o)].process(l, r, n, p.limiterOn);
        }
        outUsed_[size_t(o)] = used[size_t(o)];
        const float* src[2] = { l, r };
        ringWrite(b->streamAudio[o], sh.writePos[o], used[size_t(o)] ? src : nullptr, 2, uint32_t(n));

        outPeak_[o][0] = std::max(used[size_t(o)] ? ssdsp::blockPeak(l, n) : 0.0f, outPeak_[o][0] * decay);
        outPeak_[o][1] = std::max(used[size_t(o)] ? ssdsp::blockPeak(r, n) : 0.0f, outPeak_[o][1] * decay);
        sh.peakBits[o][0].store(floatBits(outPeak_[o][0]), std::memory_order_relaxed);
        sh.peakBits[o][1].store(floatBits(outPeak_[o][1]), std::memory_order_relaxed);
    }
    loudness_.process(outs_[0][0].data(), outs_[0][1].data(), n);
    sh.loudnessMBits.store(floatBits(loudness_.momentary()), std::memory_order_relaxed);
    sh.loudnessSBits.store(floatBits(loudness_.shortTerm()), std::memory_order_relaxed);

    // ---- output: the headphone mix, or the Stream Mix while previewing (20 ms crossfade) -----
    for (int c = 0; c < numCh; ++c) std::memcpy(io[c], phones_[size_t(std::min(c, 1))].data(), size_t(n) * sizeof(float));
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
