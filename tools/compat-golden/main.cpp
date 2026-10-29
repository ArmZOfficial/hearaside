// SPDX-License-Identifier: MIT
#include "ssdsp/consumer.h"
#include "ssengine/engine.h"
#include "ssengine/friend_reader.h"
#include "provenance.h"
#include "rt_audit.h"
#include "netsim_traces.h"

#include <bit>
#include <filesystem>
#include <fstream>
#include <functional>
#include <iostream>
#include <iterator>
#include <limits>
#include <map>
#include <mutex>
#include <set>
#include <sstream>
#include <stdexcept>
#ifndef _WIN32
#include <sys/mman.h>
#endif

namespace fs = std::filesystem;
using Bytes = std::vector<unsigned char>;
using Files = std::map<std::string, Bytes>;
constexpr uint32_t kRate = 48000;
constexpr int kBlock = 256;
constexpr double kPi = 3.14159265358979323846;
constexpr uint32_t kSeed = 0x13579BDFu;

void require(bool ok, const std::string& why) { if (!ok) throw std::runtime_error(why); }

float randomSample(uint32_t& state) {
    state = state * 1664525u + 1013904223u;
    return float(state >> 8) / 8388608.0f - 1.0f;
}
float sine(uint64_t frame, uint32_t rate, double hz = 440.0) {
    return float(std::sin(2.0 * kPi * hz * double(frame % rate) / double(rate)));
}
float music(uint64_t frame, uint32_t rate) {
    constexpr double notes[] = { 220.0, 330.0, 275.0, 440.0 };
    const auto note = size_t((frame / (rate / 4)) % 4);
    const double envelope = 0.65 + 0.35 * std::sin(2.0 * kPi * double(frame % rate) / double(rate));
    return float(envelope) * (0.22f * sine(frame, rate, notes[note]) + 0.10f * sine(frame, rate, 110.0));
}
void stereo(std::vector<float>& out, const float* l, const float* r, size_t n) {
    for (size_t i = 0; i < n; ++i) { out.push_back(l[i]); out.push_back(r[i]); }
}
float peak(const std::vector<float>& values) {
    float result = 0;
    for (float value : values) { require(std::isfinite(value), "non-finite golden sample"); result = std::max(result, std::abs(value)); }
    return result;
}
Bytes encode(const std::vector<float>& values) {
    static_assert(sizeof(float) == 4 && std::numeric_limits<float>::is_iec559);
    Bytes out;
    out.reserve(values.size() * 4);
    for (float sample : values) {
        const auto word = std::bit_cast<uint32_t>(sample);
        for (int shift = 0; shift < 32; shift += 8) out.push_back(static_cast<unsigned char>((word >> shift) & 255u));
    }
    return out;
}
Bytes textBytes(const std::string& text) { return Bytes(text.begin(), text.end()); }

struct Capture {
    std::string name, scenario, columns, windows;
    double rate;
    uint32_t channels, seed;
    std::vector<float> data;
};
struct Golden {
    std::vector<Capture> captures;
    std::map<std::string, Bytes> inputs;   // non-audio input fixtures stored verbatim
    void add(std::string name, std::string scenario, std::vector<float> data, double rate = kRate,
             uint32_t channels = 2, uint32_t seed = 0, std::string columns = "[\"left\", \"right\"]", std::string windows = "[]") {
        require(!data.empty() && data.size() % channels == 0, "invalid capture dimensions: " + name);
        require(peak(data) > 1.0e-6f, "unexpected silent capture: " + name);
        captures.push_back({ std::move(name), std::move(scenario), std::move(columns), std::move(windows), rate, channels, seed, std::move(data) });
    }
    Files files() const {
        Files files;
        std::ostringstream manifest;
        manifest << "{\n  \"schema_version\": 1,\n  \"format\": \"IEEE-754 binary32 little-endian interleaved\",\n"
                 << "  \"lcg\": {\"multiplier\": 1664525, \"increment\": 1013904223, \"output\": \"float(state >> 8) / 8388608 - 1\"},\n"
                 << "  \"audio_tolerance_dbfs_for_future_ports\": -120,\n  \"captures\": [\n";
        bool first = true;
        for (const auto& c : captures) {
            require(files.emplace(c.name, encode(c.data)).second, "duplicate capture name");
            if (!first) manifest << ",\n";
            first = false;
            manifest << "    {\"file\": \"" << c.name << "\", \"scenario\": \"" << c.scenario
                     << "\", \"sample_rate_hz\": " << c.rate << ", \"frames\": " << c.data.size() / c.channels
                     << ", \"channels\": " << c.channels << ", \"seed\": " << c.seed << ", \"columns\": " << c.columns << ", \"source_frame_windows\": " << c.windows
                     << ", \"compare\": \"" << ((c.name == "inputs/noise.f32" || c.name == "inputs/impulse.f32") ? "exact" : "absolute_error_le_1e-6") << "\"}";
        }
        manifest << "\n  ]";
        if (!inputs.empty()) {
            manifest << ",\n  \"inputs\": [";
            bool firstInput = true;
            for (const auto& [name, bytes] : inputs) {
                require(files.emplace(name, bytes).second, "duplicate input name");
                manifest << (firstInput ? "" : ", ") << '"' << name << '"';
                firstInput = false;
            }
            manifest << "]";
        }
        manifest << "\n}\n";
        files.emplace("manifest.json", textBytes(manifest.str()));
        const std::string provenance = std::string("{\n  \"source_commit\": \"") + kGoldenCommit
            + "\",\n  \"compiler\": \"" + kGoldenCompiler + "\",\n  \"platform\": \"" + kGoldenPlatform
            + "\",\n  \"source_sha256\": " + kGoldenSourceHashes + "\n}\n";
        files.emplace("provenance.json", textBytes(provenance));
        return files;
    }
};

