#pragma once

#include <cstdint>

namespace dgmod::bridge {

// Converts float frames to the DAC's exclusive-mode PCM format: 16-bit, packed 24-bit, 24-in-32 or 32-bit integer,
// or float32. Optional TPDF dither for 16/24-bit targets. Maps source channels onto device channels (extra device
// channels are silent).
class SampleWriter {
public:
    void Configure(uint32_t containerBits, uint32_t validBits, bool isFloat, bool dither, uint32_t deviceChannels);

    // Returns the number of samples that had to be clipped.
    uint64_t Write(const float* src, uint32_t frames, uint32_t srcChannels, void* dst);
    void WriteSilence(uint32_t frames, void* dst) const;
    // DoP v1.1 (DSD over PCM, needs 24 or more bits): per channel and frame 16 DSD bits (the earliest in the MSB)
    // under a marker byte that alternates 0x05 / 0xFA from frame to frame, in the top 24 bits of the container.
    // Device channels without a source channel carry DSD silence (0x69 pattern), so the DAC stays in DSD mode.
    void WriteDop(const uint16_t* words, uint32_t frames, uint32_t srcChannels, void* dst);

    [[nodiscard]] uint32_t BytesPerFrame() const { return bytesPerSample_ * deviceChannels_; }

private:
    float Dither();

    uint32_t containerBits_ = 32;
    uint32_t validBits_ = 24;
    uint32_t bytesPerSample_ = 4;
    uint32_t deviceChannels_ = 2;
    bool float_ = false;
    bool dither_ = true;
    uint64_t rng_ = 0x9E3779B97F4A7C15ull;
    uint8_t dopMarker_ = 0x05;
};

}  // namespace dgmod::bridge
