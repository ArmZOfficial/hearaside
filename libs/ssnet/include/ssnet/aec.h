#pragma once

#include <cstddef>
#include <cstdint>
#include <vector>

struct SpeexEchoState_;
struct SpeexPreprocessState_;

namespace ssnet {

enum class AecMode : uint8_t {
    Off = 0,
    On = 1,
    Auto = 2
};

// Acoustic Echo Cancellation (AEC) and residual suppression using SpeexDSP MDF + preprocessor.
// Designed for friends singing or talking over speaker playback.
class FriendEcho {
public:
    FriendEcho();
    ~FriendEcho();

    FriendEcho(const FriendEcho&) = delete;
    FriendEcho& operator=(const FriendEcho&) = delete;
    FriendEcho(FriendEcho&&) noexcept;
    FriendEcho& operator=(FriendEcho&&) noexcept;

    // Initializes the AEC engine.
    // sampleRate: typically 48000 Hz.
    // frameSize: number of samples processed per step (typically 480 = 10 ms).
    // filterLengthMs: tail length of acoustic room impulse response (e.g. 150-250 ms, default 200 ms).
    bool init(int sampleRate = 48000, int frameSize = 480, int filterLengthMs = 200);

    // Resets filter states and internal buffers without reallocating
    void reset();

    // AEC operational mode (Off, On, Auto)
    void setMode(AecMode mode) noexcept { mode_ = mode; }
    AecMode mode() const noexcept { return mode_; }

    // Feeds far-end reference audio (what the Hub is sending to this friend, downmixed to mono)
    void pushReference(const float* ref, size_t n);

    // Processes near-end microphone input from this friend.
    // micIn: near-end microphone signal (mono, n samples)
    // cleanOut: output signal with echo removed (mono, n samples)
    // n: number of samples (must match frameSize, e.g. 480)
    // delaySamples: measured acoustic round-trip delay in samples (e.g. RTT + speaker/mic latency).
    //               If 0, uses reference with 0 additional delay.
    void process(const float* micIn, float* cleanOut, size_t n, int delaySamples = 0);

    // Returns current Echo Return Loss Enhancement (ERLE) in dB measured during far-end speech
    float erleDb() const noexcept { return erleDb_; }

    // Flag indicating whether acoustic leak was detected
    bool leakDetected() const noexcept { return leakDetected_; }
    // Called by the Hub at 20 Hz. Auto switches on after kLeakLatchTicks leak readings in a row and stays on until
    // init()/reset(): the detector hears the mic after cancelling, so a working canceller would switch itself off again.
    void setLeakDetected(bool detected) noexcept {
        leakRun_ = detected ? leakRun_ + 1 : 0;
        if (leakRun_ >= kLeakLatchTicks) leakDetected_ = true;
    }
    static constexpr int kLeakLatchTicks = 40;   // 2 s

    int sampleRate() const noexcept { return sampleRate_; }
    int frameSize() const noexcept { return frameSize_; }

private:
    void cleanup();

    int sampleRate_ { 48000 };
    int frameSize_ { 480 };
    int filterLength_ { 9600 };
    AecMode mode_ { AecMode::Auto };
    bool leakDetected_ { false };
    int leakRun_ { 0 };
    float erleDb_ { 0.0f };

    SpeexEchoState_* echoState_ { nullptr };
    SpeexPreprocessState_* prepState_ { nullptr };

    // Ring buffer storing far-end reference audio
    std::vector<float> refRing_;
    size_t refWritePos_ { 0 };

    // Scratch buffers for 16-bit integer conversion required by Speex
    std::vector<int16_t> spxRec_;
    std::vector<int16_t> spxPlay_;
    std::vector<int16_t> spxOut_;
};

} // namespace ssnet
