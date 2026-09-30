#include "dsp/PeakLimiter.h"

#include <algorithm>
#include <cmath>

namespace dgmod::dsp {

void PeakLimiter::Configure(uint32_t channels, double sampleRate, float ceiling, double lookaheadMs, double releaseMs,
                            double holdMs) {
    channels_ = std::max<uint32_t>(channels, 1);
    window_ = std::max<uint32_t>(1, static_cast<uint32_t>(std::lround(lookaheadMs * sampleRate / 1000.0)));
    span_ = window_ + static_cast<uint32_t>(std::lround(std::max(0.0, holdMs) * sampleRate / 1000.0));
    ceiling_ = ceiling;
    release_ = 1.0 - std::exp(-1.0 / std::max(1.0, releaseMs * sampleRate / 1000.0));
    delay_.assign(size_t(window_) * channels_, 0.0f);
    held_.assign(window_, 1.0);
    dequeIndex_.assign(size_t(span_) + 1, 0);
    dequeValue_.assign(size_t(span_) + 1, 1.0f);
    Reset();
}

void PeakLimiter::Reset() {
    std::fill(delay_.begin(), delay_.end(), 0.0f);
    std::fill(held_.begin(), held_.end(), 1.0);
    dequeHead_ = dequeSize_ = 0;
    pos_ = counter_ = 0;
    heldState_ = 1.0;
    sum_ = double(window_);
    minGain_ = 1.0f;
    limitedFrames_ = 0;
}

float PeakLimiter::TakeMinGain() {
    const float g = minGain_;
    minGain_ = 1.0f;
    return g;
}

void PeakLimiter::Process(float* data, uint32_t frames) {
    if (!window_) return;
    const uint32_t cap = span_ + 1;
    const double invWindow = 1.0 / double(window_);
    for (uint32_t f = 0; f < frames; ++f) {
        float* x = data + size_t(f) * channels_;
        float peak = 0.0f;
        for (uint32_t c = 0; c < channels_; ++c) peak = std::max(peak, std::abs(x[c]));
        const float required = peak > ceiling_ ? ceiling_ / peak : 1.0f;

        // Sliding minimum of the required gain over the last span_ samples (look-ahead plus hold).
        while (dequeSize_ && dequeValue_[(dequeHead_ + dequeSize_ - 1) % cap] >= required) --dequeSize_;
        const uint32_t back = (dequeHead_ + dequeSize_) % cap;
        dequeIndex_[back] = counter_;
        dequeValue_[back] = required;
        ++dequeSize_;
        if (counter_ - dequeIndex_[dequeHead_] >= span_) {
            dequeHead_ = (dequeHead_ + 1) % cap;
            --dequeSize_;
        }
        const double windowMin = dequeValue_[dequeHead_];
        heldState_ = windowMin < heldState_ ? windowMin : heldState_ + (windowMin - heldState_) * release_;
        if (heldState_ > 1.0 - 1e-9) heldState_ = 1.0;  // fully recovered: bit-exact pass-through again

        // Box filter over the look-ahead window; re-summed once per window against rounding drift.
        sum_ += heldState_ - held_[pos_];
        held_[pos_] = heldState_;
        const double gain = std::min(1.0, sum_ * invWindow);

        // Delay line of window_ - 1 frames: write the new frame, read the one written window_ - 1 frames ago.
        float* slot = delay_.data() + size_t(pos_) * channels_;
        std::copy(x, x + channels_, slot);
        const uint32_t readPos = pos_ + 1 == window_ ? 0 : pos_ + 1;
        const float* old = delay_.data() + size_t(readPos) * channels_;
        const auto g = static_cast<float>(gain);
        for (uint32_t c = 0; c < channels_; ++c) x[c] = old[c] * g;
        if (g < minGain_) minGain_ = g;
        if (gain < 1.0 - 1e-7) ++limitedFrames_;

        ++counter_;
        if (++pos_ == window_) {
            pos_ = 0;
            double s = 0.0;
            for (const double h : held_) s += h;
            sum_ = s;
        }
    }
}

}  // namespace dgmod::dsp
