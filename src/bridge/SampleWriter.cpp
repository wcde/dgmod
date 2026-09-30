#include "bridge/SampleWriter.h"

#include <algorithm>
#include <cmath>
#include <cstring>

namespace dgmod::bridge {

void SampleWriter::Configure(uint32_t containerBits, uint32_t validBits, bool isFloat, bool dither, uint32_t deviceChannels) {
    containerBits_ = containerBits;
    validBits_ = std::min(validBits, containerBits);
    bytesPerSample_ = containerBits / 8;
    deviceChannels_ = deviceChannels;
    float_ = isFloat;
    dither_ = dither && !isFloat && validBits_ < 32;
}

float SampleWriter::Dither() {
    // xorshift64*: two uniform variables -> triangular PDF in (-1, 1) LSB.
    auto next = [this] {
        rng_ ^= rng_ >> 12;
        rng_ ^= rng_ << 25;
        rng_ ^= rng_ >> 27;
        return static_cast<float>((rng_ * 0x2545F4914F6CDD1Dull) >> 40) * (1.0f / 16777216.0f);
    };
    return next() - next();
}

uint64_t SampleWriter::Write(const float* src, uint32_t frames, uint32_t srcChannels, void* dstv) {
    auto* dst = static_cast<uint8_t*>(dstv);
    uint64_t clipped = 0;
    const uint32_t copyCh = std::min(srcChannels, deviceChannels_);
    const double scale = std::ldexp(1.0, static_cast<int>(validBits_) - 1);
    const double maxv = scale - 1.0, minv = -scale;
    const int shift = static_cast<int>(containerBits_ - validBits_);
    for (uint32_t f = 0; f < frames; ++f) {
        for (uint32_t c = 0; c < deviceChannels_; ++c) {
            const float s = c < copyCh ? src[size_t(f) * srcChannels + c] : 0.0f;
            uint8_t* out = dst + (size_t(f) * deviceChannels_ + c) * bytesPerSample_;
            if (float_) {
                std::memcpy(out, &s, 4);
                clipped += std::abs(s) > 1.0f;
                continue;
            }
            double v = double(s) * scale;
            if (dither_ && s != 0.0f) v += Dither();
            v = std::nearbyint(v);
            if (v > maxv) {
                v = maxv;
                ++clipped;
            } else if (v < minv) {
                v = minv;
                ++clipped;
            }
            const int64_t iv = static_cast<int64_t>(v) << shift;
            switch (bytesPerSample_) {
                case 2: {
                    const int16_t x = static_cast<int16_t>(iv);
                    std::memcpy(out, &x, 2);
                    break;
                }
                case 3: {
                    const int32_t x = static_cast<int32_t>(iv);
                    out[0] = static_cast<uint8_t>(x);
                    out[1] = static_cast<uint8_t>(x >> 8);
                    out[2] = static_cast<uint8_t>(x >> 16);
                    break;
                }
                default: {
                    const int32_t x = static_cast<int32_t>(iv);
                    std::memcpy(out, &x, 4);
                    break;
                }
            }
        }
    }
    return clipped;
}

void SampleWriter::WriteDop(const uint16_t* words, uint32_t frames, uint32_t srcChannels, void* dstv) {
    auto* dst = static_cast<uint8_t*>(dstv);
    const uint32_t copyCh = std::min(srcChannels, deviceChannels_);
    for (uint32_t f = 0; f < frames; ++f) {
        for (uint32_t c = 0; c < deviceChannels_; ++c) {
            const uint16_t word = c < copyCh ? words[size_t(f) * srcChannels + c] : uint16_t{0x6969};
            const uint32_t v = (uint32_t{dopMarker_} << 16) | word;  // 24-bit DoP sample
            uint8_t* out = dst + (size_t(f) * deviceChannels_ + c) * bytesPerSample_;
            if (bytesPerSample_ == 3) {
                out[0] = static_cast<uint8_t>(v);
                out[1] = static_cast<uint8_t>(v >> 8);
                out[2] = static_cast<uint8_t>(v >> 16);
            } else {
                const uint32_t x = v << 8;  // 24-in-32 and 32-bit containers: the top 24 bits
                std::memcpy(out, &x, 4);
            }
        }
        dopMarker_ = dopMarker_ == 0x05 ? 0xFA : 0x05;
    }
}

void SampleWriter::WriteSilence(uint32_t frames, void* dst) const {
    std::memset(dst, 0, size_t(frames) * deviceChannels_ * bytesPerSample_);
}

}  // namespace dgmod::bridge
