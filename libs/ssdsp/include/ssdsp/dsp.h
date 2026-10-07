// HEARASIDE DSP building blocks. Header-only, allocation free after prepare().
// SPDX-License-Identifier: MIT
#pragma once

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <vector>

namespace ssdsp {

constexpr float kMinusInfDb = -60.0f;   // at or below this a gain is treated as silence

inline float dbToGain(float db) noexcept { return db <= kMinusInfDb ? 0.0f : std::pow(10.0f, db * 0.05f); }
inline float gainToDb(float g) noexcept { return g <= 1.0e-9f ? -200.0f : 20.0f * std::log10(g); }

// ---------------------------------------------------------------------------------------------
// Linear ramp smoother (like juce::SmoothedValue<Linear>), no allocation.
class Smoother {
public:
    void prepare(double sampleRate, double rampMs) noexcept {
        rampLen_ = std::max(1, int(sampleRate * rampMs * 0.001));
        steps_ = 0;
        current_ = target_;
    }
    void setTarget(float t) noexcept {
        if (t == target_) return;
        target_ = t;
        steps_ = rampLen_;
        step_ = (target_ - current_) / float(steps_);
    }
    void snap(float v) noexcept { current_ = target_ = v; steps_ = 0; }
    float next() noexcept {
        if (steps_ > 0) { current_ += step_; if (--steps_ == 0) current_ = target_; }
        return current_;
    }
    bool isSmoothing() const noexcept { return steps_ > 0; }
    float current() const noexcept { return current_; }
    float target() const noexcept { return target_; }

    // Multiplies buf by the ramp. Returns false when the whole block was exactly zero gain.
    bool apply(float* const* ch, int numCh, int n) noexcept {
        if (!isSmoothing()) {
            const float g = current_;
            if (g == 1.0f) return true;
            for (int c = 0; c < numCh; ++c) for (int i = 0; i < n; ++i) ch[c][i] *= g;
            return g != 0.0f;
        }
        for (int i = 0; i < n; ++i) {
            const float g = next();
            for (int c = 0; c < numCh; ++c) ch[c][i] *= g;
        }
        return true;
    }

private:
    float current_ = 0.0f, target_ = 0.0f, step_ = 0.0f;
    int   rampLen_ = 1, steps_ = 0;
};

// ---------------------------------------------------------------------------------------------
// Equal-power pan / balance. pan in [-1, 1]. Centre = unity on both sides.
inline void panGains(float pan, float& gl, float& gr) noexcept {
    pan = std::clamp(pan, -1.0f, 1.0f);
    const float theta = (pan + 1.0f) * 0.78539816339f;   // 0 .. pi/2
    gl = 1.41421356f * std::cos(theta);
    gr = 1.41421356f * std::sin(theta);
    // Keep the centre exactly at unity (cos/sin rounding).
    if (pan == 0.0f) { gl = gr = 1.0f; }
}

// ---------------------------------------------------------------------------------------------
// Peak meter with simple hold/decay for UI (call from audio thread, read the value elsewhere).
inline float blockPeak(const float* x, int n) noexcept {
    float p = 0.0f;
    for (int i = 0; i < n; ++i) p = std::max(p, std::abs(x[i]));
    return p;
}

// ---------------------------------------------------------------------------------------------
// Brick-wall lookahead limiter (stereo). Guarantees |out| <= ceiling.
// Gain = box-filter(release-smoothed(sliding-min over lookahead window of required gain)).
class Limiter {
public:
    void prepare(double sampleRate, double lookaheadMs = 1.0, double releaseMs = 120.0) {
        la_ = std::max(1, int(std::lround(sampleRate * lookaheadMs * 0.001)));
        relCoef_ = float(1.0 - std::exp(-1.0 / (sampleRate * releaseMs * 0.001)));
        delay_[0].assign(size_t(la_), 0.0f);
        delay_[1].assign(size_t(la_), 0.0f);
        box_.assign(size_t(la_), 1.0f);
        dqVal_.assign(size_t(la_ + 2), 1.0f);
        dqIdx_.assign(size_t(la_ + 2), 0);
        reset();
    }
    void reset() noexcept {
        std::fill(delay_[0].begin(), delay_[0].end(), 0.0f);
        std::fill(delay_[1].begin(), delay_[1].end(), 0.0f);
        std::fill(box_.begin(), box_.end(), 1.0f);
        boxSum_ = double(la_);
        pos_ = 0; t_ = 0; head_ = tail_ = 0; release_ = 1.0f; recompute_ = 0;
        gainReduction_ = 1.0f;
    }
    int latency() const noexcept { return la_; }
    void setCeiling(float linear) noexcept { ceiling_ = std::max(1.0e-4f, linear); }
    float lastGain() const noexcept { return gainReduction_; }

