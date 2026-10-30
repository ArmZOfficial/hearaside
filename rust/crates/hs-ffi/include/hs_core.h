/* C ABI of the Rust core (rust/crates/hs-ffi), for the hybrid phase of the migration (PLAN R2.5).
 * Hand-written to match src/lib.rs; the C++ adapter (libs/ssengine/src/rust_core.cpp) is the only user.
 * Threading: one message thread for changes, one audio thread for *_process / *_pull, any thread for queries.
 * SPDX-License-Identifier: MIT */
#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct HsTrack HsTrack;
typedef struct HsHub HsHub;
typedef struct HsConsumer HsConsumer;   /* StreamConsumer */
typedef struct HsReader HsReader;       /* FriendReader */

typedef struct HsMirror {
    bool mon, str_, solo;
    float gain_db, pan, delay_ms, trim_db;
    int32_t stem;
    bool app;
} HsMirror;

typedef struct HsHubParams {
    float master_db;
    bool limiter_on;
    float ceiling_db;
    bool preview, panic;
    int32_t sync_safety;
    bool bypassed, silence;
} HsHubParams;

typedef struct HsFxLatency { double master_ms, tracks_ms; } HsFxLatency;
typedef struct HsSourceReadStats { int64_t lead_min, lead_max; uint32_t jumps, blocks; } HsSourceReadStats;

/* ---- TrackPublisher ---- */
HsTrack* hs_track_new(void);
void     hs_track_free(HsTrack*);
int32_t  hs_track_connect(const HsTrack*, const char* bus, const char* uuid, uint32_t sampleRate, uint32_t numChannels);
void     hs_track_disconnect(const HsTrack*);
void     hs_track_prepare(const HsTrack*, uint32_t sampleRate, uint32_t numChannels);
void     hs_track_set_identity(const HsTrack*, const char* name, uint32_t colorARGB);
int32_t  hs_track_status(const HsTrack*);   /* 0 disconnected, 1 connected, 2 slots full, 3 shm error */
size_t   hs_track_uuid(const HsTrack*, char* buf, size_t cap);
int32_t  hs_track_slot_index(const HsTrack*);
void*    hs_track_bus(const HsTrack*);    /* ssbus::BusLayout* */
void*    hs_track_slot(const HsTrack*);   /* ssbus::SlotHeader* */
bool     hs_track_hub_present(const HsTrack*);
bool     hs_track_solo_active(const HsTrack*);
void     hs_track_mirror(const HsTrack*, const HsMirror*);
void     hs_track_process(const HsTrack*, const float* const* ch, int32_t numCh, int32_t n, int64_t timeSamples,
                          bool playing, bool offline, bool bypassed);

/* ---- HubEngine ---- */
typedef void (*HsFriendTap)(void* user, int32_t friendIndex, const float* const* ch, int32_t n, int64_t timeSamples, bool playing);

HsHub*   hs_hub_new(void);
void     hs_hub_free(HsHub*);
bool     hs_hub_connect(const HsHub*, const char* bus);
void     hs_hub_disconnect(const HsHub*);
void     hs_hub_maintain(const HsHub*);
void     hs_hub_prepare(const HsHub*, double sampleRate, int32_t maxBlock);
void     hs_hub_set_output_name(const HsHub*, int32_t output, const char* name);
int32_t  hs_hub_role(const HsHub*);       /* 0 disconnected, 1 owner, 2 secondary */
void*    hs_hub_bus(const HsHub*);        /* ssbus::BusLayout* */
int32_t  hs_hub_latency_frames(const HsHub*);
int32_t  hs_hub_last_block_size(const HsHub*);
uint64_t hs_hub_history_frames(const HsHub*);
uint64_t hs_hub_active_slots_mask(const HsHub*);
void     hs_hub_measure_latencies(const HsHub*, HsFxLatency* out);
bool     hs_hub_measure_lag(const HsHub*, int32_t index, double minScore, bool anyPolarity, bool source, bool faint,
                            double removeMs /* NaN = none */, double* lagMs, double* score);
void     hs_hub_take_source_read_stats(const HsHub*, int32_t index, HsSourceReadStats* out);
const void* hs_hub_friend_control(const HsHub*, int32_t j);   /* HubEngine::FriendControl* */
const void* hs_hub_friend_info(const HsHub*, int32_t j);      /* HubEngine::FriendInfo* */
void     hs_hub_set_line_up(const HsHub*, bool on, float limitMs);
float    hs_hub_line_up_ms(const HsHub*);
int32_t  hs_hub_line_up_slowest(const HsHub*);
uint32_t hs_hub_line_up_changes(const HsHub*);
bool     hs_hub_pair_with(const HsHub*, int32_t feeder, uint32_t arg);
bool     hs_hub_start_probe(const HsHub*, int32_t feeder);
bool     hs_hub_probing(const HsHub*);
void     hs_hub_probe_step(const HsHub*);
size_t   hs_hub_pair_memory(const HsHub*, char* buf, size_t cap);   /* "feeder\tuuid\n" lines */
void     hs_hub_set_pair_memory(const HsHub*, const char* text);
void     hs_hub_set_friend_tap(const HsHub*, HsFriendTap tap, void* user);
void     hs_hub_process(const HsHub*, float* const* io, int32_t numCh, int32_t n, const HsHubParams* p,
                        int64_t timeSamples, bool playing, bool offline);

size_t   hs_probe_sweep(double sampleRate, float* out, size_t cap);

/* ---- FriendReader ---- */
HsReader* hs_reader_new(void);
void      hs_reader_free(HsReader*);
void      hs_reader_prepare(HsReader*, double sampleRate, int32_t maxBlock);
void      hs_reader_reset(HsReader*);
bool      hs_reader_pull(HsReader*, const void* ring /* ssbus::ChannelRing* */, const void* writePos /* std::atomic<uint64_t>* */,
                         uint32_t rate, float* const* out, int32_t n);
double    hs_reader_buffer_ms(const HsReader*);
bool      hs_reader_primed(const HsReader*);
uint32_t  hs_reader_underruns(const HsReader*);
uint32_t  hs_reader_reprimes(const HsReader*);
double    hs_reader_concealed_ms(const HsReader*);
double    hs_reader_jitter_p95_ms(const HsReader*);

#ifdef __cplusplus
}
#endif
