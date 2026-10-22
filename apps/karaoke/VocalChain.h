#pragma once

#include <juce_audio_basics/juce_audio_basics.h>
#include <juce_dsp/juce_dsp.h>

#include <algorithm>
#include <atomic>
#include <vector>

namespace hearaside::karaoke {

class VocalChain {
public:
    VocalChain();
    ~VocalChain() = default;

    void prepare(double sampleRate, int maxBlockSize);
    void reset();

    // Audio thread: strictly 0 allocations, 0 locks
    void process(const float* inMic, float* outL, float* outR, float* outNetwork, int numSamples);

    // Message thread controls
    void setMuted(bool muted) noexcept { muted_.store(muted, std::memory_order_relaxed); }
    bool isMuted() const noexcept { return muted_.load(std::memory_order_relaxed); }

    void setSendWet(bool wet) noexcept { sendWet_.store(wet, std::memory_order_relaxed); }
    bool isSendWet() const noexcept { return sendWet_.load(std::memory_order_relaxed); }

    void setReverbLevel(float lvl) noexcept { reverbLevel_.store(std::clamp(lvl, 0.0f, 1.0f), std::memory_order_relaxed); }
    float reverbLevel() const noexcept { return reverbLevel_.load(std::memory_order_relaxed); }

    void setEchoLevel(float lvl) noexcept { echoLevel_.store(std::clamp(lvl, 0.0f, 1.0f), std::memory_order_relaxed); }
    float echoLevel() const noexcept { return echoLevel_.load(std::memory_order_relaxed); }

    void setGateThresholdDb(float db) noexcept { gateThresholdDb_.store(db, std::memory_order_relaxed); }

    float currentGateGain() const noexcept { return currentGateGain_.load(std::memory_order_relaxed); }

private:
    double sampleRate_ = 48000.0;
    std::atomic<bool> muted_{ false };
    std::atomic<bool> sendWet_{ false };
    std::atomic<float> reverbLevel_{ 0.25f };
    std::atomic<float> echoLevel_{ 0.20f };
    std::atomic<float> gateThresholdDb_{ -45.0f };
    std::atomic<float> currentGateGain_{ 1.0f };

    // Gate state
    float gateGain_ = 1.0f;
    int gateHoldSamples_ = 0;

    // Filters (EQ)
    juce::dsp::IIR::Filter<float> lowCutFilter_;
    juce::dsp::IIR::Filter<float> midPeakFilter_;
    juce::dsp::IIR::Filter<float> highShelfFilter_;

    // Compressor envelope
    float compEnv_ = 0.0f;

    // Reverb
    juce::dsp::Reverb reverb_;

    // Echo delay line
    std::vector<float> echoBufferL_;
    std::vector<float> echoBufferR_;
    int echoWritePos_ = 0;
    float echoFilterStateL_ = 0.0f;
    float echoFilterStateR_ = 0.0f;

    // Scratch buffers preallocated in prepare()
    std::vector<float> scratchDry_;
    std::vector<float> scratchWetL_;
    std::vector<float> scratchWetR_;
};

} // namespace hearaside::karaoke
