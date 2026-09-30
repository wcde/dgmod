#include "dsp/VariableResampler.h"

#include <algorithm>
#include <cmath>
#include <cstring>

namespace dgmod::dsp {

bool VariableResampler::Configure(std::shared_ptr<const Kernel> kernel, uint32_t channels, uint32_t maxInputFrames) {
    if (!kernel || channels == 0 || maxInputFrames == 0) return false;
    const KernelInfo& k = kernel->Info();
    kernel_ = std::move(kernel);
    taps_ = k.taps;
    tablePhases_ = k.tablePhases;
    nominalStep_ = double(k.inRate) / double(k.outRate);
    SetCorrectionPpm(ppm_);
    channels_ = channels;
    capacity_ = 2 * taps_ + 2 * maxInputFrames + 64;
    history_.Allocate(size_t(capacity_) * channels_);
    Reset();
    return true;
}

void VariableResampler::Reset() {
    if (!kernel_) return;
    history_.Zero();
    fill_ = taps_ - 1;  // causal priming, as in Resampler
    pos_ = 0;
    frac_ = 0;
    zeroRun_ = fill_;
}

namespace {
constexpr double kOne = 4294967296.0;  // 2^32
}

void VariableResampler::SetCorrectionPpm(double ppm) {
    ppm_ = ppm;
    step_ = static_cast<uint64_t>(std::llround(nominalStep_ * (1.0 + ppm * 1e-6) * kOne));
    if (step_ == 0) step_ = 1;
}

uint32_t VariableResampler::InputFramesFor(uint32_t outFrames) const {
    if (!kernel_ || outFrames == 0) return 0;
    const uint64_t last = ((uint64_t{pos_} << 32) + frac_ + uint64_t{outFrames - 1} * step_) >> 32;
    const uint64_t needFill = last + taps_;
    return needFill > fill_ ? static_cast<uint32_t>(needFill - fill_) : 0u;
}

uint32_t VariableResampler::OutputFramesFor(uint32_t inFrames) const {
    if (!kernel_) return 0;
    const uint64_t avail = std::min<uint64_t>(uint64_t{fill_} + inFrames, capacity_);
    if (pos_ + uint64_t{taps_} > avail) return 0;
    // Output n is possible while (frac + n*step) >> 32 <= avail - taps - pos, i.e. frac + n*step < (k+1) << 32.
    const uint64_t k = avail - taps_ - pos_;
    const uint64_t limit = (k + 1) << 32;
    return static_cast<uint32_t>(std::min<uint64_t>((limit - frac_ + step_ - 1) / step_, UINT32_MAX));
}

VariableResampler::ProcessResult VariableResampler::Process(const float* in, uint32_t inFrames, float* out,
                                                            uint32_t maxOut, float gain) {
    ProcessResult r;
    if (!kernel_) return r;

    const uint32_t space = capacity_ - fill_;
    const uint32_t n = std::min(inFrames, space);
    r.dropped = inFrames - n;
    if (n) {
        if (in) {
            uint32_t lastNonZero = UINT32_MAX;
            for (uint32_t c = 0; c < channels_; ++c) {
                float* dst = Channel(c) + fill_;
                const float* src = in + c;
                for (uint32_t i = 0; i < n; ++i) {
                    const float v = src[size_t(i) * channels_] * gain;
                    dst[i] = v;
                    if (v != 0.0f && (lastNonZero == UINT32_MAX || i > lastNonZero)) lastNonZero = i;
                }
            }
            zeroRun_ = lastNonZero == UINT32_MAX ? zeroRun_ + n : uint64_t{n - 1 - lastNonZero};
        } else {
            for (uint32_t c = 0; c < channels_; ++c) std::memset(Channel(c) + fill_, 0, size_t(n) * sizeof(float));
            zeroRun_ += n;
        }
        fill_ += n;
    }

    const uint64_t zeroStart = fill_ - std::min<uint64_t>(zeroRun_, fill_);
    r.silent = pos_ >= zeroStart;
    uint32_t produced = 0;
    while (produced < maxOut && pos_ + taps_ <= fill_) {
        float* dst = out + size_t(produced) * channels_;
        if (r.silent) {
            std::memset(dst, 0, size_t(channels_) * sizeof(float));
        } else {
            const uint64_t x = frac_ * tablePhases_;  // Q32.32 position in table phases
            const int64_t idx = static_cast<int64_t>(x >> 32);
            if ((x & 0xFFFFFFFFull) == 0) {
                // On a stored phase (always for fixed-phase kernels): one row, no interpolation.
                const float* row = kernel_->Phase(idx);
                for (uint32_t c = 0; c < channels_; ++c) dst[c] = static_cast<float>(dot_.dot(Channel(c) + pos_, row, taps_));
                frac_ += step_;
                pos_ += static_cast<uint32_t>(frac_ >> 32);
                frac_ &= 0xFFFFFFFFull;
                ++produced;
                continue;
            }
            float w[4];
            CubicWeights(static_cast<float>(double(x & 0xFFFFFFFFull) / kOne), w);
            const float* rows[4] = {kernel_->Phase(idx - 1), kernel_->Phase(idx), kernel_->Phase(idx + 1),
                                    kernel_->Phase(idx + 2)};
            for (uint32_t c = 0; c < channels_; ++c)
                dst[c] = static_cast<float>(dot_.dotCubic(Channel(c) + pos_, rows, w, taps_));
        }
        frac_ += step_;
        pos_ += static_cast<uint32_t>(frac_ >> 32);
        frac_ &= 0xFFFFFFFFull;
        ++produced;
    }
    r.produced = produced;

    if (pos_ > 0) {
        const uint32_t keep = fill_ - std::min(pos_, fill_);
        for (uint32_t c = 0; c < channels_; ++c) std::memmove(Channel(c), Channel(c) + pos_, size_t(keep) * sizeof(float));
        fill_ = keep;
        pos_ = 0;
    }
    return r;
}

}  // namespace dgmod::dsp
