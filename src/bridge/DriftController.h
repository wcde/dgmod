#pragma once

#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstdint>

namespace dgmod::bridge {

// Turns packetized arrivals into a continuous frame count for the drift controller.
//
// Each capture packet carries a device timestamp of its first frame, which advances regularly with the source clock,
// while the time the capture thread actually gets it scatters widely (engine delivery, polling; on real systems the
// spread is bimodal, from under a millisecond to a whole engine period). The clock places each packet on the device
// timeline (shifted by a fixed anchor, the first observed delay) and draws the continuous line through the middle of
// each packet step: neither the staircase nor the delivery scatter reaches the controller. What remains is a constant
// offset against the real FIFO, which LevelCalibrator removes. The averaged delivery delay is kept for diagnostics.
// Single writer (capture thread), any reader (render thread): seqlock.
class ArrivalClock {
public:
    void Configure(double rate, double delaySmoothingSec = 20.0) {
        rate_ = rate;
        smoothing_ = delaySmoothingSec;
        delay_ = 0;
        anchor_ = 0;
        primed_ = false;
        seq_.store(0);
    }

    // Capture thread: `framesAfter` = total frames written including this packet.
    void Publish(uint64_t framesAfter, uint32_t packetFrames, double firstFrameTime, double receivedTime) {
        const double duration = double(packetFrames) / rate_;
        const double observed = receivedTime - (firstFrameTime + duration);
        if (!primed_ || std::abs(observed - delay_) > 0.1) {  // first packet or a timeline jump
            delay_ = anchor_ = observed;
            primed_ = true;
        } else {
            delay_ += (observed - delay_) * (1.0 - std::exp(-std::max(duration, 1e-4) / smoothing_));
        }
        delayOut_.store(delay_, std::memory_order_relaxed);
        StoreMin(observedMin_, observed);
        StoreMax(observedMax_, observed);
        const double available = firstFrameTime + duration + anchor_;
        const uint32_t s = seq_.load(std::memory_order_relaxed);
        seq_.store(s + 1, std::memory_order_relaxed);
        std::atomic_thread_fence(std::memory_order_release);
        after_.store(framesAfter, std::memory_order_relaxed);
        half_.store(0.5 * packetFrames, std::memory_order_relaxed);
        time_.store(available, std::memory_order_relaxed);
        seq_.store(s + 2, std::memory_order_release);
    }

    // Estimated frames delivered by `now` (continuous); `age` = seconds since the latest packet became available.
    bool Estimate(double now, double& frames, double& age) const {
        for (int attempt = 0; attempt < 8; ++attempt) {
            const uint32_t s = seq_.load(std::memory_order_acquire);
            if (s & 1u) continue;
            const uint64_t after = after_.load(std::memory_order_relaxed);
            const double half = half_.load(std::memory_order_relaxed);
            const double t = time_.load(std::memory_order_relaxed);
            std::atomic_thread_fence(std::memory_order_acquire);
            if (seq_.load(std::memory_order_relaxed) != s) continue;
            if (s == 0) return false;
            age = now - t;
            frames = double(after) - half + rate_ * age;
            return true;
        }
        return false;
    }

    [[nodiscard]] double DelaySec() const { return delayOut_.load(std::memory_order_relaxed); }
    // Range of the observed delivery delays since the previous call (diagnostics); returns false if none.
    bool TakeObservedRange(double& lo, double& hi) {
        lo = observedMin_.exchange(1e9);
        hi = observedMax_.exchange(-1e9);
        return lo <= hi;
    }

private:
    double rate_ = 48000.0;
    double smoothing_ = 20.0;
    static void StoreMin(std::atomic<double>& a, double v) {
        double cur = a.load(std::memory_order_relaxed);
        while (v < cur && !a.compare_exchange_weak(cur, v, std::memory_order_relaxed)) {
        }
    }
    static void StoreMax(std::atomic<double>& a, double v) {
        double cur = a.load(std::memory_order_relaxed);
        while (v > cur && !a.compare_exchange_weak(cur, v, std::memory_order_relaxed)) {
        }
    }

    double delay_ = 0;   // capture thread only
    double anchor_ = 0;  // capture thread only
    std::atomic<double> delayOut_{0}, observedMin_{1e9}, observedMax_{-1e9};
    bool primed_ = false;
    std::atomic<uint32_t> seq_{0};
    std::atomic<uint64_t> after_{0};
    std::atomic<double> half_{0};
    std::atomic<double> time_{0};
};

// Removes the constant offset between the continuous level estimate (smooth, but only as good as the timestamps'
// relation to the real delivery) and the real FIFO level (exact on average, but a staircase): the controller gets the
// smooth shape with the true mean. Learns quickly at first (running mean), then follows slowly; a jump in the
// difference (timeline glitch) starts learning again.
class LevelCalibrator {
public:
    void Configure(double rate, double tauSec = 120.0) {
        jumpFrames_ = 0.04 * rate;
        tau_ = tauSec;
        Reset();
    }
    void Reset() {
        bias_ = 0;
        elapsed_ = 0;
    }

