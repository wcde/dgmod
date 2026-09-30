#pragma once

// Bit-exact pass-through between two clocks running at the same nominal rate (bypass mode of the bridge).
//
// Frames are copied unchanged; the clock drift is absorbed by consuming one input frame more or less now and then,
// as the drift controller asks. A slip is not a hard jump: it is spread over a short linear fractional-delay ramp, so
// it does not click. Outside those ramps (a few per second at typical drift) the output is bit-exact.

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <vector>

namespace dgmod::bridge {

inline constexpr uint32_t kSlipRampFrames = 64;  // frames over which one slip is spread

class SlipCopier {
public:
    static constexpr uint32_t kRamp = kSlipRampFrames;

    void Configure(uint32_t channels) {
        channels_ = channels;
        last_.assign(channels, 0.0f);
        acc_ = 0;
        ppm_ = 0;
        slip_ = 0;
        planned_ = false;
    }

    // Positive = consume input faster (same convention as the resampler trim).
    void SetCorrectionPpm(double ppm) { ppm_ = ppm; }

    // Input frames the next Process() for `outFrames` will consume: outFrames, outFrames + 1 or outFrames - 1.
    // Idempotent until Process() runs, so it can be asked again after an underrun.
    uint32_t InputFramesFor(uint32_t outFrames) {
        if (!planned_) {
            acc_ += ppm_ * 1e-6 * double(outFrames);
            slip_ = 0;
            if (outFrames >= 2) {
                if (acc_ >= 1.0) slip_ = 1;
                else if (acc_ <= -1.0) slip_ = -1;
            }
            acc_ -= slip_;
            planned_ = true;
        }
        return static_cast<uint32_t>(int64_t{outFrames} + slip_);
    }

    [[nodiscard]] int PlannedSlip() const { return slip_; }

    // `in` holds InputFramesFor(outFrames) interleaved frames.
    void Process(const float* in, uint32_t inFrames, float* out, uint32_t outFrames) {
        InputFramesFor(outFrames);
        planned_ = false;
        const size_t ch = channels_;
        if (slip_ == 0 || inFrames != uint32_t(int64_t{outFrames} + slip_)) {
            const uint32_t n = std::min(inFrames, outFrames);
            std::copy(in, in + size_t(n) * ch, out);
            std::fill(out + size_t(n) * ch, out + size_t(outFrames) * ch, 0.0f);
        } else {
            const uint32_t ramp = std::min(kRamp, outFrames);
            for (uint32_t i = 0; i < outFrames; ++i) {
                // Position in the input: i, sliding by one frame (forward or back) over the first `ramp` frames.
                const double pos = double(i) + double(slip_) * double(std::min(i + 1, ramp)) / double(ramp);
                const double k = std::floor(pos);
                const float t = static_cast<float>(pos - k);
                const int64_t ki = static_cast<int64_t>(k);
                const float* a = ki < 0 ? last_.data() : in + size_t(ki) * ch;
                float* o = out + size_t(i) * ch;
                if (t == 0.0f) {
                    std::copy(a, a + ch, o);
                } else {
                    const float* b = in + size_t(ki + 1) * ch;
                    for (size_t c = 0; c < ch; ++c) o[c] = a[c] + t * (b[c] - a[c]);
                }
            }
        }
        if (inFrames) std::copy(in + size_t(inFrames - 1) * ch, in + size_t(inFrames) * ch, last_.begin());
    }

private:
    uint32_t channels_ = 0;
    std::vector<float> last_;  // last consumed input frame (needed by a backward slip)
    double acc_ = 0;           // accumulated fractional frames still to slip
    double ppm_ = 0;
    int slip_ = 0;
    bool planned_ = false;
};

}  // namespace dgmod::bridge
