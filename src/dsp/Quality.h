#pragma once

#include <algorithm>
#include <array>
#include <cstdint>
#include <string_view>

namespace dgmod::dsp {

enum class QualityPreset : uint32_t { Fast = 0, Balanced = 1, High = 2, Ultra = 3, Custom = 4 };
inline constexpr uint32_t kPresetCount = 5;

// Linear phase: symmetric impulse response (pre- and post-ringing, constant delay). Minimum phase: the same magnitude
// response with all ringing after the main peak and almost no delay (phase shifts near the band edge). Intermediate:
// half of the minimum-phase phase response: pre-ringing about 30 dB weaker than linear phase, a similar delay, and
// twice the filter length (CPU).
enum class FilterPhase : uint32_t { Linear = 0, Minimum = 1, Intermediate = 2 };

// Fraction of the minimum-phase phase response (0 = linear/zero phase, 1 = minimum phase).
constexpr double PhaseFraction(FilterPhase p) {
    return p == FilterPhase::Minimum ? 1.0 : p == FilterPhase::Intermediate ? 0.5 : 0.0;
}

constexpr std::wstring_view PhaseName(FilterPhase p) {
    return p == FilterPhase::Minimum ? L"minimum" : p == FilterPhase::Intermediate ? L"intermediate" : L"linear";
}

// Roll-off of the magnitude response. Sharp: flat to the pass-band edge, full attenuation at the source Nyquist.
// Slow: a short, gentle filter, about -3 dB at 20 kHz (44.1 kHz source) that lets the first images through partly
// attenuated (less ringing, softer treble). NOS: the non-oversampling DAC response, each source sample held for its
// whole period (sin(x)/x roll-off, -3.9 dB at Nyquist, images barely attenuated), band-limited at the output Nyquist
// so the hold steps do not alias. NOS is linear phase and needs an output rate above the source rate; otherwise the
// slow filter is used. Gaussian: a sinc under a Gaussian window (the smallest time-frequency spread: the ringing dies
// away as a Gaussian, without the tail of a truncated window; to -60 dB about ten times sooner than the sharp filter),
// -0.1 dB at 20 kHz (44.1 kHz source), about -3 dB at the source Nyquist and full attenuation at 1.33 times it: images
// just above the Nyquist are only partly removed.
enum class FilterResponse : uint32_t { Sharp = 0, Slow = 1, Nos = 2, Gaussian = 3 };

constexpr std::wstring_view ResponseName(FilterResponse r) {
    return r == FilterResponse::Slow       ? L"slow"
           : r == FilterResponse::Nos      ? L"NOS"
           : r == FilterResponse::Gaussian ? L"Gaussian"
                                           : L"sharp";
}

// Coefficient design of the sharp roll-off. Kaiser: windowed sinc, pass-band ripple as small as the stop-band level.
// Equiripple: Parks-McClellan (Remez) optimum with a pass-band ripple of about +-0.0002 dB, 15-25 % shorter for the
// same edges and attenuation (less latency and ringing). Needs the two-stage oversampler (output at least twice the
// source rate); otherwise the Kaiser design is used.
enum class FilterDesign : uint32_t { Kaiser = 0, Equiripple = 1 };

constexpr std::wstring_view DesignName(FilterDesign d) { return d == FilterDesign::Equiripple ? L"equiripple" : L"Kaiser"; }

// Low-pass prototype specification, relative to the lower of the two sample rates.
struct FilterSpec {
    double attenuationDb = 140.0;  // stop-band attenuation
    double passband = 0.96;        // pass-band edge as a fraction of min(fin, fout) / 2
    double stopband = 1.0;         // stop-band edge, same unit; below 1.0 the filter is apodizing
    FilterPhase phase = FilterPhase::Linear;
    FilterResponse response = FilterResponse::Sharp;
    FilterDesign design = FilterDesign::Kaiser;

