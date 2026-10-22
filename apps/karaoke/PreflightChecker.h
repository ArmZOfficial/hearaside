#pragma once

#include <juce_core/juce_core.h>

#include <atomic>
#include <cmath>
#include <mutex>
#include <vector>

namespace hearaside::karaoke {

enum class LeakStatus {
    Untested,
    Testing,
    HeadphonesClean,    // No leak detected (closed headphones / clean)
    HeadphonesOpen,     // Very low leak (open-back headphones, safe)
    SpeakerLeakDetected // Loud leak detected (speakers / feedback risk)
};

struct PreflightResult {
    bool micDetected = false;
    float micRmsDb = -100.0f;
    float micPeakDb = -100.0f;

    LeakStatus leakStatus = LeakStatus::Untested;
    float leakEnergyDb = -100.0f;
    float correlationPeak = 0.0f;

    float rttMs = -1.0f;
    float jitterMs = -1.0f;

    bool isReady = false;          // "พร้อมร้อง ✓"
    juce::String summary;          // e.g. "พร้อมร้อง ✓"
    juce::String advice;           // actionable guidance
};

class PreflightChecker {
public:
    PreflightChecker();
    ~PreflightChecker() = default;

    void prepare(double sampleRate);
    void startCheck(float netRttMs = -1.0f, float netJitterMs = -1.0f, bool aecActive = false);
    void cancel();

    bool isTesting() const noexcept { return testing_.load(std::memory_order_relaxed); }
    PreflightResult result() const;

    // Audio thread: strictly 0 locks, 0 allocations
    // If testing, adds gentle chirp to outL, outR and records inMic into test buffer
    void processAudio(const float* inMic, float* outL, float* outR, int numSamples) noexcept;

    // Evaluates recorded mic signal against reference chirp (can be used directly in unit tests)
    static PreflightResult evaluate(const float* micSignal, int numMicSamples,
                                    const float* chirpRef, int chirpSamples,
                                    double sampleRate,
                                    float netRttMs = -1.0f, float netJitterMs = -1.0f,
                                    bool aecActive = false);

private:
    void generateChirp();
    void analyzeRecorded();

    double sampleRate_ = 48000.0;
    std::atomic<bool> testing_{ false };
    std::atomic<bool> aecActive_{ false };
    std::atomic<float> rttMs_{ -1.0f };
    std::atomic<float> jitterMs_{ -1.0f };

    std::vector<float> chirpRef_;
    std::atomic<int> chirpPlayPos_{ 0 };

    std::vector<float> micRecordBuf_;
    std::atomic<int> micRecordPos_{ 0 };
    int totalRecordSamples_ = 12000; // 250 ms at 48k

    mutable std::mutex resultMutex_;
    PreflightResult result_;
};

} // namespace hearaside::karaoke
