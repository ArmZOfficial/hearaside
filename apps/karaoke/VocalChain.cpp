#include "VocalChain.h"

#include <cmath>
#include <cstring>

namespace hearaside::karaoke {

VocalChain::VocalChain() = default;

void VocalChain::prepare(double sampleRate, int maxBlockSize) {
    sampleRate_ = sampleRate > 0 ? sampleRate : 48000.0;
    const int allocSize = std::max(maxBlockSize, 2048);

    scratchDry_.assign(allocSize, 0.0f);
    scratchWetL_.assign(allocSize, 0.0f);
    scratchWetR_.assign(allocSize, 0.0f);

    // 2 seconds delay ring buffer
    const int echoLen = juce::roundToInt(sampleRate_ * 2.0);
    echoBufferL_.assign(echoLen, 0.0f);
    echoBufferR_.assign(echoLen, 0.0f);
    echoWritePos_ = 0;
    echoFilterStateL_ = 0.0f;
    echoFilterStateR_ = 0.0f;

    // Filters: 80 Hz highpass, 2.5 kHz peak, 10 kHz shelf
    *lowCutFilter_.coefficients = *juce::dsp::IIR::Coefficients<float>::makeHighPass(sampleRate_, 80.0f);
    *midPeakFilter_.coefficients = *juce::dsp::IIR::Coefficients<float>::makePeakFilter(sampleRate_, 2500.0f, 1.0f, 1.25f);
    *highShelfFilter_.coefficients = *juce::dsp::IIR::Coefficients<float>::makeHighShelf(sampleRate_, 10000.0f, 0.707f, 1.15f);

    juce::dsp::ProcessSpec spec;
    spec.sampleRate = sampleRate_;
    spec.maximumBlockSize = allocSize;
    spec.numChannels = 2;

    reverb_.prepare(spec);
    juce::dsp::Reverb::Parameters p;
    p.roomSize = 0.55f;
    p.damping = 0.35f;
    p.wetLevel = 1.0f;
    p.dryLevel = 0.0f;
    p.width = 1.0f;
    reverb_.setParameters(p);

    reset();
}

void VocalChain::reset() {
    lowCutFilter_.reset();
    midPeakFilter_.reset();
    highShelfFilter_.reset();
    reverb_.reset();
    std::fill(echoBufferL_.begin(), echoBufferL_.end(), 0.0f);
    std::fill(echoBufferR_.begin(), echoBufferR_.end(), 0.0f);
    echoWritePos_ = 0;
    echoFilterStateL_ = 0.0f;
    echoFilterStateR_ = 0.0f;
    gateGain_ = 1.0f;
    gateHoldSamples_ = 0;
    compEnv_ = 0.0f;
}

void VocalChain::process(const float* inMic, float* outL, float* outR, float* outNetwork, int numSamples) {
    if (inMic == nullptr || numSamples <= 0) {
        if (outNetwork != nullptr) std::memset(outNetwork, 0, numSamples * sizeof(float));
        if (outL != nullptr) std::memset(outL, 0, numSamples * sizeof(float));
        if (outR != nullptr) std::memset(outR, 0, numSamples * sizeof(float));
        return;
    }

    const bool isMuted = muted_.load(std::memory_order_relaxed);
    if (isMuted) {
        if (outNetwork != nullptr) std::memset(outNetwork, 0, numSamples * sizeof(float));
        if (outL != nullptr) std::memset(outL, 0, numSamples * sizeof(float));
        if (outR != nullptr) std::memset(outR, 0, numSamples * sizeof(float));
        return;
    }

    const int n = std::min(numSamples, (int)scratchDry_.size());
    const float gateThreshDb = gateThresholdDb_.load(std::memory_order_relaxed);
    const float gateThreshLin = std::pow(10.0f, gateThreshDb * 0.05f);
    const int holdSamples = juce::roundToInt(sampleRate_ * 0.040); // 40 ms hold

    // Coefficients for smooth gate slew
    const float gateAttackCoeff = 0.08f;
    const float gateReleaseCoeff = 0.003f;

    // Step 1: Gate, EQ, Comp on dry microphone
    for (int i = 0; i < n; ++i) {
        float s = inMic[i];
        const float absS = std::abs(s);

        // Noise gate
        if (absS >= gateThreshLin) {
            gateHoldSamples_ = holdSamples;
        } else if (gateHoldSamples_ > 0) {
            --gateHoldSamples_;
        }
        const float targetGain = (gateHoldSamples_ > 0 || absS >= gateThreshLin) ? 1.0f : 0.0f;
        gateGain_ += (targetGain - gateGain_) * (targetGain > gateGain_ ? gateAttackCoeff : gateReleaseCoeff);
        s *= gateGain_;

        // EQ (Highpass -> Mid Peak -> High Shelf)
        s = lowCutFilter_.processSample(s);
        s = midPeakFilter_.processSample(s);
        s = highShelfFilter_.processSample(s);

        // Vocal Compressor (3:1 ratio above -16 dBFS)
        const float rectified = std::abs(s);
        compEnv_ += (rectified - compEnv_) * (rectified > compEnv_ ? 0.02f : 0.002f);
        constexpr float compThresh = 0.158f; // ~ -16 dBFS
        float compGain = 1.0f;
        if (compEnv_ > compThresh) {
            const float over = compEnv_ / compThresh;
            compGain = std::pow(over, -0.66f); // 3:1 ratio
        }
        constexpr float makeup = 1.25f; // +2 dB
        s = s * compGain * makeup;

        scratchDry_[i] = s;
    }
    currentGateGain_.store(gateGain_, std::memory_order_relaxed);

    // Step 2: Ambience (Reverb + Stereo Echo)
    const float revLvl = reverbLevel_.load(std::memory_order_relaxed);
    const float echoLvl = echoLevel_.load(std::memory_order_relaxed);

    // Reverb pass
    for (int i = 0; i < n; ++i) {
        scratchWetL_[i] = scratchDry_[i];
        scratchWetR_[i] = scratchDry_[i];
    }
    if (revLvl > 0.001f) {
        float* channels[2] = { scratchWetL_.data(), scratchWetR_.data() };
        juce::dsp::AudioBlock<float> block(channels, 2, n);
        juce::dsp::ProcessContextReplacing<float> context(block);
        reverb_.process(context);
        for (int i = 0; i < n; ++i) {
            scratchWetL_[i] *= revLvl;
            scratchWetR_[i] *= revLvl;
        }
    } else {
        std::fill_n(scratchWetL_.data(), n, 0.0f);
        std::fill_n(scratchWetR_.data(), n, 0.0f);
    }

    // Echo pass (ping-pong delay: L 170ms, R 200ms)
    if (echoLvl > 0.001f && !echoBufferL_.empty()) {
        const int echoCap = (int)echoBufferL_.size();
        const int delayL = juce::roundToInt(sampleRate_ * 0.170);
        const int delayR = juce::roundToInt(sampleRate_ * 0.200);

        for (int i = 0; i < n; ++i) {
            const int readPosL = (echoWritePos_ - delayL + echoCap) % echoCap;
            const int readPosR = (echoWritePos_ - delayR + echoCap) % echoCap;

            float tapL = echoBufferL_[readPosL];
            float tapR = echoBufferR_[readPosR];

            // 1st order lowpass on feedback (damping at ~3.5 kHz)
            echoFilterStateL_ += (tapL - echoFilterStateL_) * 0.35f;
            echoFilterStateR_ += (tapR - echoFilterStateR_) * 0.35f;

            // Feedback back into ring buffer
            constexpr float feedback = 0.32f;
            echoBufferL_[echoWritePos_] = scratchDry_[i] + (echoFilterStateR_ * feedback);
            echoBufferR_[echoWritePos_] = scratchDry_[i] + (echoFilterStateL_ * feedback);

            echoWritePos_ = (echoWritePos_ + 1) % echoCap;

            scratchWetL_[i] += tapL * echoLvl;
            scratchWetR_[i] += tapR * echoLvl;
        }
    }

    // Step 3: Mix to local headphones
    if (outL != nullptr && outR != nullptr) {
        for (int i = 0; i < n; ++i) {
            outL[i] = scratchDry_[i] + scratchWetL_[i];
            outR[i] = scratchDry_[i] + scratchWetR_[i];
        }
    }

    // Step 4: Network send (dry or wet)
    if (outNetwork != nullptr) {
        const bool sendWet = sendWet_.load(std::memory_order_relaxed);
        if (sendWet) {
            for (int i = 0; i < n; ++i) {
                outNetwork[i] = scratchDry_[i] + 0.5f * (scratchWetL_[i] + scratchWetR_[i]);
            }
        } else {
            std::memcpy(outNetwork, scratchDry_.data(), n * sizeof(float));
        }
    }
}

} // namespace hearaside::karaoke