struct PrivateBus {
    std::string name = "golden_" + std::to_string(ssbus::currentPid()) + "_" + std::to_string(ssbus::randomToken());
    ~PrivateBus() {
#ifndef _WIN32
        // This tool alone owns these randomly named test segments; unlink after every engine/consumer closes.
        shm_unlink(ssbus::SharedMemory::segmentName(name).c_str());
        shm_unlink(ssbus::BeaconMap::segmentName(name).c_str());
#endif
    }
};

void dspGoldens(Golden& gold) {
    std::vector<float> tone(kRate), noise(kRate), original, impulse(4096, 0.0f);
    impulse[0] = 1.0f;
    uint32_t seed = kSeed;
    for (uint32_t i = 0; i < kRate; ++i) {
        tone[i] = 0.5f * sine(i, kRate);
        noise[i] = randomSample(seed);
        original.push_back(music(i, kRate)); original.push_back(music(i + 37, kRate));
    }
    gold.add("inputs/sine.f32", "440 Hz sine at peak 0.5", tone, kRate, 1, 0, "[\"mono\"]");
    gold.add("inputs/noise.f32", "LCG noise", noise, kRate, 1, kSeed, "[\"mono\"]");
    gold.add("inputs/impulse.f32", "unit impulse followed by silence", impulse, kRate, 1, 0, "[\"mono\"]");
    gold.add("inputs/music.f32", "original four-note synthetic music with 110 Hz bass and envelope; right offset 37 frames", original);

    ssdsp::Smoother smoother;
    smoother.prepare(kRate, 5.0); smoother.snap(0); smoother.setTarget(1);
    std::vector<float> ramp;
    for (int i = 0; i < 2048; ++i) {
        float value;
        {
            rt::Scope audio("ssdsp::Smoother");
            if (i == 600) smoother.setTarget(0.25f);
            if (i == 1200) smoother.setTarget(0);
            value = smoother.next();
        }
        ramp.push_back(value);
    }
    require(ramp.back() == 0 && !smoother.isSmoothing(), "smoother did not settle");
    gold.add("dsp/smoother.f32", "5 ms ramps: 0 to 1 at frame 0; to 0.25 at 600; to 0 at 1200", ramp, kRate, 1, 0, "[\"gain\"]");
    std::vector<float> panned;
    for (int i = 0; i < 2049; ++i) {
        float l, r;
        { rt::Scope audio("ssdsp::panGains"); ssdsp::panGains(float(i) / 1024.0f - 1.0f, l, r); }
        panned.push_back(tone[size_t(i)] * l); panned.push_back(tone[size_t(i)] * r);
    }
    gold.add("dsp/pan.f32", "440 Hz sine panned linearly from -1 to +1 over 2049 frames", panned);

    ssdsp::Limiter limiter;
    limiter.prepare(kRate, 1.0, 100.0);
    const float ceiling = ssdsp::dbToGain(-1.0f);
    limiter.setCeiling(ceiling);
    std::vector<float> limited;
    for (int block = 0; block < 188; ++block) {
        std::vector<float> l(kBlock), r(kBlock);
        for (int i = 0; i < kBlock; ++i) {
            const size_t at = size_t(block * kBlock + i) % tone.size();
            l[size_t(i)] = block < 40 ? tone[at] : block < 80 ? noise[at] * 4.0f : tone[at] * 0.25f;
            r[size_t(i)] = l[size_t(i)] * -0.75f;
            if (block == 80 && i == 0) l[0] = 3.0f;
        }
        const bool enabled = block < 160;
        { rt::Scope audio("ssdsp::Limiter::process"); limiter.process(l.data(), r.data(), kBlock, enabled); }
        if (enabled) require(std::max(peak(l), peak(r)) <= ceiling + 1e-6f, "limiter ceiling exceeded");
        stereo(limited, l.data(), r.data(), kBlock);
    }
    gold.add("dsp/limiter.f32", "1 ms lookahead 100 ms release -1 dB ceiling; quiet sine then hot LCG noise then impulse/recovery; disabled at block 160", limited, kRate, 2, kSeed);

    ssdsp::LoudnessMeter meter;
    meter.prepare(kRate);
    std::vector<float> levels, l(kBlock), r(kBlock);
    for (int block = 0; block < 750; ++block) {
        for (int i = 0; i < kBlock; ++i) l[size_t(i)] = r[size_t(i)] = (block < 562 ? 0.1f : 0.2f) * sine(uint64_t(block * kBlock + i), kRate, 1000);
        { rt::Scope audio("ssdsp::LoudnessMeter::process"); meter.process(l.data(), r.data(), kBlock); }
        levels.push_back(meter.momentary()); levels.push_back(meter.shortTerm());
    }
    require(meter.momentary() > -30 && meter.shortTerm() > -30, "LUFS capture never warmed up");
    gold.add("dsp/loudness.f32", "4 s stereo 1 kHz; peak 0.1 until block 562 then 0.2; M 400 ms and S 3 s sampled each 256 frames", levels, double(kRate) / kBlock, 2, 0, "[\"momentary_lufs\", \"short_term_lufs\"]");

    ssdsp::VarResampler resampler(44100, kRate);
    uint64_t cursor = 0;
    std::vector<float> resampled, consumption;
    for (int block = 0; block < 100; ++block) {
        const int ppm = block < 25 ? 0 : block < 50 ? 150 : block < 75 ? -200 : 0;
        uint32_t used, made = 480;
        {
            rt::Scope audio("ssdsp::VarResampler");
            if (block == 75) resampler.reset();
            resampler.setCorrectionPpm(ppm);
            used = resampler.inputFor(480);
        }
        std::vector<float> il(used), ir(used), ol(480), orr(480);
        for (uint32_t i = 0; i < used; ++i) { il[i] = music(cursor + i, 44100); ir[i] = 0.5f * sine(cursor + i, 44100, 441); }
        const float* in[2] = { il.data(), ir.data() }; float* out[2] = { ol.data(), orr.data() };
        { rt::Scope audio("ssdsp::VarResampler"); resampler.process(in, used, out, made); }
        require(made == 480, "resampler failed to fill output");
        cursor += used;
        stereo(resampled, ol.data(), orr.data(), made);
        consumption.insert(consumption.end(), { float(used), float(made), float(ppm) });
    }
    gold.add("dsp/resampler.f32", "Speex quality 5 44100 to 48000; 480-frame output blocks; ppm 0/+150/-200/0 each 25 blocks; reset at block 75", resampled);
    gold.add("dsp/resampler_counts.f32", "per-block consumed/produced frames and requested correction for resampler scenario", consumption, 100, 3, 0, "[\"input_frames\", \"output_frames\", \"requested_ppm\"]");

    ssdsp::DriftController drift;
    drift.reset(1440);
    double fill = 1440;
    std::vector<float> correction;
    for (int i = 0; i < 1200; ++i) {
        if (i == 400) fill += 128;
        if (i == 800) fill -= 64;
        double c;
        {
            rt::Scope audio("ssdsp::DriftController");
            if (i == 600) drift.setTarget(1600);
            c = drift.update(fill + double(i % 3) * 64.0, 0.01);
        }
        correction.insert(correction.end(), { float(c), float(drift.filteredError()), float(drift.target()) });
        fill += 480.0 * (1.0 + 200.0e-6) - 480.0 * (1.0 + c);
    }
    gold.add("dsp/drift.f32", "PI 200 ppm producer; 10 ms ticks; 64-frame sawtooth; +128 fill at tick 400; target 1440 to 1600 at 600; -64 fill at 800", correction, 100, 3, 0, "[\"correction\", \"filtered_error\", \"target_frames\"]");
}

