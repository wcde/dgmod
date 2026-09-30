#pragma once

#include <cstdint>
#include <vector>

namespace dgmod::dsp {

// Look-ahead peak limiter for the oversampled output. At 8x or more above the source rate the output samples trace the
// reconstructed waveform, so bounding them bounds the true (inter-sample) peak that the DAC would otherwise clip.
// Channels are linked (one gain), the gain never exceeds 1 and reaches the required value exactly when a peak leaves
// the delay line:
//   required[n] = min(1, ceiling / max|x[n]|)
//   held[n]     = min(window minimum of required over the look-ahead plus the hold, exponential release of held[n-1])
//   gain[n]     = mean of held over the look-ahead window,  y[n] = x[n - (W - 1)] * gain[n]
// Every held value in the window that averages into the gain of a peak is <= that peak's requirement, so the
// output never exceeds the ceiling. Signals below the ceiling pass unchanged (only delayed).
// The hold keeps the gain down for a while after the last peak: bass waveforms peak every half period, and a gain
// that recovered in between would ride on each of them (amplitude modulation of the whole mix at the bass frequency,
// heard as rasp). 25 ms covers the half period down to 20 Hz, so a steady over-loud bass gets one constant gain.
class PeakLimiter {
public:
    void Configure(uint32_t channels, double sampleRate, float ceiling, double lookaheadMs = 2.0, double releaseMs = 60.0,
                   double holdMs = 25.0);
    void Reset();
    [[nodiscard]] bool Configured() const { return window_ != 0; }

    // Interleaved, in place. Real-time safe.
    void Process(float* data, uint32_t frames);

    [[nodiscard]] uint32_t LatencyFrames() const { return window_ ? window_ - 1 : 0; }
    [[nodiscard]] float Ceiling() const { return ceiling_; }
    // Lowest gain applied since the previous call (1 = no limiting).
    float TakeMinGain();
    // Frames processed with a gain below 1, since Configure/Reset.
    [[nodiscard]] uint64_t LimitedFrames() const { return limitedFrames_; }

private:
    uint32_t channels_ = 0;
    uint32_t window_ = 0;
    uint32_t span_ = 0;  // window_ + hold: length of the sliding minimum
    float ceiling_ = 1.0f;
    double release_ = 0.0;  // per-sample recovery coefficient

    std::vector<float> delay_;  // window_ frames, interleaved ring
    std::vector<double> held_;  // window_ values, ring (box-filter input)
    std::vector<uint32_t> dequeIndex_;  // monotonic deque (ring) of sample indices with increasing required gain
    std::vector<float> dequeValue_;
    uint32_t dequeHead_ = 0, dequeSize_ = 0;
    uint32_t pos_ = 0;       // ring position
    uint32_t counter_ = 0;   // running sample index (wraps; only differences are used)
    double heldState_ = 1.0;
    double sum_ = 0.0;       // sum of held_
    float minGain_ = 1.0f;
    uint64_t limitedFrames_ = 0;
};

}  // namespace dgmod::dsp
