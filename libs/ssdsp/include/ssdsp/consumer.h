// Consumer side of the bus (OBS source, Bridge): reads a stream output from shared memory,
// compensates clock drift and converts the sample rate with a variable-ratio resampler.
// SPDX-License-Identifier: MIT
#pragma once

#include "ssbus/bus.h"
#include "ssdsp/dsp.h"

#include <memory>
#include <string>
#include <vector>

namespace ssdsp {

class VarResampler;

struct ConsumerStatus {
    bool     connected      = false;   // shared memory open
    bool     hubAlive       = false;   // Hub heartbeat fresh
    bool     streaming      = false;   // output currently active
    uint32_t dawRate        = 0;
    double   bufferMs       = 0;       // current (filtered) buffer fill
    double   targetMs       = 0;
    double   driftPpm       = 0;       // current correction
    uint64_t underruns      = 0;
    uint64_t overruns       = 0;
    uint64_t resyncs        = 0;
    uint32_t hubFlags       = 0;
    std::string outputName;
    std::string error;
};

class StreamConsumer {
public:
    StreamConsumer();
    ~StreamConsumer();

    // Not thread safe with pull(); call from the consumer thread or while stopped.
    void configure(const std::string& busName, int outputIndex, double bufferMs, uint32_t outRate);

    // Produces exactly `frames` frames at outRate into out[0], out[1] (planar).
    // Always fills the buffers (silence when disconnected). dtSec: time since previous call.
    void pull(float* const* out, uint32_t frames, double dtSec);

    ConsumerStatus status() const;   // snapshot (copy) - call from the same thread as pull()

private:
    void tryConnect();
    void resync(uint32_t dawRate);

    std::string busName_ = "Main";
    int         output_ = 0;
    double      bufferMs_ = 30;
    uint32_t    outRate_ = 48000;

    std::unique_ptr<ssbus::SharedMemory> shm_;
    std::unique_ptr<VarResampler> rs_;
    DriftController drift_;
    uint64_t cursor_ = 0;
    uint32_t dawRate_ = 0;
    bool     needResync_ = true;
    double   reconnectTimer_ = 0;
    double   fillFiltered_ = 0;
    float    fade_ = 0;           // 0..1 output fade (for disconnect / hub loss)
    uint64_t lastHeartbeat_ = 0;
    std::vector<float> inBuf_[2];
    ConsumerStatus st_;
};

// Thin wrapper around the speexdsp resampler with ppm-resolution ratio control.
class VarResampler {
public:
    VarResampler(uint32_t inRate, uint32_t outRate, int quality = 5);
    ~VarResampler();
    VarResampler(const VarResampler&) = delete;
    VarResampler& operator=(const VarResampler&) = delete;

    void setCorrectionPpm(int ppm);
    // Consumes up to inFrames, produces up to outFrames. Returns consumed/produced counts.
    void process(const float* const* in, uint32_t& inFrames, float* const* out, uint32_t& outFrames);
    // Input frames needed to produce `outFrames` with margin.
    uint32_t inputFor(uint32_t outFrames) const;
    void reset();
    uint32_t inRate() const { return in_; }
    uint32_t outRate() const { return out_; }

private:
    void* st_ = nullptr;
    uint32_t in_, out_, baseNum_, baseDen_;
    int ppm_ = 0;
    uint32_t lastNum_ = 0;
    uint32_t priming_ = 0;   // input still swallowed by the filter delay after reset()
};

} // namespace ssdsp