void engineGoldens(Golden& gold) {
    PrivateBus privateBus;
    ssengine::HubEngine hub;
    require(hub.connect(privateBus.name), "golden Hub connect failed");
    hub.prepare(kRate, kBlock); hub.maintain();
    require(hub.role() == ssengine::HubEngine::Role::Owner, "golden Hub not owner");
    ssengine::TrackPublisher a, b;
    require(a.connect(privateBus.name, "golden-a", kRate, 1) == ssengine::TrackPublisher::Status::Connected, "Track A connect failed");
    require(b.connect(privateBus.name, "golden-b", kRate, 1) == ssengine::TrackPublisher::Status::Connected, "Track B connect failed");
    a.prepare(kRate, 1); b.prepare(kRate, 1);
    a.mirror({ true, true, false, -3.0f, -0.4f, 0, 0, 0 });
    b.mirror({ true, true, false, 0, 0.4f, 0, -3.0f, 1 });
    ssengine::HubParams params;
    std::vector<float> phones, stream, stem, l(kBlock), r(kBlock), x(kBlock), y(kBlock), sl(kBlock), sr(kBlock);
    uint64_t read = 0;
    int64_t timeline = 100000;
    for (int block = 0; block < 200; ++block) {
        if (block == 40) { rt::Scope audio("ssengine::TrackPublisher"); a.mirror({ false, true, false, -3.0f, -0.4f, 0, 0, 0 }); }
        if (block == 80) params.panic = true;
        if (block == 100) { params.panic = false; params.preview = true; }
        if (block == 120) params.preview = false;
        if (block == 140) timeline += 10000;
        if (block == 180) a.prepare(kRate, 1);
        const bool playing = !(block >= 120 && block < 140);
        const bool suspend = block >= 160 && block < 180;
        for (int i = 0; i < kBlock; ++i) { x[size_t(i)] = music(uint64_t(timeline + i), kRate); y[size_t(i)] = 0.2f * sine(uint64_t(timeline + i), kRate); }
        const float* ax[1] = { x.data() }; const float* by[1] = { y.data() };
        {
            rt::Scope audio("ssengine::TrackPublisher");
            if (!suspend) a.process(ax, 1, kBlock, timeline, playing, false, false);
            b.process(by, 1, kBlock, timeline, playing, false, false);
        }
        float agl, agr, bgl, bgr;
        ssdsp::panGains(-0.4f, agl, agr); ssdsp::panGains(0.4f, bgl, bgr);
        for (int i = 0; i < kBlock; ++i) {
            const float av = suspend ? 0.0f : x[size_t(i)] * ssdsp::dbToGain(-3.0f);
            l[size_t(i)] = av * agl + y[size_t(i)] * bgl;
            r[size_t(i)] = av * agr + y[size_t(i)] * bgr;
        }
        float* io[2] = { l.data(), r.data() };
        { rt::Scope audio("ssengine::HubEngine::process"); hub.process(io, 2, kBlock, params, playing ? timeline : ssengine::kNoTime, playing, false); }
        stereo(phones, l.data(), r.data(), kBlock);
        auto& bus = *hub.bus();
        float* dest[2] = { sl.data(), sr.data() };
        require(ssbus::ringReadAt(bus.streamAudio[0], bus.streamHeader.writePos[0], read, dest, kBlock) == ssbus::ReadResult::Ok, "stream read failed");
        stereo(stream, sl.data(), sr.data(), kBlock);
        require(ssbus::ringReadAt(bus.streamAudio[1], bus.streamHeader.writePos[1], read, dest, kBlock) == ssbus::ReadResult::Ok, "stem read failed");
        stereo(stem, sl.data(), sr.data(), kBlock);
        read += kBlock; timeline += kBlock;
    }
    const std::string scenario = "two mono TrackPublishers with original music/sine; -3 dB and pan -0.4/+0.4; stems 0/1; mon A off at block 40; panic 80-99; preview 100-119; stop 120-139; seek +10000 at 140; A suspended 160-179; prepare epoch at 180";
    gold.add("engine/phones.f32", scenario + "; Hub headphone output", phones);
    gold.add("engine/stream.f32", scenario + "; live Stream Mix through default limiter", stream);
    gold.add("engine/stem0.f32", scenario + "; stem 0 from Track A", stem);
}