    double Correct(double estimate, double actual, double dt) {
        const double diff = estimate - actual;
        if (elapsed_ > 0 && std::abs(diff - bias_) > jumpFrames_) elapsed_ = 0;
        elapsed_ += dt;
        const double a = std::max(dt / elapsed_, 1.0 - std::exp(-dt / tau_));
        bias_ += (diff - bias_) * a;
        return estimate - bias_;
    }

    [[nodiscard]] double BiasFrames() const { return bias_; }

private:
    double jumpFrames_ = 1920;
    double tau_ = 120.0;
    double bias_ = 0;
    double elapsed_ = 0;
};

// Keeps the FIFO between the capture clock (virtual device) and the render clock (DAC) at its target level by
// trimming the resampler ratio. PI controller on the low-pass filtered FIFO level; output in ppm, positive = consume
// input faster. The level is sampled once per render period, right after the resampler read.
//
// Two gain sets: a fast one acquires the clock drift within seconds; once the level has stayed within `lockBandMs` of
// the target for `lockSec`, a gentle one takes over, so the trim follows the (practically constant) crystal drift and
// not the capture timing noise. The switch is bumpless; a large error returns to acquisition.
class DriftController {
public:
    struct Gains {
        double kpPpmPerMs;    // proportional (the FIFO integrates 0.001 ms/s per ppm of mismatch)
        double kiPpmPerMsSec; // integral
    };
    struct Params {
        double smoothingSec = 1.0;           // time constant of the level low-pass
        Gains acquire{400.0, 40.0};          // natural frequency 0.2 rad/s, critically damped
        Gains track{60.0, 0.9};              // natural frequency 0.03 rad/s, critically damped
        double lockBandMs = 0.5;
        double lockSec = 10.0;
        double unlockMs = 3.0;
        double limitPpm = 1000.0;            // clock tolerance we are willing to follow
    };

    DriftController() = default;
    explicit DriftController(Params p) : p_(p) {}

    void Reset(double targetFrames, double inRate) {
        target_ = targetFrames;
        framesPerMs_ = inRate / 1000.0;
        smoothed_ = targetFrames;
        integral_ = 0;
        ppm_ = 0;
        primed_ = false;
        locked_ = false;
        inBand_ = 0;
    }

    // Restart the level smoothing after a discontinuity (FIFO trimmed); the integral (clock drift) is kept.
    void RestartLevel() { primed_ = false; }

    // `level`: FIFO frames after the read; `dt`: seconds since the previous update.
    double Update(double level, double dt) {
        if (!primed_) {
            smoothed_ = level;
            primed_ = true;
        }
        const double a = 1.0 - std::exp(-dt / p_.smoothingSec);
        smoothed_ += (level - smoothed_) * a;
        const double errorMs = (smoothed_ - target_) / framesPerMs_;

        inBand_ = std::abs(errorMs) < p_.lockBandMs ? inBand_ + dt : 0.0;
        if (!locked_ && inBand_ >= p_.lockSec) SwitchGains(true, errorMs);
        else if (locked_ && std::abs(errorMs) > p_.unlockMs) SwitchGains(false, errorMs);

        const Gains& g = locked_ ? p_.track : p_.acquire;
        integral_ = std::clamp(integral_ + errorMs * dt * g.kiPpmPerMsSec, -p_.limitPpm, p_.limitPpm);
        ppm_ = std::clamp(g.kpPpmPerMs * errorMs + integral_, -p_.limitPpm, p_.limitPpm);
        return ppm_;
    }

    [[nodiscard]] double Ppm() const { return ppm_; }
    [[nodiscard]] bool Locked() const { return locked_; }
    [[nodiscard]] double SmoothedLevel() const { return smoothed_; }
    [[nodiscard]] double ErrorMs() const { return framesPerMs_ > 0 ? (smoothed_ - target_) / framesPerMs_ : 0.0; }

private:
    void SwitchGains(bool lock, double errorMs) {
        const Gains& from = locked_ ? p_.track : p_.acquire;
        const Gains& to = lock ? p_.track : p_.acquire;
        integral_ = std::clamp(integral_ + (from.kpPpmPerMs - to.kpPpmPerMs) * errorMs, -p_.limitPpm, p_.limitPpm);
        locked_ = lock;
        inBand_ = 0;
    }

    Params p_{};
    double target_ = 0;
    double framesPerMs_ = 48.0;
    double smoothed_ = 0;
    double integral_ = 0;
    double ppm_ = 0;
    double inBand_ = 0;
    bool primed_ = false;
    bool locked_ = false;
};

}  // namespace dgmod::bridge
