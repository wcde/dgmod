#pragma once

#include "dsp/AlignedBuffer.h"
#include "dsp/Dot.h"
#include "dsp/Kernel.h"

#include <cstdint>
#include <memory>

namespace dgmod::dsp {

// Asynchronous sample-rate converter: polyphase resampler whose ratio can be trimmed continuously (in ppm) to follow
// the drift between two independent clocks. Uses the finely tabulated polyphase prototype of Kernel with linear
// interpolation between neighbouring phases. The output position is an integer
// history index plus a Q32.32 fraction, so frame accounting (InputFramesFor / OutputFramesFor) is exact.
class VariableResampler {
public:
    struct ProcessResult {
        uint32_t produced = 0;
        uint32_t dropped = 0;  // input frames discarded because the history was full
        bool silent = false;
    };

    VariableResampler() = default;
    VariableResampler(const VariableResampler&) = delete;
    VariableResampler& operator=(const VariableResampler&) = delete;

    // Not real-time safe.
    bool Configure(std::shared_ptr<const Kernel> kernel, uint32_t channels, uint32_t maxInputFrames);
    void Reset();

    [[nodiscard]] bool Configured() const { return kernel_ != nullptr; }
    [[nodiscard]] const KernelInfo& Info() const { return kernel_->Info(); }
    [[nodiscard]] uint32_t Channels() const { return channels_; }

    // Ratio trim relative to the nominal inRate/outRate: +100 ppm consumes input 0.01 % faster. Real-time safe.
    void SetCorrectionPpm(double ppm);
    [[nodiscard]] double CorrectionPpm() const { return ppm_; }
    // Input frames per output frame actually used (after Q32.32 quantization).
    [[nodiscard]] double EffectiveStep() const { return double(step_) / 4294967296.0; }

    // Input frames to append so that `outFrames` can be produced with the current ratio.
    [[nodiscard]] uint32_t InputFramesFor(uint32_t outFrames) const;
    // Output frames that `inFrames` more input would allow with the current ratio.
    [[nodiscard]] uint32_t OutputFramesFor(uint32_t inFrames) const;

    ProcessResult Process(const float* in, uint32_t inFrames, float* out, uint32_t maxOut, float gain = 1.0f);

    void SetDotKernels(const DotKernels& k) { dot_ = k; }

private:
    float* Channel(uint32_t c) { return history_.data() + size_t(c) * capacity_; }
    const float* Channel(uint32_t c) const { return history_.data() + size_t(c) * capacity_; }

    std::shared_ptr<const Kernel> kernel_;
    DotKernels dot_ = Kernels();
    uint32_t taps_ = 0;
    uint32_t tablePhases_ = 0;
    double nominalStep_ = 1.0;  // input frames per output frame
    uint64_t step_ = 1ull << 32;  // Q32.32
    double ppm_ = 0.0;

    uint32_t channels_ = 0;
    uint32_t capacity_ = 0;
    AlignedBuffer<float> history_;
    uint32_t fill_ = 0;
    uint32_t pos_ = 0;
    uint64_t frac_ = 0;  // Q32.32, < 1.0
    uint64_t zeroRun_ = 0;
};

}  // namespace dgmod::dsp