void consumerGolden(Golden& gold) {
    PrivateBus privateBus;
    ssbus::SharedMemory::Status status{};
    auto memory = ssbus::SharedMemory::open(privateBus.name, status);
    require(memory != nullptr, "consumer bus create failed");
    auto& bus = memory->layout();
    bus.header.hubOwnerToken.store(1); bus.header.hubOwnerPid.store(ssbus::currentPid());
    // Pin test liveness so scheduler delays cannot affect StreamConsumer's heartbeat gate.
    bus.header.hubHeartbeatNs.store(std::numeric_limits<uint64_t>::max());
    bus.header.hubBlockSize.store(kBlock);
    bus.streamHeader.sampleRate.store(44100); bus.streamHeader.active.store(1);
    ssdsp::StreamConsumer consumer;
    consumer.setReportToHub(false); consumer.configure(privateBus.name, 0, 30.0, kRate);
    uint64_t produced = 0;
    double producerTime = 0;
    std::vector<float> captured, trace, input(kBlock), ol(480), orr(480);
    for (int tick = 0; tick < 300; ++tick) {
        const double now = double(tick) * 0.010;
        while (producerTime <= now) {
            for (int i = 0; i < kBlock; ++i) input[size_t(i)] = 0.5f * sine(produced + uint64_t(i), 44100, 441);
            const float* source[1] = { input.data() };
            ssbus::ringWrite(bus.streamAudio[0], bus.streamHeader.writePos[0], source, 1, kBlock);
            produced += kBlock; producerTime += double(kBlock) / (44100.0 * (1.0 + 150.0e-6));
        }
        float* out[2] = { ol.data(), orr.data() };
        { rt::Scope audio("ssdsp::StreamConsumer::pull"); consumer.pull(out, 480, 0.010); }
        stereo(captured, ol.data(), orr.data(), 480);
        const auto st = consumer.status();
        require(st.connected && st.hubAlive, "consumer lost pinned live bus");
        trace.insert(trace.end(), { float(st.bufferMs), float(st.targetMs), float(st.driftPpm), float(st.underruns), float(st.resyncs) });
    }
    require(consumer.status().streaming, "consumer failed to stream");
    gold.add("dsp/consumer.f32", "StreamConsumer fixed 30 ms buffer 44100 to 48000; 256-frame producer +150 ppm; 480-frame consumer every 10 ms for 3 s; isolated bus with pinned-live heartbeat", captured);
    gold.add("dsp/consumer_status.f32", "one snapshot per consumer pull; same consumer scenario", trace, 100, 5, 0, "[\"buffer_ms\", \"target_ms\", \"drift_ppm\", \"underruns\", \"resyncs\"]");
}

