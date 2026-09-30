#pragma once

// Analog "warmth" of the bridge, in the way a tube or tape stage colours the sound: mostly through its frequency
// response (tape: the low-frequency head bump and a soft top; tube: fuller low mids, softened presence) and only
// slightly through harmonics, which grow with the level like in a real stage (clean at normal levels, a little on loud
// peaks). Waveshaping alone does not sound warm on a full mix: at audible levels its intermodulation is heard as rasp.
//
// The equalizer runs at the source rate before the oversampling filter, scaled so no frequency rises above its input
// peak level (the limiter never has to catch it); the harmonics at the oversampled output rate, where they do not alias.

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <string_view>
#include <vector>

namespace dgmod::dsp {

// Tube: fuller low mids (shelf below 200 Hz), softened presence (3 kHz) and top, 2nd harmonic. Tape: head bump
// (75 Hz) with its dip above, soft top (shelf from 10 kHz), 3rd harmonic.
enum class WarmthType : uint32_t { Tube = 0, Tape = 1 };

constexpr std::wstring_view WarmthName(WarmthType t) { return t == WarmthType::Tape ? L"tape" : L"tube"; }

struct ToneSettings {
    bool warmth = false;
    WarmthType warmthType = WarmthType::Tape;
    double warmthAmount = 0.5;  // 0..1: depth of the response and the harmonic level

    void Validate() {
        if (static_cast<uint32_t>(warmthType) > 1) warmthType = WarmthType::Tape;
        warmthAmount = warmthAmount == warmthAmount ? std::clamp(warmthAmount, 0.0, 1.0) : 0.5;
    }
    friend bool operator==(const ToneSettings&, const ToneSettings&) = default;
};

// Second-order IIR section (transposed direct form II, double precision); RBJ cookbook designs.
struct Biquad {
    double b0 = 1, b1 = 0, b2 = 0, a1 = 0, a2 = 0;

    static Biquad Lowpass(double hz, double rate, double q);
    static Biquad Highpass(double hz, double rate, double q);
    static Biquad Peaking(double hz, double rate, double q, double db);
    static Biquad LowShelf(double hz, double rate, double q, double db);
    static Biquad HighShelf(double hz, double rate, double q, double db);
    static Biquad Allpass(double hz, double rate, double q);
    // |H| at `hz`.
    [[nodiscard]] double Magnitude(double hz, double rate) const;
};

struct BiquadState {
    double z1 = 0, z2 = 0;
    double Run(const Biquad& f, double x) {
        const double y = f.b0 * x + z1;
        z1 = f.b1 * x - f.a1 * y + z2;
        z2 = f.b2 * x - f.a2 * y;
        return y;
    }
    void Flush() {
        if (std::abs(z1) < 1e-30) z1 = 0;
        if (std::abs(z2) < 1e-30) z2 = 0;
    }
};

// The warmth equalizer: three sections and the gain that keeps its highest point at 0 dB.
struct WarmthResponse {
    static constexpr int kSections = 3;
    Biquad s[kSections];
    double gain = 1.0;
    [[nodiscard]] double Magnitude(double hz, double rate) const {
        double m = gain;
        for (const auto& f : s) m *= f.Magnitude(hz, rate);
        return m;
    }
};
WarmthResponse WarmthEqDesign(WarmthType type, double amount, double rate);

// Harmonic curve of a stage: y = x + k2 c^2 - k3 c^3, c = x clamped to +-1. A full-scale sine gets a 2nd harmonic of
// k2 / 2 and a 3rd of k3 / 4; a sine of amplitude A gets k2 A / 2 and k3 A^2 / 4 (6 or 12 dB less per 6 dB quieter).
struct WarmthCurve {
    double k2 = 0, k3 = 0;
};
WarmthCurve WarmthCoefficients(WarmthType type, double amount);
inline constexpr double kWarmthFullScaleHarmonic = 0.005;  // main harmonic of a full-scale sine at amount 1 (0.5 %)

// Source-rate part: the equalizer, faded in and out over one block (its phase shifts are small).
class WarmthEq {
public:
    void Configure(uint32_t channels, uint32_t rate);
    void Set(bool on, WarmthType type, double amount);
    void Process(float* x, uint32_t frames);
    [[nodiscard]] bool Active() const { return mix_ != 0 || targetMix_ != 0; }

private:
    uint32_t channels_ = 0, rate_ = 0;
    WarmthResponse eq_{};
    double mix_ = 0, targetMix_ = 0;
    std::vector<BiquadState> state_;  // channels x sections
};

// Output-rate part: the harmonics, added to the untouched signal (only the added part is DC-blocked). Amount changes
// ramp over one block; switched off, the stage costs nothing.
class Warmth {
public:
    void Configure(uint32_t channels, uint32_t rate);
    void Set(bool on, WarmthType type, double amount);
    // In place, `channels` interleaved.
    void Process(float* x, uint32_t frames);
    [[nodiscard]] bool Active() const { return mix_ != 0 || targetMix_ != 0; }

private:
    uint32_t channels_ = 0;
    double dcPole_ = 0;
    WarmthCurve cur_{}, target_{};
    double mix_ = 0, targetMix_ = 0;
    std::vector<double> prev_, dc_;  // per channel: previous added value and DC-blocker output
};

}  // namespace dgmod::dsp
