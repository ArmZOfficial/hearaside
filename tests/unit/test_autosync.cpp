// Auto sync under realistic sound: music with a steady beat (it repeats every beat and every bar)
// and a microphone that only hears a quiet, treble-heavy bleed of it from the headphones, with
// room noise. measureLag() must find the same lag in every reading, like HubProcessor::steadyLag
// asks (five readings 0.4 s apart, within 4 ms of each other).
#include "testing.h"

#include "ssengine/engine.h"

#include <algorithm>
#include <cmath>
#include <optional>
#include <vector>

using namespace ssengine;
using namespace ssbus;

namespace {

constexpr double kRate = 48000.0;
constexpr double kPi = 3.14159265358979323846;

float hashNoise(int64_t t, uint32_t seed) {
    uint32_t h = uint32_t(t) * 2654435761u ^ seed * 40503u;
    h ^= h >> 15; h *= 2246822519u; h ^= h >> 13; h *= 3266489917u; h ^= h >> 16;
    return float(h & 0xffff) / 65535.0f - 0.5f;
}

// A loop at 120 bpm: kick on every beat, snare on 2 and 4, closed hi-hat on eighths, a bass line
// that changes every beat and a chord pad that changes every bar.
float music(int64_t t) {
    if (t < 0) return 0.0f;
    const double s = double(t) / kRate;
    const double beat = 0.5;
    const double inBeat = std::fmod(s, beat);
    const double inEighth = std::fmod(s, beat / 2.0);
    const int beatNo = int(s / beat);
    double v = 0.0;
    v += 0.6 * std::sin(2 * kPi * (50.0 + 60.0 * std::exp(-inBeat / 0.03)) * inBeat) * std::exp(-inBeat / 0.12);   // kick
    if (beatNo % 2 == 1) v += 0.35 * hashNoise(t, 3) * std::exp(-inBeat / 0.07);                                     // snare
    v += 0.12 * (hashNoise(t, 9) - hashNoise(t - 1, 9)) * std::exp(-inEighth / 0.015);                                // hi-hat
    static const double bass[8] = { 55.0, 55.0, 65.4, 73.4, 49.0, 49.0, 61.7, 73.4 };
    v += 0.25 * std::sin(2 * kPi * bass[beatNo % 8] * s);
    static const double pad[4][3] = { { 220.0, 277.2, 329.6 }, { 196.0, 246.9, 293.7 }, { 174.6, 220.0, 261.6 }, { 196.0, 246.9, 311.1 } };
    const auto& chord = pad[(beatNo / 4) % 4];
    for (double f : chord) v += 0.05 * std::sin(2 * kPi * f * s);
    return float(v);
}

struct Rig {
    HubEngine hub;
    TrackPublisher musicTrack, micTrack;
    int64_t t = 100000;
    // the microphone: headphone bleed of what the Hub played, `roundTrip` frames later
    int64_t roundTrip = 0;
    float bleed = 0.02f, noise = 0.002f;
    float leak = 0.0f;       // music that reaches the master another way (a channel without HEARASIDE Track)
    float echo = 0.0f;       // room / reverb: a copy 23 ms later
    std::vector<float> hp;   // a crude high-pass (headphones held to a mic lose the bass)

    explicit Rig(const std::string& bus) {
        hub.connect(bus);
        hub.prepare(kRate, 1024);
        hub.maintain();
        musicTrack.connect(bus, "", 48000, 1);
        musicTrack.prepare(48000, 1);
        micTrack.connect(bus, "", 48000, 1);
        micTrack.prepare(48000, 1);
        musicTrack.mirror({ true, true, false, 0, 0, 0, 0, -1 });
        micTrack.mirror({ false, true, false, 0, 0, 0, 0, -1 });
    }

    float mic(int64_t at) const {
        const int64_t s = at - roundTrip;
        const float treble = music(s) - music(s - 1);
        const float treble2 = music(s - 1104) - music(s - 1105);
        return bleed * 4.0f * (treble + echo * treble2) + noise * hashNoise(at, 77);
    }

    // musicToMaster: phase 1 (only the music reaches the master) or phase 2 (only the mic)
    void run(double seconds, bool musicToMaster) {
        const int n = 256;
        std::vector<float> a(n), m(n), l(n), r(n);
        for (int64_t done = 0; done < int64_t(seconds * kRate); done += n) {
            for (int i = 0; i < n; ++i) { a[size_t(i)] = music(t + i); m[size_t(i)] = mic(t + i); }
            const float* pa[1] = { a.data() };
            const float* pm[1] = { m.data() };
            musicTrack.process(pa, 1, n, t, true, false, false);
            micTrack.process(pm, 1, n, t, true, false, false);
            for (int i = 0; i < n; ++i) l[size_t(i)] = r[size_t(i)] = musicToMaster ? a[size_t(i)] : m[size_t(i)] + leak * a[size_t(i)];
            float* io[2] = { l.data(), r.data() };
            HubParams p;
            hub.process(io, 2, n, p, t, true, false);
            t += n;
        }
    }

