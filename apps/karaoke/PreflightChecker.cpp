#include "PreflightChecker.h"

#include <juce_events/juce_events.h>

#include <algorithm>
#include <cmath>

namespace hearaside::karaoke {

PreflightChecker::PreflightChecker() {
    prepare(48000.0);
}

void PreflightChecker::prepare(double sampleRate) {
    sampleRate_ = sampleRate > 0 ? sampleRate : 48000.0;
    totalRecordSamples_ = int(sampleRate_ * 0.250); // 250 ms
    micRecordBuf_.assign(size_t(totalRecordSamples_), 0.0f);
    generateChirp();
}

void PreflightChecker::generateChirp() {
    const int chirpSamples = int(sampleRate_ * 0.050); // 50 ms
    chirpRef_.resize(size_t(chirpSamples));

    const double f0 = 500.0;   // start freq
    const double f1 = 2500.0;  // end freq
    const double T = double(chirpSamples) / sampleRate_;
    const double k = (f1 - f0) / T;
    const double twoPi = 2.0 * 3.14159265358979323846;

    for (int i = 0; i < chirpSamples; ++i) {
        const double t = double(i) / sampleRate_;
        // Phase for linear sweep f(t) = f0 + k*t
        const double phase = twoPi * (f0 * t + 0.5 * k * t * t);
        // Hann window: 0.5 * (1 - cos(2*pi*i/N))
        const double win = 0.5 * (1.0 - std::cos(twoPi * double(i) / double(chirpSamples)));
        // -18 dBFS amplitude (~0.125f)
        chirpRef_[size_t(i)] = float(0.125 * win * std::sin(phase));
    }
}

void PreflightChecker::startCheck(float netRttMs, float netJitterMs, bool aecActive) {
    cancel();
    rttMs_.store(netRttMs, std::memory_order_relaxed);
    jitterMs_.store(netJitterMs, std::memory_order_relaxed);
    aecActive_.store(aecActive, std::memory_order_relaxed);

    std::fill(micRecordBuf_.begin(), micRecordBuf_.end(), 0.0f);
    chirpPlayPos_.store(0, std::memory_order_relaxed);
    micRecordPos_.store(0, std::memory_order_relaxed);

    {
        std::lock_guard<std::mutex> lk(resultMutex_);
        result_ = PreflightResult{};
        result_.leakStatus = LeakStatus::Testing;
        result_.summary = juce::String::fromUTF8("กำลังตรวจสอบอุปกรณ์เสียงและระบบ...");
    }

    testing_.store(true, std::memory_order_release);
}

void PreflightChecker::cancel() {
    testing_.store(false, std::memory_order_relaxed);
}

PreflightResult PreflightChecker::result() const {
    std::lock_guard<std::mutex> lk(resultMutex_);
    return result_;
}

void PreflightChecker::processAudio(const float* inMic, float* outL, float* outR, int numSamples) noexcept {
    if (!testing_.load(std::memory_order_acquire) || numSamples <= 0) return;

    // 1. Play chirp
    int playPos = chirpPlayPos_.load(std::memory_order_relaxed);
    const int chirpLen = int(chirpRef_.size());
    if (playPos < chirpLen) {
        const int count = std::min(numSamples, chirpLen - playPos);
        for (int i = 0; i < count; ++i) {
            const float s = chirpRef_[size_t(playPos + i)];
            if (outL != nullptr) outL[i] += s;
            if (outR != nullptr) outR[i] += s;
        }
        chirpPlayPos_.store(playPos + count, std::memory_order_relaxed);
    }

    // 2. Record mic input
    int recPos = micRecordPos_.load(std::memory_order_relaxed);
    if (recPos < totalRecordSamples_) {
        const int count = std::min(numSamples, totalRecordSamples_ - recPos);
        for (int i = 0; i < count; ++i) {
            micRecordBuf_[size_t(recPos + i)] = (inMic != nullptr) ? inMic[i] : 0.0f;
        }
        recPos += count;
        micRecordPos_.store(recPos, std::memory_order_relaxed);

        if (recPos >= totalRecordSamples_) {
            testing_.store(false, std::memory_order_release);
            juce::MessageManager::callAsync([this] { analyzeRecorded(); });
        }
    }
}

void PreflightChecker::analyzeRecorded() {
    const auto r = evaluate(micRecordBuf_.data(), int(micRecordBuf_.size()),
                            chirpRef_.data(), int(chirpRef_.size()),
                            sampleRate_,
                            rttMs_.load(std::memory_order_relaxed),
                            jitterMs_.load(std::memory_order_relaxed),
                            aecActive_.load(std::memory_order_relaxed));

    std::lock_guard<std::mutex> lk(resultMutex_);
    result_ = r;
}

PreflightResult PreflightChecker::evaluate(const float* micSignal, int numMicSamples,
                                           const float* chirpRef, int chirpSamples,
                                           double sampleRate,
                                           float netRttMs, float netJitterMs,
                                           bool aecActive) {
    juce::ignoreUnused(sampleRate);
    PreflightResult res;
    res.rttMs = netRttMs;
    res.jitterMs = netJitterMs;

    if (micSignal == nullptr || numMicSamples <= 0 || chirpRef == nullptr || chirpSamples <= 0) {
        res.leakStatus = LeakStatus::Untested;
        res.summary = juce::String::fromUTF8("ข้อมูลเสียงไม่สมบูรณ์");
        return res;
    }

    // 1. Microphone level check
    double sumSq = 0.0;
    float peak = 0.0f;
    for (int i = 0; i < numMicSamples; ++i) {
        const float a = std::abs(micSignal[i]);
        sumSq += double(a) * double(a);
        if (a > peak) peak = a;
    }
    const double rms = std::sqrt(sumSq / double(numMicSamples));
    res.micRmsDb = (rms > 1e-6) ? float(20.0 * std::log10(rms)) : -100.0f;
    res.micPeakDb = (peak > 1e-6f) ? float(20.0 * std::log10(peak)) : -100.0f;
    res.micDetected = (peak > 0.0005f); // > -66 dBFS

    // 2. Chirp reference energy
    double chirpEnergy = 0.0;
    for (int i = 0; i < chirpSamples; ++i) {
        chirpEnergy += double(chirpRef[i]) * double(chirpRef[i]);
    }
    chirpEnergy = std::max(chirpEnergy, 1e-12);

    // 3. Normalized cross-correlation across search lags
    // Acoustic lag search window: 0 ms to 180 ms (up to numMicSamples - chirpSamples)
    const int maxLag = std::max(0, numMicSamples - chirpSamples);
    double maxCorr = 0.0;
    int bestLag = 0;
    double bestWindowEnergy = 0.0;

    for (int lag = 0; lag <= maxLag; ++lag) {
        double dot = 0.0;
        double winEnergy = 0.0;
        for (int i = 0; i < chirpSamples; ++i) {
            const double m = double(micSignal[lag + i]);
            const double c = double(chirpRef[i]);
            dot += m * c;
            winEnergy += m * m;
        }

        const double norm = std::sqrt(winEnergy * chirpEnergy);
        const double corr = (norm > 1e-10) ? (std::abs(dot) / norm) : 0.0;

        if (corr > maxCorr) {
            maxCorr = corr;
            bestLag = lag;
            bestWindowEnergy = winEnergy;
        }
    }

    juce::ignoreUnused(bestLag);
    res.correlationPeak = float(maxCorr);
    const double leakRatio = bestWindowEnergy / chirpEnergy;
    res.leakEnergyDb = (leakRatio > 1e-8) ? float(10.0 * std::log10(leakRatio)) : -100.0f;

    // 4. Acoustic leak classification
    // Strong speaker leak: cross-correlation peak >= 0.20 and energy > -35 dB
    // Slight open-back headphone leak: cross-correlation peak >= 0.08 and energy > -45 dB
    // Closed headphones / zero leak: cross-correlation peak < 0.08 or energy <= -45 dB
    if (res.correlationPeak >= 0.20f && res.leakEnergyDb > -35.0f) {
        res.leakStatus = LeakStatus::SpeakerLeakDetected;
    } else if (res.correlationPeak >= 0.08f && res.leakEnergyDb > -45.0f) {
        res.leakStatus = LeakStatus::HeadphonesOpen;
    } else {
        res.leakStatus = LeakStatus::HeadphonesClean;
    }

    // 5. Synthesize advice and readiness
    std::vector<juce::String> issues;
    std::vector<juce::String> goods;

    if (!res.micDetected) {
        issues.push_back(juce::String::fromUTF8("ไมโครโฟนเงียบเกินไป กรุณาตรวจสอบว่าเปิดไมค์หรือยัง"));
    } else {
        goods.push_back(juce::String::fromUTF8("ไมโครโฟนรับเสียงได้ปกติ"));
    }

    if (res.leakStatus == LeakStatus::SpeakerLeakDetected) {
        if (aecActive) {
            goods.push_back(juce::String::fromUTF8("ตรวจพบเสียงลำโพง (ระบบตัดเสียงสะท้อน AEC กำลังทำงาน ✓)"));
        } else {
            issues.push_back(juce::String::fromUTF8("ตรวจพบเสียงลำโพงรั่วเข้าไมค์ แนะนำให้ใส่หูฟัง หรือเปิดโหมดลำโพง (AEC)"));
        }
    } else if (res.leakStatus == LeakStatus::HeadphonesOpen) {
        goods.push_back(juce::String::fromUTF8("หูฟังระดับเสียงปลอดภัย ✓"));
    } else {
        goods.push_back(juce::String::fromUTF8("ใช้หูฟัง ไม่มีเสียงสะท้อน ✓"));
    }

    if (netRttMs > 0.0f) {
        if (netRttMs > 120.0f || netJitterMs > 30.0f) {
            issues.push_back(juce::String::fromUTF8("เน็ตมีความหน่วงหรือกระตุก (RTT ") + juce::String(juce::roundToInt(netRttMs)) + juce::String::fromUTF8(" ms) แนะนำต่อสาย LAN"));
        } else {
            goods.push_back(juce::String::fromUTF8("เน็ตเร็ว เสถียรดีมาก (RTT ") + juce::String(juce::roundToInt(netRttMs)) + juce::String::fromUTF8(" ms)"));
        }
    }

    if (issues.empty()) {
        res.isReady = true;
        res.summary = juce::String::fromUTF8("พร้อมร้อง ✓");
        res.advice = goods.empty() ? juce::String::fromUTF8("ระบบเสียงพร้อมใช้งาน") : juce::StringArray(goods.data(), int(goods.size())).joinIntoString(" · ");
    } else {
        res.isReady = false;
        res.summary = juce::String::fromUTF8("ต้องปรับตั้งค่าก่อนร้อง");
        res.advice = juce::StringArray(issues.data(), int(issues.size())).joinIntoString(" · ");
    }

    return res;
}

} // namespace hearaside::karaoke
