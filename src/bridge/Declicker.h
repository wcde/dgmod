#pragma once

// Click protection of the bridge. Every place where the stream would jump (playback start after buffering, an
// underrun, a re-sync that drops frames, a gap in the captured source, the bridge stopping) is turned into a short
// raised-cosine transition instead of a step:
//  * the stream is continued past its last frame by linear prediction (Burg's method on its last ~20 ms, as in packet
//    loss concealment): a tone keeps its phase and slope, so the continuation joins without a kink, and it is bounded
//    by the level just played;
//  * that continuation is crossfaded into whatever follows: the new position after a jump, or silence (a fade-out).
// A fade-in from silence is the same operation with a silent past. The splicer runs at the source rate, before the
// oversampling filter, which then band-limits the transition. Outside transitions frames pass bit-exactly.

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <numbers>
#include <vector>

namespace dgmod::bridge {

inline constexpr double kDeclickMs = 5.0;  // length of one transition
// Captured samples outside +-kInputMax (+18 dBFS) are clamped, non-finite ones replaced by silence.
inline constexpr float kInputMax = 8.0f;

// Replaces non-finite samples by 0 and clamps the rest to +-limit. Returns the number of samples changed.
inline uint32_t SanitizeSamples(float* x, size_t n, float limit) {
    uint32_t bad = 0;
    for (size_t i = 0; i < n; ++i) {
        const float v = x[i];
        if (!(std::abs(v) <= limit)) {  // also true for NaN
            x[i] = std::isfinite(v) ? std::copysign(limit, v) : 0.0f;
            ++bad;
        }
    }
    return bad;
}

// Burg's method: prediction coefficients a[1..order] (a[0] = 1, unused orders 0) of x[0..n), so that
// x[k] ~ -sum(a[i] * x[k - i]). `f` and `b` are scratch buffers of n values. Returns false for a silent input.
inline bool BurgCoefficients(const double* x, uint32_t n, uint32_t order, double* a, double* f, double* b) {
    std::fill(a, a + order + 1, 0.0);
    a[0] = 1.0;
    if (n < 2 * order + 2) return false;
    double energy = 0;
    for (uint32_t i = 0; i < n; ++i) energy += x[i] * x[i];
    if (energy < 1e-18 * n) return false;
    std::copy(x, x + n, f);
    std::copy(x, x + n, b);
    const uint32_t last = n - 1;
    // The error energies are summed afresh at every order: the usual recursive update loses its precision at high
    // orders on clean signals and then yields reflection coefficients beyond 1 (an exploding predictor). No early stop
    // and no regularization: both measurably worsen the continuation of clean tones.
    const double floor = 2.0 * energy * 1e-24;
    for (uint32_t k = 0; k < order; ++k) {
        double num = 0, den = 0;
        for (uint32_t i = 0; i + k + 1 <= last; ++i) {
            num += f[i + k + 1] * b[i];
            den += f[i + k + 1] * f[i + k + 1] + b[i] * b[i];
        }
        if (den <= floor) break;
        const double mu = -2.0 * num / den;
        for (uint32_t i = 0; i <= (k + 1) / 2; ++i) {
            const double t1 = a[i] + mu * a[k + 1 - i];
            const double t2 = a[k + 1 - i] + mu * a[i];
            a[i] = t1;
            a[k + 1 - i] = t2;
        }
        for (uint32_t i = 0; i + k + 1 <= last; ++i) {
            const double t1 = f[i + k + 1] + mu * b[i];
            const double t2 = b[i] + mu * f[i + k + 1];
            f[i + k + 1] = t1;
            b[i] = t2;
        }
    }
    return true;
}

class Splicer {
public:
    static constexpr uint32_t kOrder = 32;

    // `historyFrames` analysed for the continuation (about 20 ms is plenty); not real-time safe.
    void Configure(uint32_t channels, uint32_t fadeFrames, uint32_t historyFrames = 1024) {
        channels_ = std::max<uint32_t>(channels, 1);
        fade_ = std::max<uint32_t>(fadeFrames, 1);
        ring_ = std::max({historyFrames, 4 * kOrder, fade_ + 1});
        history_.assign(size_t(ring_) * channels_, 0.0f);
        continuation_.assign(size_t(fade_) * channels_, 0.0f);
        x_.assign(size_t(ring_) + fade_, 0.0);
        f_.assign(ring_, 0.0);
        b_.assign(ring_, 0.0);
        window_.resize(fade_);
        for (uint32_t j = 0; j < fade_; ++j)
            window_[j] = static_cast<float>(0.5 - 0.5 * std::cos(std::numbers::pi * double(j + 1) / double(fade_ + 1)));
        Reset();
    }

    // Silent past, no transition in progress.
    void Reset() {
        std::fill(history_.begin(), history_.end(), 0.0f);
        head_ = 0;
        pos_ = fade_;
    }