    friend constexpr bool operator==(const FilterSpec&, const FilterSpec&) = default;
};

inline constexpr double kMinAttenuationDb = 80.0;
inline constexpr double kMaxAttenuationDb = 150.0;  // float32 coefficient noise floor
inline constexpr double kMinPassband = 0.80;
inline constexpr double kMaxPassband = 0.995;
inline constexpr double kMinStopband = 0.85;
// Apodizing: the stop band starts below the source Nyquist frequency, so the band where the recording's own
// anti-alias filter rings (its transition band just below Nyquist) is removed together with that ringing.
inline constexpr double kApodizingPassband = 0.87;
inline constexpr double kApodizingStopband = 0.955;
// Slow roll-off: -6 dB just below the source Nyquist, the stop band at 0.7 fs (where DAC slow filters put it).
inline constexpr double kSlowPassband = 0.50;
inline constexpr double kSlowStopband = 1.40;
// NOS: the anti-alias filter at the output Nyquist starts at this fraction of it (or just above the source's audio
// band when the output rate is only a little higher).
inline constexpr double kNosFilterStart = 0.5;
// Gaussian: -0.1 dB edge and full-attenuation edge (with apodizing: the apodizing edges).
inline constexpr double kGaussianPassband = 0.907;
inline constexpr double kGaussianStopband = 1.333;
// Equiripple pass-band ripple (design target +-0.0001 dB; +-0.0002 dB measured between the design grid points).
inline constexpr double kEquirippleRipple = 1.15e-5;

inline constexpr std::array<FilterSpec, 4> kPresetSpecs = {{
    {100.0, 0.90},  // Fast
    {120.0, 0.94},  // Balanced
    {140.0, 0.96},  // High
    {150.0, 0.98},  // Ultra
}};

constexpr FilterSpec ClampSpec(FilterSpec s) {
    if (static_cast<uint32_t>(s.response) > 3) s.response = FilterResponse::Sharp;
    s.attenuationDb = std::clamp(s.attenuationDb, kMinAttenuationDb, kMaxAttenuationDb);
    if (s.response == FilterResponse::Slow || s.response == FilterResponse::Gaussian) {
        s.stopband = std::clamp(s.stopband, kMinStopband, kSlowStopband);
        s.passband = std::clamp(s.passband, kSlowPassband, std::min(kMaxPassband, s.stopband - 0.005));
    } else {
        s.stopband = std::clamp(s.stopband, kMinStopband, 1.0);
        s.passband = std::clamp(s.passband, kMinPassband, std::min(kMaxPassband, s.stopband - 0.005));
    }
    if (static_cast<uint32_t>(s.phase) > 2) s.phase = FilterPhase::Linear;
    if (s.response == FilterResponse::Nos) s.phase = FilterPhase::Linear;
    if (static_cast<uint32_t>(s.design) > 1 || s.response != FilterResponse::Sharp) s.design = FilterDesign::Kaiser;
    return s;
}

constexpr FilterSpec SpecFor(QualityPreset preset, FilterSpec custom) {
    const auto i = static_cast<uint32_t>(preset);
    return i < kPresetSpecs.size() ? kPresetSpecs[i] : ClampSpec(custom);
}

// Preset (or custom) spec with the chosen impulse-response shape, roll-off and design. Apodizing applies to the sharp
// and Gaussian roll-offs (the slow and NOS responses are already well down at the source Nyquist).
constexpr FilterSpec ShapeSpec(FilterSpec s, FilterPhase phase, bool apodizing,
                               FilterResponse response = FilterResponse::Sharp,
                               FilterDesign design = FilterDesign::Kaiser) {
    s.phase = phase;
    s.response = response;
    s.design = design;
    if (response == FilterResponse::Slow) {
        s.passband = kSlowPassband;
        s.stopband = kSlowStopband;
    } else if (response == FilterResponse::Gaussian) {
        s.passband = apodizing ? kApodizingPassband : kGaussianPassband;
        s.stopband = apodizing ? kApodizingStopband : kGaussianStopband;
    } else if (apodizing && response == FilterResponse::Sharp) {
        s.stopband = kApodizingStopband;
        s.passband = std::min(s.passband, kApodizingPassband);
    }
    return ClampSpec(s);
}

constexpr std::wstring_view PresetName(QualityPreset preset) {
    switch (preset) {
        case QualityPreset::Fast: return L"Fast";
        case QualityPreset::Balanced: return L"Balanced";
        case QualityPreset::High: return L"High";
        case QualityPreset::Ultra: return L"Ultra";
        case QualityPreset::Custom: return L"Custom";
    }
    return L"?";
}

}  // namespace dgmod::dsp