struct FriendRun {
    std::vector<float> captured, trace;
    uint64_t reprimes = 0;
    bool endAudible = false;
};

// Packets of 10 ms of 400 ms singing/200 ms pause arrive at arrival(index) ms; three 0.5 s output windows are kept.
// No lost time is relabeled contiguous audio: windows are in 48 kHz output-frame coordinates.
FriendRun runFriend(uint32_t sourceRate, int blocks, const std::function<double(int)>& arrival,
                    const uint64_t (&windows)[3], uint64_t audibleFrom, const char* auditLabel) {
    auto ring = std::make_unique<ssbus::ChannelRing>();
    std::atomic<uint64_t> write{0};
    ssengine::FriendReader reader;
    reader.prepare(kRate, kBlock);
    const int packetFrames = int(sourceRate / 100);
    std::vector<float> packet(static_cast<size_t>(packetFrames)), l(kBlock), r(kBlock);
    const float* source[1] = { packet.data() }; float* dest[2] = { l.data(), r.data() };
    FriendRun run;
    int next = 0;
    double cachedArrival = arrival(0), lastArrival = 0;
    for (int block = 0; block < blocks; ++block) {
        const double now = double(block * kBlock) * 1000.0 / kRate;
        while (std::max(lastArrival, cachedArrival) <= now) {
            for (int i = 0; i < packetFrames; ++i) {
                const uint64_t frame = uint64_t(next) * uint64_t(packetFrames) + uint64_t(i);
                packet[size_t(i)] = (frame / (sourceRate / 10)) % 6 < 4 ? 0.5f * sine(frame, sourceRate) : 0.0f;
            }
            ssbus::ringWrite(*ring, write, source, 1, uint32_t(packetFrames));
            lastArrival = std::max(lastArrival, cachedArrival);
            cachedArrival = arrival(++next);
        }
        { rt::Scope audio(auditLabel); reader.pull(*ring, write, sourceRate, dest, kBlock); }
        const uint64_t start = uint64_t(block) * kBlock;
        for (int i = 0; i < kBlock; ++i) {
            const uint64_t frame = start + uint64_t(i);
            for (uint64_t w : windows) if (frame >= w && frame < w + 24000) { run.captured.push_back(l[size_t(i)]); run.captured.push_back(r[size_t(i)]); }
            if (frame >= audibleFrom && std::abs(l[size_t(i)]) > 0.1f) run.endAudible = true;
        }
        run.trace.insert(run.trace.end(), { float(reader.bufferMs()), float(reader.jitterP95Ms()), float(reader.underruns()),
            float(reader.reprimes()), float(reader.concealedMs()), reader.primed() ? 1.0f : 0.0f });
    }
    run.reprimes = reader.reprimes();
    return run;
}