    [[nodiscard]] uint32_t FadeFrames() const { return fade_; }
    [[nodiscard]] bool Transitioning() const { return pos_ < fade_; }

    // The next frame starts a transition from the predicted continuation of the frames passed so far. Starting one
    // during another continues the partly crossfaded frames, so transitions chain smoothly. Real-time safe.
    void Splice() {
        const size_t ch = channels_;
        const uint32_t n = ring_;
        for (size_t c = 0; c < ch; ++c) {
            double peak = 0;
            for (uint32_t i = 0; i < n; ++i) {  // chronological: head_ is the oldest frame
                const uint32_t k = head_ + i < n ? head_ + i : head_ + i - n;
                x_[i] = history_[size_t(k) * ch + c];
                peak = std::max(peak, std::abs(x_[i]));
            }
            // A high-order predictor in direct form can come out unstable through rounding (clustered poles near the
            // unit circle, typically on a clean test tone), so every continuation is checked and the order lowered
            // until one is plausible; the last resort is the time-reversed past (continuous, never louder).
            bool done = false;
            if (peak > 1e-9) {
                const double roughness = Roughness(x_.data() + (n - fade_), fade_);
                for (uint32_t order = kOrder; order >= 4 && !done; order /= 2) {
                    double a[kOrder + 1];
                    if (!BurgCoefficients(x_.data(), n, order, a, f_.data(), b_.data())) break;
                    bool bounded = true;
                    for (uint32_t j = 0; j < fade_ && bounded; ++j) {
                        double y = 0;
                        for (uint32_t i = 1; i <= order; ++i) y -= a[i] * x_[size_t(n) + j - i];
                        bounded = std::abs(y) <= peak * 1.05;
                        x_[size_t(n) + j] = y;
                    }
                    done = bounded && Roughness(x_.data() + n, fade_) <= 2.0 * roughness + 1e-12;
                }
            }
            for (uint32_t j = 0; j < fade_; ++j) {
                double y = 0;
                if (done) y = std::clamp(x_[size_t(n) + j], -peak, peak);
                else if (peak > 1e-9) y = x_[n - 2 - j];  // even reflection around the last frame (fade_ < ring_)
                continuation_[size_t(j) * ch + c] = static_cast<float>(y);
            }
        }
        pos_ = 0;
    }

    // Passes `frames` interleaved frames in place: crossfaded from the continuation while a transition runs, and
    // remembered.
    void Process(float* x, uint32_t frames) {
        const size_t ch = channels_;
        for (uint32_t i = 0; i < frames && pos_ < fade_; ++i, ++pos_) {
            float* f = x + size_t(i) * ch;
            const float w = window_[pos_];
            const float* m = continuation_.data() + size_t(pos_) * ch;
            for (size_t c = 0; c < ch; ++c) f[c] = m[c] + w * (f[c] - m[c]);
        }
        // Only the last ring_ frames matter for the history.
        const uint32_t keep = std::min(frames, ring_);
        for (uint32_t i = frames - keep; i < frames; ++i) {
            std::copy_n(x + size_t(i) * ch, ch, history_.data() + size_t(head_) * ch);
            head_ = head_ + 1 == ring_ ? 0 : head_ + 1;
        }
    }

    // Process() of `frames` silent frames without a buffer. Only outside a transition (which would not be silent).
    void PassSilence(uint32_t frames) {
        const size_t ch = channels_;
        const uint32_t keep = std::min(frames, ring_);
        for (uint32_t i = 0; i < keep; ++i) {
            std::fill_n(history_.data() + size_t(head_) * ch, ch, 0.0f);
            head_ = head_ + 1 == ring_ ? 0 : head_ + 1;
        }
    }

private:
    // Mean squared second difference relative to the mean square: how much high-frequency content a stretch holds.
    static double Roughness(const double* x, uint32_t n) {
        double d2 = 0, e = 1e-30;
        for (uint32_t i = 2; i < n; ++i) {
            const double d = x[i] - 2.0 * x[i - 1] + x[i - 2];
            d2 += d * d;
        }
        for (uint32_t i = 0; i < n; ++i) e += x[i] * x[i];
        return d2 / e;
    }

    uint32_t channels_ = 1;
    uint32_t fade_ = 1;
    uint32_t ring_ = 4 * kOrder;
    std::vector<float> history_;       // ring_ frames, interleaved; head_ = next write position (the oldest frame)
    std::vector<float> continuation_;  // fade_ frames predicted at Splice()
    std::vector<float> window_;        // gain of the new signal at each transition step
    std::vector<double> x_, f_, b_;    // Splice() scratch: one channel's history plus its continuation; Burg buffers
    uint32_t head_ = 0;
    uint32_t pos_ = 0;
};

}  // namespace dgmod::bridge
