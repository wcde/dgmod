#include "dsp/Tone.h"

#include <complex>
#include <numbers>

namespace dgmod::dsp {

namespace {

// DC blocker of the added harmonics (the c^2 term has a mean of k2 A^2 / 2).
constexpr double kWarmthDcHz = 5.0;

struct Rbj {
    double c, alpha;
    Rbj(double hz, double rate, double q) {
        const double w = 2.0 * std::numbers::pi * hz / rate;
        c = std::cos(w);
        alpha = std::sin(w) / (2.0 * q);
    }
};

Biquad Normalize(double b0, double b1, double b2, double a0, double a1, double a2) {
    Biquad f;
    f.b0 = b0 / a0;
    f.b1 = b1 / a0;
    f.b2 = b2 / a0;
    f.a1 = a1 / a0;
    f.a2 = a2 / a0;
    return f;
}

}  // namespace

Biquad Biquad::Lowpass(double hz, double rate, double q) {
    const Rbj r(hz, rate, q);
    return Normalize((1.0 - r.c) * 0.5, 1.0 - r.c, (1.0 - r.c) * 0.5, 1.0 + r.alpha, -2.0 * r.c, 1.0 - r.alpha);
}

Biquad Biquad::Highpass(double hz, double rate, double q) {
    const Rbj r(hz, rate, q);
    return Normalize((1.0 + r.c) * 0.5, -(1.0 + r.c), (1.0 + r.c) * 0.5, 1.0 + r.alpha, -2.0 * r.c, 1.0 - r.alpha);
}

Biquad Biquad::Peaking(double hz, double rate, double q, double db) {
    const Rbj r(hz, rate, q);
    const double a = std::pow(10.0, db / 40.0);
    return Normalize(1.0 + r.alpha * a, -2.0 * r.c, 1.0 - r.alpha * a, 1.0 + r.alpha / a, -2.0 * r.c, 1.0 - r.alpha / a);
}

Biquad Biquad::LowShelf(double hz, double rate, double q, double db) {
    const Rbj r(hz, rate, q);
    const double a = std::pow(10.0, db / 40.0), s = 2.0 * std::sqrt(a) * r.alpha;
    return Normalize(a * ((a + 1) - (a - 1) * r.c + s), 2 * a * ((a - 1) - (a + 1) * r.c), a * ((a + 1) - (a - 1) * r.c - s),
                     (a + 1) + (a - 1) * r.c + s, -2 * ((a - 1) + (a + 1) * r.c), (a + 1) + (a - 1) * r.c - s);
}

Biquad Biquad::HighShelf(double hz, double rate, double q, double db) {
    const Rbj r(hz, rate, q);
    const double a = std::pow(10.0, db / 40.0), s = 2.0 * std::sqrt(a) * r.alpha;
    return Normalize(a * ((a + 1) + (a - 1) * r.c + s), -2 * a * ((a - 1) + (a + 1) * r.c), a * ((a + 1) + (a - 1) * r.c - s),
                     (a + 1) - (a - 1) * r.c + s, 2 * ((a - 1) - (a + 1) * r.c), (a + 1) - (a - 1) * r.c - s);
}

Biquad Biquad::Allpass(double hz, double rate, double q) {
    const Rbj r(hz, rate, q);
    return Normalize(1.0 - r.alpha, -2.0 * r.c, 1.0 + r.alpha, 1.0 + r.alpha, -2.0 * r.c, 1.0 - r.alpha);
}

double Biquad::Magnitude(double hz, double rate) const {
    const std::complex<double> z1 = std::polar(1.0, -2.0 * std::numbers::pi * hz / rate), z2 = z1 * z1;
    return std::abs((b0 + b1 * z1 + b2 * z2) / (1.0 + a1 * z1 + a2 * z2));
}

WarmthResponse WarmthEqDesign(WarmthType type, double amount, double rate) {
    const double a = std::clamp(amount, 0.0, 1.0);
    WarmthResponse r;
    if (type == WarmthType::Tape) {
        r.s[0] = Biquad::Peaking(75.0, rate, 0.9, 4.0 * a);       // head bump
        r.s[1] = Biquad::Peaking(180.0, rate, 1.4, -1.2 * a);     // the dip above it
        r.s[2] = Biquad::HighShelf(10000.0, rate, 0.6, -4.0 * a); // soft top
    } else {
        r.s[0] = Biquad::LowShelf(200.0, rate, 0.6, 3.0 * a);     // fuller low mids
        r.s[1] = Biquad::Peaking(3000.0, rate, 0.8, -2.5 * a);    // softened presence
        r.s[2] = Biquad::HighShelf(13000.0, rate, 0.6, -2.0 * a);
    }
    // Scale the highest point to 0 dB (log grid 10 Hz .. Nyquist), so peaks never rise.
    double peak = 1.0;
    for (int i = 0; i <= 400; ++i) {
        const double hz = 10.0 * std::pow(rate * 0.5 / 10.0, i / 400.0);
        peak = std::max(peak, r.Magnitude(std::min(hz, rate * 0.499), rate));
    }
    r.gain = 1.0 / peak;
    return r;
}

WarmthCurve WarmthCoefficients(WarmthType type, double amount) {
    const double g = kWarmthFullScaleHarmonic * std::clamp(amount, 0.0, 1.0);
    // Tube: 2nd harmonic g, 3rd g / 4 (-12 dB). Tape: 3rd harmonic g, 2nd g / 10 (-20 dB).
    return type == WarmthType::Tape ? WarmthCurve{0.2 * g, 4.0 * g} : WarmthCurve{2.0 * g, g};
}

// ---- Equalizer (source rate)

void WarmthEq::Configure(uint32_t channels, uint32_t rate) {
    channels_ = channels;
    rate_ = rate;
    state_.assign(size_t(channels) * WarmthResponse::kSections, BiquadState{});
    mix_ = targetMix_ = 0;
    eq_ = {};
}

void WarmthEq::Set(bool on, WarmthType type, double amount) {
    eq_ = WarmthEqDesign(type, amount, rate_);
    if (!Active()) std::fill(state_.begin(), state_.end(), BiquadState{});  // starting from rest
    targetMix_ = on ? 1.0 : 0.0;
}

void WarmthEq::Process(float* x, uint32_t frames) {
    if (!Active() || frames == 0) return;
    const double dm = (targetMix_ - mix_) / frames;
    double m = mix_;
    for (uint32_t i = 0; i < frames; ++i) {
        m += dm;
        float* f = x + size_t(i) * channels_;
        for (uint32_t c = 0; c < channels_; ++c) {
            BiquadState* st = state_.data() + size_t(c) * WarmthResponse::kSections;
            const double in = f[c];
            double y = in;
            for (int k = 0; k < WarmthResponse::kSections; ++k) y = st[k].Run(eq_.s[k], y);
            y *= eq_.gain;
            f[c] = static_cast<float>(in + m * (y - in));
        }
    }
    mix_ = targetMix_;
    for (auto& s : state_) s.Flush();
}

// ---- Harmonics (output rate)

void Warmth::Configure(uint32_t channels, uint32_t rate) {
    channels_ = channels;
    dcPole_ = std::exp(-2.0 * std::numbers::pi * kWarmthDcHz / rate);
    prev_.assign(channels, 0.0);
    dc_.assign(channels, 0.0);
    mix_ = targetMix_ = 0;
    cur_ = target_ = {};
}

void Warmth::Set(bool on, WarmthType type, double amount) {
    target_ = WarmthCoefficients(type, amount);
    if (!Active()) cur_ = target_;  // starting from bypass: the curve itself needs no ramp
    targetMix_ = on ? 1.0 : 0.0;
}

void Warmth::Process(float* x, uint32_t frames) {
    if (!Active() || frames == 0) return;
    const double inv = 1.0 / frames;
    const double dm = (targetMix_ - mix_) * inv, d2 = (target_.k2 - cur_.k2) * inv, d3 = (target_.k3 - cur_.k3) * inv;
    double m = mix_, k2 = cur_.k2, k3 = cur_.k3;
    for (uint32_t i = 0; i < frames; ++i) {
        m += dm;
        k2 += d2;
        k3 += d3;
        float* f = x + size_t(i) * channels_;
        for (uint32_t c = 0; c < channels_; ++c) {
            const double in = f[c];
            const double v = std::clamp(in, -1.0, 1.0);
            const double d = (k2 - k3 * v) * v * v;
            const double h = d - prev_[c] + dcPole_ * dc_[c];
            prev_[c] = d;
            dc_[c] = h;
            f[c] = static_cast<float>(in + m * h);
        }
    }
    mix_ = targetMix_;
    cur_ = target_;
    if (mix_ == 0) {  // bypassed: the next start begins from rest
        std::fill(prev_.begin(), prev_.end(), 0.0);
        std::fill(dc_.begin(), dc_.end(), 0.0);
    } else {
        for (double& v : dc_)
            if (std::abs(v) < 1e-30) v = 0.0;
    }
}

}  // namespace dgmod::dsp