    void process(float* l, float* r, int n, bool enabled) noexcept {
        const int cap = la_ + 2;
        float minGain = 1.0f;
        for (int i = 0; i < n; ++i) {
            const float xl = l[i], xr = r[i];
            const float peak = std::max(std::abs(xl), std::abs(xr));
            const float req = (enabled && peak > ceiling_) ? ceiling_ / peak : 1.0f;

            // sliding minimum over the last la_+1 samples (monotonic deque in fixed arrays)
            while (head_ != tail_) {
                const int last = (tail_ + cap - 1) % cap;
                if (dqVal_[size_t(last)] >= req) tail_ = last; else break;
            }
            dqVal_[size_t(tail_)] = req; dqIdx_[size_t(tail_)] = t_; tail_ = (tail_ + 1) % cap;
            while (t_ - dqIdx_[size_t(head_)] > int64_t(la_)) head_ = (head_ + 1) % cap;
            const float held = dqVal_[size_t(head_)];

            release_ = held < release_ ? held : release_ + (held - release_) * relCoef_;

            boxSum_ += double(release_) - double(box_[size_t(pos_)]);
            box_[size_t(pos_)] = release_;
            float g = float(boxSum_ / double(la_));
            g = std::min(g, 1.0f);

            const float dl = delay_[0][size_t(pos_)], dr = delay_[1][size_t(pos_)];
            delay_[0][size_t(pos_)] = xl; delay_[1][size_t(pos_)] = xr;
            pos_ = (pos_ + 1) % la_;
            ++t_;

            float ol = dl * g, orr = dr * g;
            if (enabled) {   // guard against rounding only
                ol = std::clamp(ol, -ceiling_, ceiling_);
                orr = std::clamp(orr, -ceiling_, ceiling_);
            }
            l[i] = ol; r[i] = orr;
            minGain = std::min(minGain, g);
        }
        if (++recompute_ >= 256) {   // avoid floating drift of the running sum
            recompute_ = 0;
            double s = 0; for (float v : box_) s += v; boxSum_ = s;
        }
        gainReduction_ = minGain;
    }

private:
    int la_ = 48;
    float relCoef_ = 0.001f, ceiling_ = 0.891f, release_ = 1.0f, gainReduction_ = 1.0f;
    std::vector<float> delay_[2], box_, dqVal_;
    std::vector<int64_t> dqIdx_;
    double boxSum_ = 0;
    int pos_ = 0, head_ = 0, tail_ = 0, recompute_ = 0;
    int64_t t_ = 0;
};

// ---------------------------------------------------------------------------------------------
// ITU-R BS.1770 loudness (momentary 400 ms, short-term 3 s), stereo.
class LoudnessMeter {
public:
    void prepare(double fs) {
        fs_ = fs;
        // Stage 1: high shelf
        {
            const double f0 = 1681.974450955533, G = 3.999843853973347, Q = 0.7071752369554196;
            const double K = std::tan(M_PI_ * f0 / fs);
            const double Vh = std::pow(10.0, G / 20.0);
            const double Vb = std::pow(Vh, 0.4996667741545416);
            const double a0 = 1.0 + K / Q + K * K;
            s1_ = { (Vh + Vb * K / Q + K * K) / a0, 2.0 * (K * K - Vh) / a0, (Vh - Vb * K / Q + K * K) / a0,
                    2.0 * (K * K - 1.0) / a0, (1.0 - K / Q + K * K) / a0 };
        }
        // Stage 2: RLB high pass
        {
            const double f0 = 38.13547087602444, Q = 0.5003270373238773;
            const double K = std::tan(M_PI_ * f0 / fs);
            const double a0 = 1.0 + K / Q + K * K;
            s2_ = { 1.0, -2.0, 1.0, 2.0 * (K * K - 1.0) / a0, (1.0 - K / Q + K * K) / a0 };
        }
        subLen_ = std::max(1, int(std::lround(fs * 0.1)));
        reset();
    }
    void reset() noexcept {
        for (auto& z : z1_) z = {};
        for (auto& z : z2_) z = {};
        acc_ = 0; accN_ = 0; blocks_ = 0;
        for (double& e : hist_) e = 0;
        momentary_ = shortTerm_ = -200.0f;
    }
    void process(const float* l, const float* r, int n) noexcept {
        const float* ch[2] = { l, r };
        for (int i = 0; i < n; ++i) {
            double sum = 0;
            for (int c = 0; c < 2; ++c) {
                const double y1 = biquad(s1_, z1_[c], double(ch[c][i]));
                const double y2 = biquad(s2_, z2_[c], y1);
                sum += y2 * y2;
            }
            acc_ += sum;
            if (++accN_ >= subLen_) {
                hist_[size_t(blocks_ % kHist)] = acc_ / double(accN_);
                ++blocks_;
                acc_ = 0; accN_ = 0;
                momentary_ = toLufs(average(4));
                shortTerm_ = toLufs(average(30));
            }
        }
    }
    float momentary() const noexcept { return momentary_; }
    float shortTerm() const noexcept { return shortTerm_; }

private:
    struct Coeffs { double b0, b1, b2, a1, a2; };
    struct State { double z1 = 0, z2 = 0; };
    static constexpr double M_PI_ = 3.14159265358979323846;
    static constexpr int kHist = 30;