std::string windowJson(const uint64_t (&windows)[3]) {
    std::string json = "[";
    for (int i = 0; i < 3; ++i) json += std::string(i ? "," : "") + "{\"source_start_frame\":" + std::to_string(windows[i])
        + ",\"frames\":24000,\"destination_start_frame\":" + std::to_string(i * 24000) + "}";
    return json + "]";
}
constexpr const char* kFriendColumns = "[\"buffer_ms\", \"jitter_p95_ms\", \"underruns\", \"reprimes\", \"concealed_ms\", \"primed\"]";

void friendGolden(Golden& gold, uint32_t sourceRate, bool outage) {
    const uint32_t packetSeed = kSeed + sourceRate;
    uint32_t seed = packetSeed;
    auto arrival = [&](int index) {
        const double due = 40.0 + index * (10.0 / (1.0 + 200.0e-6));
        const double jitter = double(randomSample(seed)) * 5.0;
        if (outage && due >= 10000.0 && due < 10500.0) return 10500.0;
        if (!outage && due >= 10000.0 && due < 10120.0) return 10120.0;
        return due + jitter;
    };
    static constexpr uint64_t windows[3] = { 0, 480000, 1200000 };
    const auto run = runFriend(sourceRate, 4875 /* exactly 26 simulated seconds */, arrival, windows, 1200000, "ssengine::FriendReader::pull");
    require(run.reprimes >= 1, "friend scenario did not exercise reprime");
    require(run.endAudible, "friend did not recover audible output");
    const std::string name = outage ? "outage_48000" : "stall_44100";
    const std::string scenario = "FriendReader " + std::to_string(sourceRate) + " to 48000; 10 ms mono packets +200 ppm; LCG jitter +/-5 ms in arrival order; 400 ms singing/200 ms pause; "
        + (outage ? "500 ms backlog at 10 s" : "120 ms backlog at 10 s")
        + "; 26 s simulation; audio windows [0,24000),[480000,504000),[1200000,1224000) in 48 kHz output-frame coordinates";
    require(run.captured.size() == 72000 * 2, "friend audio windows incomplete");
    gold.add("friend/" + name + ".f32", scenario, run.captured, kRate, 2, packetSeed, "[\"left\", \"right\"]", windowJson(windows));
    gold.add("friend/" + name + "_status.f32", "per-256-frame-block trace over all 26 s; " + scenario, run.trace, double(kRate) / kBlock, 6, packetSeed, kFriendColumns);
}

// Arrival times recorded through the real tools/net-sim proxy (tools/net-sim/record-trace.mjs), replayed exactly.
std::vector<double> parseTrace(const NetsimTrace& trace) {
    std::istringstream text(std::string(reinterpret_cast<const char*>(trace.data), trace.size));
    std::vector<double> arrivalMs;
    std::string line;
    while (std::getline(text, line)) {
        if (line.empty() || line[0] == '#') continue;
        std::istringstream fields(line);
        long long index = -1, arrivalUs = -1, sentUs = -1;
        require(bool(fields >> index >> arrivalUs >> sentUs) && index == (long long) arrivalMs.size() && arrivalUs >= 0,
            std::string("invalid net-sim trace line in ") + trace.name);
        require(arrivalMs.empty() || double(arrivalUs) / 1000.0 >= arrivalMs.back(), std::string("trace out of order: ") + trace.name);
        arrivalMs.push_back(double(arrivalUs) / 1000.0);
    }
    require(arrivalMs.size() == 2000, std::string("net-sim trace must hold 20 s of 10 ms packets: ") + trace.name);
    return arrivalMs;
}

