// The ssengine API over the Rust core (HEARASIDE_USE_RUST_CORE, PLAN-rust-migration R2.5).
// Thin forwarding to rust/crates/hs-ffi; the Rust side owns all state and its threading rules.
// SPDX-License-Identifier: MIT
#include "ssengine/engine.h"
#include "ssengine/friend_reader.h"

#include "hs_core.h"

#include <cstring>
#include <limits>

namespace ssengine {

static_assert(sizeof(HubEngine::FriendControl) == 12, "FriendControl must match the Rust repr(C) struct");
static_assert(sizeof(HubEngine::FriendInfo) == 40, "FriendInfo must match the Rust repr(C) struct");

std::vector<float> probeSweep(double sampleRate) {
    std::vector<float> out(hs_probe_sweep(sampleRate, nullptr, 0));
    hs_probe_sweep(sampleRate, out.data(), out.size());
    return out;
}

// ---- TrackPublisher ------------------------------------------------------------------------------

TrackPublisher::TrackPublisher() : h_(hs_track_new()) {}
TrackPublisher::~TrackPublisher() { hs_track_free(h_); }

TrackPublisher::Status TrackPublisher::connect(const std::string& busName, std::string uuid, uint32_t sampleRate, uint32_t numChannels) {
    busName_ = busName.empty() ? std::string("Main") : busName;
    const auto s = Status(hs_track_connect(h_, busName_.c_str(), uuid.c_str(), sampleRate, numChannels));
    uuid_.assign(hs_track_uuid(h_, nullptr, 0), '\0');
    if (!uuid_.empty()) {
        std::vector<char> buf(uuid_.size() + 1);
        hs_track_uuid(h_, buf.data(), buf.size());
        uuid_.assign(buf.data());
    }
    return s;
}

void TrackPublisher::disconnect() { hs_track_disconnect(h_); }
void TrackPublisher::prepare(uint32_t sampleRate, uint32_t numChannels) { hs_track_prepare(h_, sampleRate, numChannels); }
void TrackPublisher::setIdentity(const std::string& name, uint32_t colorARGB) { hs_track_set_identity(h_, name.c_str(), colorARGB); }
TrackPublisher::Status TrackPublisher::status() const noexcept { return Status(hs_track_status(h_)); }
int TrackPublisher::slotIndex() const noexcept { return hs_track_slot_index(h_); }
ssbus::BusLayout* TrackPublisher::bus() const noexcept { return static_cast<ssbus::BusLayout*>(hs_track_bus(h_)); }
ssbus::SlotHeader* TrackPublisher::slot() const noexcept { return static_cast<ssbus::SlotHeader*>(hs_track_slot(h_)); }
bool TrackPublisher::hubPresent() const noexcept { return hs_track_hub_present(h_); }
bool TrackPublisher::soloActive() const noexcept { return hs_track_solo_active(h_); }

void TrackPublisher::mirror(const Mirror& m) noexcept {
    const HsMirror hm{ m.mon, m.str, m.solo, m.gainDb, m.pan, m.delayMs, m.trimDb, m.stem, m.app };
    hs_track_mirror(h_, &hm);
}

void TrackPublisher::process(const float* const* ch, int numCh, int n, int64_t timeSamples, bool playing,
                             bool offline, bool bypassed) noexcept {
    hs_track_process(h_, ch, numCh, n, timeSamples, playing, offline, bypassed);
}

// ---- HubEngine -------------------------------------------------------------------------------------

HubEngine::HubEngine() : h_(hs_hub_new()) {}
HubEngine::~HubEngine() { hs_hub_free(h_); }

bool HubEngine::connect(const std::string& busName) {
    busName_ = busName.empty() ? std::string("Main") : busName;
    return hs_hub_connect(h_, busName_.c_str());
}
void HubEngine::disconnect() { hs_hub_disconnect(h_); }
void HubEngine::maintain() { hs_hub_maintain(h_); }
void HubEngine::prepare(double sampleRate, int maxBlock) { hs_hub_prepare(h_, sampleRate, maxBlock); }
void HubEngine::setOutputName(int output, const std::string& name) { hs_hub_set_output_name(h_, output, name.c_str()); }
HubEngine::Role HubEngine::role() const noexcept { return Role(hs_hub_role(h_)); }
ssbus::BusLayout* HubEngine::bus() const noexcept { return static_cast<ssbus::BusLayout*>(hs_hub_bus(h_)); }
int HubEngine::latencyFrames() const noexcept { return hs_hub_latency_frames(h_); }
uint64_t HubEngine::historyFrames() const noexcept { return hs_hub_history_frames(h_); }
int HubEngine::lastBlockSize() const noexcept { return hs_hub_last_block_size(h_); }
uint64_t HubEngine::activeSlotsMask() const noexcept { return hs_hub_active_slots_mask(h_); }

HubEngine::FxLatency HubEngine::measureLatencies() {
    HsFxLatency r{ -1.0, -1.0 };
    hs_hub_measure_latencies(h_, &r);
    return { r.master_ms, r.tracks_ms };
}

std::optional<double> HubEngine::measureLag(int index, double minScore, bool anyPolarity, bool source, double* score,
                                            bool faint, std::optional<double> removeMs) {
    double lag = 0.0, s = 0.0;
    const bool found = hs_hub_measure_lag(h_, index, minScore, anyPolarity, source, faint, removeMs ? *removeMs : std::numeric_limits<double>::quiet_NaN(), &lag, &s);
    if (score) *score = s;
    return found ? std::optional<double>(lag) : std::nullopt;
}

HubEngine::SourceReadStats HubEngine::takeSourceReadStats(int index) noexcept {
    HsSourceReadStats s{};
    hs_hub_take_source_read_stats(h_, index, &s);
    return { s.lead_min, s.lead_max, s.jumps, s.blocks };
}

HubEngine::FriendControl& HubEngine::friendControl(int i) noexcept {
    return *static_cast<FriendControl*>(const_cast<void*>(hs_hub_friend_control(h_, i)));
}
const HubEngine::FriendInfo& HubEngine::friendInfo(int i) const noexcept {
    return *static_cast<const FriendInfo*>(hs_hub_friend_info(h_, i));
}
void HubEngine::setLineUp(bool on, float limitMs) noexcept { hs_hub_set_line_up(h_, on, limitMs); }
float HubEngine::lineUpMs() const noexcept { return hs_hub_line_up_ms(h_); }
int HubEngine::lineUpSlowest() const noexcept { return hs_hub_line_up_slowest(h_); }
uint32_t HubEngine::lineUpChanges() const noexcept { return hs_hub_line_up_changes(h_); }

void HubEngine::setFriendTap(FriendTap tap, void* user) noexcept {
    tap_ = tap;
    tapUser_ = user;
    hs_hub_set_friend_tap(h_, reinterpret_cast<HsFriendTap>(tap), user);
}

bool HubEngine::pairWith(int feeder, uint32_t arg) { return hs_hub_pair_with(h_, feeder, arg); }
bool HubEngine::startProbe(int feeder) { return hs_hub_start_probe(h_, feeder); }
bool HubEngine::probing() const noexcept { return hs_hub_probing(h_); }
void HubEngine::probeStep() { hs_hub_probe_step(h_); }

std::vector<HubEngine::PairMemory> HubEngine::pairMemory() const {
    std::vector<char> buf(hs_hub_pair_memory(h_, nullptr, 0) + 1);
    hs_hub_pair_memory(h_, buf.data(), buf.size());
    std::vector<PairMemory> out;
    const std::string text(buf.data());
    size_t at = 0;
    while (at < text.size()) {
        const size_t nl = text.find('\n', at), tab = text.find('\t', at);
        if (nl == std::string::npos || tab == std::string::npos || tab > nl) break;
        out.push_back({ text.substr(at, tab - at), text.substr(tab + 1, nl - tab - 1) });
        at = nl + 1;
    }
    return out;
}

void HubEngine::setPairMemory(std::vector<PairMemory> memory) {
    std::string text;
    for (const auto& m : memory) text += m.feeder + "\t" + m.slotUuid + "\n";
    hs_hub_set_pair_memory(h_, text.c_str());
}

void HubEngine::process(float* const* io, int numCh, int n, const HubParams& p,
                        int64_t timeSamples, bool playing, bool offline) noexcept {
    const HsHubParams hp{ p.masterDb, p.limiterOn, p.ceilingDb, p.preview, p.panic, p.syncSafety, p.bypassed, p.silence };
    hs_hub_process(h_, io, numCh, n, &hp, timeSamples, playing, offline);
}

// ---- FriendReader ----------------------------------------------------------------------------------

FriendReader::FriendReader() : r_(hs_reader_new()) {}
FriendReader::~FriendReader() { hs_reader_free(r_); }
void FriendReader::prepare(double sampleRate, int maxBlock) { hs_reader_prepare(r_, sampleRate, maxBlock); }
void FriendReader::reset() noexcept { hs_reader_reset(r_); }
bool FriendReader::pull(const ssbus::ChannelRing& ring, const std::atomic<uint64_t>& writePos, uint32_t rate, float* const* out, int n) noexcept {
    return hs_reader_pull(r_, &ring, &writePos, rate, out, n);
}
double FriendReader::bufferMs() const noexcept { return hs_reader_buffer_ms(r_); }
bool FriendReader::primed() const noexcept { return hs_reader_primed(r_); }
uint32_t FriendReader::underruns() const noexcept { return hs_reader_underruns(r_); }
uint32_t FriendReader::reprimes() const noexcept { return hs_reader_reprimes(r_); }
double FriendReader::concealedMs() const noexcept { return hs_reader_concealed_ms(r_); }
double FriendReader::jitterP95Ms() const noexcept { return hs_reader_jitter_p95_ms(r_); }

} // namespace ssengine
