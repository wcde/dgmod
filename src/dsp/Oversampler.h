#pragma once

#include "dsp/Kernel.h"
#include "dsp/VariableResampler.h"

#include <cstdint>
#include <memory>
#include <vector>

namespace dgmod::dsp {

// Layout of the Oversampler for one (inRate, outRate, spec): either a single variable-ratio stage with the whole filter
// (downsampling, NOS, or an output too low for the cascade), or two stages:
//   1. x2 at twice the source rate with the filter that defines the sound (every phase exact, cost per output sample
//      = its length, so even very long or equiripple filters are cheap), then
//   2. a short Kaiser interpolator to the output rate that follows the clock drift. Its input holds nothing between
//      the stage-1 stop band and its first image, so a few dozen taps keep the attenuation.
struct OversamplerPlan {
    KernelInfo filter;  // the sound-defining filter (stage 1, or the single stage)
    KernelInfo interp;  // stage 2 (taps == 0 without a cascade)
    [[nodiscard]] bool Cascade() const { return interp.taps != 0; }
    [[nodiscard]] double LatencySeconds() const {
        return filter.LatencySeconds() + (Cascade() ? interp.LatencySeconds() : 0.0);
    }
    // Time the filters need to empty (input seconds).
    [[nodiscard]] double LengthSeconds() const {
        return (filter.inRate ? double(filter.taps) / filter.inRate : 0.0) +
               (Cascade() && interp.inRate ? double(interp.taps) / interp.inRate : 0.0);
    }
    [[nodiscard]] double MacsPerSecondPerChannel() const {
        return filter.MacsPerSecondPerChannel() + (Cascade() ? interp.MacsPerSecondPerChannel() : 0.0);
    }
};

// Sample-rate converter of the bridge (see OversamplerPlan), same interface as VariableResampler: the drift trim
// applies to the last stage, the frame accounting stays exact.
class Oversampler {
public:
    // Estimates only (no coefficient computation).
    static OversamplerPlan Plan(uint32_t inRate, uint32_t outRate, FilterSpec spec);
    // The kernel whose response is heard: stage 1 of the cascade, or the single stage (for plots).
    static std::shared_ptr<const Kernel> FilterKernel(uint32_t inRate, uint32_t outRate, FilterSpec spec);

    // Not real-time safe.
    bool Configure(uint32_t inRate, uint32_t outRate, FilterSpec spec, uint32_t channels, uint32_t maxInputFrames);
    void Reset();

    [[nodiscard]] bool Configured() const { return last_.Configured(); }
    [[nodiscard]] bool Cascade() const { return cascade_; }
    // Layout as built (the equiripple design may be longer than planned).
    [[nodiscard]] const OversamplerPlan& Info() const { return plan_; }

    void SetCorrectionPpm(double ppm) { last_.SetCorrectionPpm(ppm); }
    // Input frames per output frame actually used (the x2 stage is exact).
    [[nodiscard]] double EffectiveStep() const { return last_.EffectiveStep() / (cascade_ ? 2.0 : 1.0); }
    [[nodiscard]] double CorrectionPpm() const { return last_.CorrectionPpm(); }
    [[nodiscard]] uint32_t InputFramesFor(uint32_t outFrames) const;
    [[nodiscard]] uint32_t OutputFramesFor(uint32_t inFrames) const;
    VariableResampler::ProcessResult Process(const float* in, uint32_t inFrames, float* out, uint32_t maxOut,
                                             float gain = 1.0f);

private:
    static bool UseCascade(uint32_t inRate, uint32_t outRate, FilterSpec spec, KernelInfo& filter);

    OversamplerPlan plan_;
    bool cascade_ = false;
    VariableResampler first_;  // x2 (cascade only)
    VariableResampler last_;   // variable ratio to the output rate
    std::vector<float> mid_;   // stage-1 output of one call
    uint32_t channels_ = 0;
};

}  // namespace dgmod::dsp