void netsimGolden(Golden& gold, const NetsimTrace& trace) {
    const auto arrivalMs = parseTrace(trace);
    auto arrival = [&](int index) {
        return size_t(index) < arrivalMs.size() ? 40.0 + arrivalMs[size_t(index)] : std::numeric_limits<double>::infinity();
    };
    static constexpr uint64_t windows[3] = { 0, 456000, 912000 };
    const auto run = runFriend(kRate, 3937 /* 21 s: the 20 s trace and its drain */, arrival, windows, 912000, "ssengine::FriendReader::pull (net-sim)");
    require(run.endAudible, std::string("friend not audible at the end of net-sim trace ") + trace.name);
    require(run.captured.size() == 72000 * 2, "net-sim audio windows incomplete");
    const std::string name = trace.name;
    const std::string scenario = "FriendReader 48000 to 48000; 10 ms mono packets, 400 ms singing/200 ms pause; arrivals replayed from traces/" + name
        + ".trace (real tools/net-sim TCP line) plus 40 ms; 21 s simulation; audio windows [0,24000),[456000,480000),[912000,936000) in 48 kHz output-frame coordinates";
    gold.inputs.emplace("traces/" + name + ".trace", Bytes(trace.data, trace.data + trace.size));
    gold.add("friend/" + name + ".f32", scenario, run.captured, kRate, 2, 0, "[\"left\", \"right\"]", windowJson(windows));
    gold.add("friend/" + name + "_status.f32", "per-256-frame-block trace over all 21 s; " + scenario, run.trace, double(kRate) / kBlock, 6, 0, kFriendColumns);
}

enum class Corpus { Core, Netsim };

Files generate(Corpus corpus = Corpus::Core) {
    Golden golden;
    if (corpus == Corpus::Netsim) {
        for (const auto& trace : kNetsimTraces) netsimGolden(golden, trace);
        return golden.files();
    }
    dspGoldens(golden); engineGoldens(golden); consumerGolden(golden);
    friendGolden(golden, 44100, false); friendGolden(golden, 48000, true);
    return golden.files();
}

// Allocations the unchanged C++ sources make on the audio thread, counted by --rt-audit with glibc.
// They are baseline findings, not allowances for the Rust port: PLAN R2.4 requires zero.
struct KnownRtException { const char* label; uint64_t maxAllocations; const char* cause; };
constexpr KnownRtException kKnownRtExceptions[] = {
    { "ssdsp::StreamConsumer::pull", 10, "first pull opens the shared memory (tryConnect) and resync constructs the Speex VarResampler" },
    { "ssengine::FriendReader::pull", 1, "setCorrectionPpm: speex_resampler_set_rate_frac reallocates the filter when a correction lengthens it" },
    { "ssengine::FriendReader::pull (net-sim)", 3, "the same Speex filter reallocation, once in each of the three replayed readers" },
};

int rtAudit() {
    std::mutex probe;
    require(rt::selfTest(probe), "real-time audit detectors did not fire");
    rt::reset();
    const auto files = generate(Corpus::Core).size() + generate(Corpus::Netsim).size();
    bool ok = true;
    for (int i = 0; i < rt::g_siteCount.load(); ++i) {
        const auto& site = rt::g_sites[i];
        const std::string label = site.label;
        if (label == "rt-audit self-test") continue;
        const uint64_t allocations = site.allocations.load(), locks = site.locks.load();
        const KnownRtException* known = nullptr;
        for (const auto& e : kKnownRtExceptions) if (label == e.label) known = &e;
        const bool pass = locks == 0 && allocations <= (known ? known->maxAllocations : 0);
        ok = ok && pass;
        std::cout << (!pass ? "FAIL  " : allocations ? "KNOWN " : "ok    ") << label << ": " << allocations << " allocations, "
                  << (rt::kDetectsLocks ? std::to_string(locks) + " blocking locks" : std::string("locks not detected here"));
        if (known && allocations) std::cout << " (C++ baseline, limit " << known->maxAllocations << ": " << known->cause << ')';
        std::cout << '\n';
    }
    require(ok, "audio-thread calls allocated or blocked beyond the recorded C++ baseline");
    std::cout << "real-time audit: " << files << " core and net-sim golden files generated; no audio-thread allocation"
              << (rt::kDetectsLocks ? " or blocking lock" : "") << " beyond the recorded C++ baseline\n";
    return 0;
}

Bytes readFile(const fs::path& file) {
    std::ifstream in(file, std::ios::binary);
    require(bool(in), "cannot read " + file.string());
    Bytes bytes(std::istreambuf_iterator<char>(in), {});
    require(!in.bad(), "read failed: " + file.string());
    return bytes;
}
bool equalBytes(const Bytes& expected, const Bytes& actual) { return expected == actual; }