    static double biquad(const Coeffs& k, State& s, double x) noexcept {   // transposed DF2
        const double y = k.b0 * x + s.z1;
        s.z1 = k.b1 * x - k.a1 * y + s.z2;
        s.z2 = k.b2 * x - k.a2 * y;
        return y;
    }
    double average(int count) const noexcept {
        const int64_t have = std::min<int64_t>(blocks_, count);
        if (have <= 0) return 0;
        double s = 0;
        for (int64_t k = 0; k < have; ++k) s += hist_[size_t((blocks_ - 1 - k) % kHist)];
        return s / double(count);   // partial windows count as silence (as a real meter warming up)
    }
    static float toLufs(double ms) noexcept { return ms <= 1.0e-20 ? -200.0f : float(-0.691 + 10.0 * std::log10(ms)); }

    double fs_ = 48000;
    Coeffs s1_{}, s2_{};
    State z1_[2]{}, z2_[2]{};
    double acc_ = 0; int accN_ = 0, subLen_ = 4800;
    int64_t blocks_ = 0;
    double hist_[kHist]{};
    float momentary_ = -200.0f, shortTerm_ = -200.0f;
};

// ---------------------------------------------------------------------------------------------
// PI controller that keeps a consumer's buffer fill at a target by nudging the resampling
// ratio. Output: relative correction c, use ratio = inRate * (1 + c) / outRate.
class DriftController {
public:
    void reset(double targetFrames) noexcept {
        target_ = std::max(1.0, targetFrames);
        filtered_ = 0; integral_ = 0; correction_ = 0; primed_ = false;
    }
    // fill = frames available to read, dtSec = time since previous call.
    double update(double fill, double dtSec) noexcept {
        const double e = (fill - target_) / target_;
        if (!primed_) { filtered_ = e; primed_ = true; }
        const double a = 1.0 - std::exp(-dtSec / kSmoothSec);
        filtered_ += (e - filtered_) * a;
        integral_ = std::clamp(integral_ + filtered_ * dtSec, -kIntegralLimit, kIntegralLimit);
        correction_ = std::clamp(kKp * filtered_ + kKi * integral_, -kMaxCorrection, kMaxCorrection);
        return correction_;
    }
    // Moves the target without resetting the controller (used by the adaptive buffer).
    void setTarget(double targetFrames) noexcept {
        const double t = std::max(1.0, targetFrames);
        filtered_ *= target_ / t;   // keep the absolute error continuous
        target_ = t;
    }
    double correction() const noexcept { return correction_; }
    double filteredError() const noexcept { return filtered_; }
    double target() const noexcept { return target_; }

    static constexpr double kMaxCorrection = 0.005;   // ±0.5 %
private:
    static constexpr double kSmoothSec = 0.5;
    static constexpr double kKp = 0.002;
    static constexpr double kKi = 0.0004;
    static constexpr double kIntegralLimit = kMaxCorrection / kKi;
    double target_ = 1, filtered_ = 0, integral_ = 0, correction_ = 0;
    bool primed_ = false;
};

} // namespace ssdsp