    // HubProcessor::serviceAutoSync: five readings 0.4 s apart after a settling time
    std::vector<std::optional<double>> readings(bool musicPhase, int slot, std::optional<double> removeMs = std::nullopt) {
        run(musicPhase ? 1.2 : 2.0, musicPhase);
        std::vector<std::optional<double>> out;
        for (int k = 0; k < 5; ++k) {
            if (k > 0) run(0.4, musicPhase);
            double score = 0.0;
            out.push_back(hub.measureLag(slot, musicPhase ? 0.3 : 0.1, !musicPhase, false, &score, !musicPhase, removeMs));
            std::printf("    %s reading %d: %s (score %.2f)\n", musicPhase ? "music" : "mic  ", k,
                        out.back() ? std::to_string(*out.back()).c_str() : "--", score);
        }
        return out;
    }
};

// what steadyLag accepts: at least 3 of 5 within 4 ms of their median
std::optional<double> steady(const std::vector<std::optional<double>>& rs) {
    std::vector<double> f;
    for (const auto& r : rs) if (r) f.push_back(*r);
    if (f.size() < 3) return std::nullopt;
    std::sort(f.begin(), f.end());
    const double med = f[f.size() / 2];
    double sum = 0; int agree = 0;
    for (double v : f) if (std::abs(v - med) <= 4.0) { sum += v; ++agree; }
    return agree >= 3 ? std::optional<double>(sum / agree) : std::nullopt;
}

int musicSlot(Rig& r) { return r.musicTrack.slotIndex(); }

} // namespace

TEST_CASE("auto sync: music with a beat, mic hears a quiet bleed from the headphones") {
    for (const int rtMs : { 12, 40, 95 }) {
        Rig r("autosync_beat_" + std::to_string(rtMs) + "_" + std::to_string(currentPid()));
        r.roundTrip = int64_t(rtMs * 48);
        std::printf("  round trip %d ms\n", rtMs);
        const auto ref = steady(r.readings(true, musicSlot(r)));
        REQUIRE(ref.has_value());
        const auto mic = steady(r.readings(false, musicSlot(r), ref));
        REQUIRE(mic.has_value());
        CHECK_NEAR(*mic - *ref, double(rtMs), 1.0);
    }
}

TEST_CASE("auto sync: music that reaches the master another way doesn't outvote the mic") {
    // an FX return or a channel without HEARASIDE Track isn't muted while measuring: before the fix
    // the readings flipped between 40 and 0 ms ("measurements disagreed") or settled on 0 ms
    for (const float leak : { 0.003f, 0.01f, 0.1f, 0.5f }) {
        Rig r("autosync_leak_" + std::to_string(int(leak * 1000)) + "_" + std::to_string(currentPid()));
        r.roundTrip = 40 * 48;
        r.bleed = 0.01f;
        r.leak = leak;
        std::printf("  music leak %.0f dB\n", 20.0 * std::log10(leak));
        const auto ref = steady(r.readings(true, musicSlot(r)));
        REQUIRE(ref.has_value());
        const auto mic = steady(r.readings(false, musicSlot(r), ref));
        REQUIRE(mic.has_value());
        CHECK_NEAR(*mic - *ref, 40.0, 1.0);
    }
}

TEST_CASE("auto sync: room echo, and a mic that hears nothing gives no number rather than a wrong one") {
    {
        Rig r("autosync_echo_" + std::to_string(currentPid()));
        r.roundTrip = 40 * 48; r.bleed = 0.01f; r.echo = 0.8f;
        const auto ref = steady(r.readings(true, musicSlot(r)));
        REQUIRE(ref.has_value());
        const auto mic = steady(r.readings(false, musicSlot(r), ref));
        REQUIRE(mic.has_value());
        CHECK_NEAR(*mic - *ref, 40.0, 1.0);
    }
    {
        Rig r("autosync_deaf_" + std::to_string(currentPid()));
        r.roundTrip = 40 * 48; r.bleed = 0.001f; r.noise = 0.02f;
        const auto ref = steady(r.readings(true, musicSlot(r)));
        REQUIRE(ref.has_value());
        CHECK(!steady(r.readings(false, musicSlot(r), ref)).has_value());
    }
}