bool equalFloats(const Bytes& expected, const Bytes& actual) {
    if (expected.size() != actual.size() || expected.size() % 4 != 0) return false;
    auto sample = [](const Bytes& bytes, size_t offset) {
        uint32_t bits = 0;
        for (int i = 0; i < 4; ++i) bits |= uint32_t(bytes[offset + size_t(i)]) << (i * 8);
        return std::bit_cast<float>(bits);
    };
    for (size_t at = 0; at < expected.size(); at += 4) {
        const auto a = sample(expected, at), b = sample(actual, at);
        if (!std::isfinite(a) || !std::isfinite(b) || std::abs(double(a) - double(b)) > 1.0e-6) return false;
    }
    return true;
}

int main(int argc, char** argv) {
    try {
        // A leading --netsim selects the net-sim replay corpus (compat/golden/netsim) instead of core.
        const bool netsim = argc > 1 && std::string(argv[1]) == "--netsim";
        const Corpus corpus = netsim ? Corpus::Netsim : Corpus::Core;
        if (netsim) { --argc; ++argv; }
        if (argc == 2 && std::string(argv[1]) == "--self-test") {
            const auto first = generate(corpus), second = generate(corpus);
            require(first == second, "golden generation is not repeatable in one process");
            auto mutated = first.begin()->second;
            require(!mutated.empty(), "empty capture");
            mutated[0] ^= 1;
            require(!equalBytes(first.begin()->second, mutated), "comparison accepted changed byte");
            mutated.pop_back();
            require(!equalBytes(first.begin()->second, mutated), "comparison accepted truncated data");
            require(!equalFloats(encode({0.0f}), encode({0.01f})), "numeric comparison accepted changed sample");
            require(equalFloats(encode({0.0f}), encode({0.0000005f})), "numeric comparison rejected sub-tolerance roundoff");
            require(!equalFloats(encode({0.0f}), encode({std::numeric_limits<float>::quiet_NaN()})), "numeric comparison accepted NaN");
            std::cout << "golden determinism: " << first.size() << " files identical; mutation/truncation rejected\n";
            return 0;
        }
        if (argc == 2 && std::string(argv[1]) == "--rt-audit" && !netsim) return rtAudit();
        require(argc == 3 && (std::string(argv[1]) == "--output" || std::string(argv[1]) == "--check" || std::string(argv[1]) == "--compare"),
            "usage: hs_compat_golden [--netsim] --output <new-directory> | --check <fixture-directory> | --compare <fixture-directory> | --self-test\n"
            "       hs_compat_golden --rt-audit");
        const bool check = std::string(argv[1]) != "--output";
        const bool numeric = std::string(argv[1]) == "--compare";
        const fs::path directory(argv[2]);
        if (!check) require(!fs::exists(directory) || (fs::is_directory(directory) && fs::is_empty(directory)),
            "output directory must be new or empty; existing goldens are never overwritten");
        const auto files = generate(corpus);
        if (check) {
            require(fs::is_directory(directory), "fixture directory missing: " + directory.string());
            std::set<std::string> expected, actual;
            for (const auto& [name, bytes] : files) { (void) bytes; expected.insert(name); }
            for (const auto& entry : fs::recursive_directory_iterator(directory)) {
                if (entry.is_regular_file()) actual.insert(fs::relative(entry.path(), directory).generic_string());
            }
            require(expected == actual, "fixture inventory differs (missing or extra files): " + directory.string());
        }
        for (const auto& [name, bytes] : files) {
            const auto path = directory / name;
            if (check) {
                const auto expected = readFile(path);
                if (name == "provenance.json") continue; // Reference compiler/source identity stays attached to the recorded baseline.
                const bool exact = !numeric || !name.ends_with(".f32") || name == "inputs/noise.f32" || name == "inputs/impulse.f32";
                require(exact ? equalBytes(expected, bytes) : equalFloats(expected, bytes), "golden differs: " + path.string());
            } else {
                fs::create_directories(path.parent_path());
                std::ofstream out(path, std::ios::binary);
                require(bool(out), "cannot write " + path.string());
                out.write(reinterpret_cast<const char*>(bytes.data()), std::streamsize(bytes.size()));
                out.close();
                require(bool(out), "write failed: " + path.string());
            }
        }
        std::cout << (check ? "golden matches: " : "golden generated: ") << files.size() << " files\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << "hs_compat_golden: " << error.what() << '\n';
        return 1;
    }
}
