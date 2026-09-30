#pragma once

#include "dsp/AlignedBuffer.h"
#include "dsp/Quality.h"

#include <cstdint>
#include <memory>
#include <vector>

namespace dgmod::dsp {

struct KernelInfo {
    uint32_t inRate = 0;
    uint32_t outRate = 0;
    uint32_t taps = 0;          // coefficients per phase (multiple of 8)
    uint32_t protoTaps = 0;     // length of the linear-phase prototype (= taps, except intermediate phase: longer)
    uint32_t storedPhases = 0;  // tablePhases + 3 (phases -1 .. tablePhases + 1)
    uint32_t tablePhases = 0;   // resolution of the table: phases per input sample
    uint32_t fixedPhases = 0;   // > 0: fixed ratio outRate = fixedPhases * inRate, every phase exact (no interpolation)
    FilterSpec spec{};
    double beta = 0;            // Kaiser window parameter
    double gaussSigma = 0;      // Gaussian response: window standard deviation, input samples
    double passbandHz = 0;
    double stopbandHz = 0;
    double cutoffHz = 0;        // -6 dB point of the prototype
    double latencyInputFrames = 0;  // group delay at DC, in input frames (estimate for minimum phase until built)

    [[nodiscard]] double LatencySeconds() const { return inRate ? latencyInputFrames / inRate : 0.0; }
    // Multiply-accumulates per second per channel at steady state.
    [[nodiscard]] double MacsPerSecondPerChannel() const { return double(outRate) * taps * (fixedPhases ? 1.0 : 1.5); }
    [[nodiscard]] size_t TableBytes() const { return size_t(storedPhases) * taps * sizeof(float); }
};

// Prototype samples per input sample of the minimum-phase design grid.
inline constexpr int kMinPhaseGrid = 8;

// Immutable polyphase coefficient table for one (inRate, outRate, spec) triple, finely tabulated for a continuously
// variable ratio (VariableResampler), or with `fixedPhases` exact phases for the integer ratio outRate = fixedPhases *
// inRate (first stage of the Oversampler; the equiripple design is only available there). Shareable between sessions.
class Kernel {
public:
    // Builds the table (expensive for high quality; never call from a real-time thread).
    static std::shared_ptr<const Kernel> Create(uint32_t inRate, uint32_t outRate, FilterSpec spec, uint32_t fixedPhases = 0);
    // Same, but served from a process-wide cache of live kernels.
    static std::shared_ptr<const Kernel> Get(uint32_t inRate, uint32_t outRate, FilterSpec spec, uint32_t fixedPhases = 0);
    // Short linear-phase interpolator (Kaiser) flat to `passHz`, full attenuation from `stopHz`: the second stage of
    // the Oversampler, whose input has nothing between passHz and stopHz. Cached like Get.
    static std::shared_ptr<const Kernel> GetInterstage(uint32_t inRate, uint32_t outRate, double passHz, double stopHz,
                                                       double attenuationDb);

    // Filter layout only (no coefficient computation) — used by the UI for estimates. An equiripple design may end
    // up a little longer once built.
    static KernelInfo Plan(uint32_t inRate, uint32_t outRate, FilterSpec spec, uint32_t fixedPhases = 0);
    static KernelInfo PlanInterstage(uint32_t inRate, uint32_t outRate, double passHz, double stopHz, double attenuationDb);

    [[nodiscard]] const KernelInfo& Info() const { return info_; }
    // Coefficients of table phase `index` (index / tablePhases input frames), -1 <= index <= tablePhases + 1.
    [[nodiscard]] const float* Phase(int64_t index) const { return coeffs_.data() + size_t(index + 1) * info_.taps; }

    // Continuous prototype impulse response h(t), t in input samples (for analysis / plots): centered on t = 0 for
    // linear phase, starting at t = 0 (the start of its window) for minimum/intermediate phase. Not normalized.
    [[nodiscard]] double Prototype(double t) const;
    // Position of the largest |h(t)|, in input samples.
    [[nodiscard]] double PeakTime() const { return peakTime_; }

    // Magnitude response (dB) of the effective filter at the given frequencies (Hz), computed from the
    // prototype at the output-side sampling grid. Intended for UI plots, not for real-time use.
    [[nodiscard]] std::vector<double> MagnitudeResponseDb(const std::vector<double>& freqsHz) const;

private:
    static std::shared_ptr<const Kernel> Build(const KernelInfo& info);
    [[nodiscard]] double LinearPrototype(double t) const;
    // Band-limited reconstruction of a tabulated prototype (grid index 0 at t = 0).
    [[nodiscard]] double GridValue(const std::vector<double>& grid, double t) const;
    void DesignShapedPhase(double fraction);
    void DesignNos();
    // Replaces the Kaiser design by the equiripple one (on the grid of the fixed phases); false if it cannot meet the
    // attenuation (the Kaiser design then stays).
    bool DesignEquiripple();

    KernelInfo info_{};
    double cutoffNorm_ = 0;  // cutoff / inRate
    double peakTime_ = 0;
    // Tabulated prototypes, gridDensity_ samples per input sample, values divided by the density. linearGrid_: the
    // linear-phase response where it has no closed form (NOS, which needs a finer grid for its wider band; equiripple),
    // starting at -protoTaps / 2. shapedGrid_: the min./intermediate-phase design, starting at its window.
    std::vector<double> linearGrid_, shapedGrid_;
    int gridDensity_ = kMinPhaseGrid;
    int interpHalf_ = 8;  // half length of the grid reconstruction kernel (longer for coarse grids)
    AlignedBuffer<float> coeffs_;
};

// Typical group delay of the minimum-phase prototype (input samples), used by Plan() before the design exists.
inline constexpr double kMinPhaseLatencyEstimate = 5.0;
// Intermediate phase: the mixed-phase response is longer than the prototype it is derived from; the table holds this
// many times the prototype length. Its delay, as a fraction of the prototype's linear-phase delay (estimate for Plan()).
inline constexpr double kIntermediateLengthFactor = 2.0;
inline constexpr double kIntermediateLatencyFraction = 1.0;

// Modified Bessel function of the first kind, order 0.
double BesselI0(double x);
// Kaiser beta for the requested stop-band attenuation.
double KaiserBeta(double attenuationDb);

}  // namespace dgmod::dsp
