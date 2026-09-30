#pragma once

#include <windows.h>
#include <mmreg.h>

#include <cstdint>
#include <string>

namespace dgmod {

// Flattened description of a WAVEFORMATEX / WAVEFORMATEXTENSIBLE.
struct StreamFormat {
    uint16_t tag = 0;            // WAVE_FORMAT_* (EXTENSIBLE resolved to the sub-format tag when known)
    bool extensible = false;
    bool isFloat = false;
    bool isPcm = false;
    uint32_t channels = 0;
    uint32_t sampleRate = 0;
    uint32_t bitsPerSample = 0;  // container bits
    uint32_t validBits = 0;
    uint32_t channelMask = 0;

    [[nodiscard]] bool IsFloat32() const { return isFloat && bitsPerSample == 32; }
    friend bool operator==(const StreamFormat&, const StreamFormat&) = default;
};

// Returns false for null or compressed/unknown formats (fields are still filled where possible).
bool DescribeFormat(const WAVEFORMATEX* wfx, StreamFormat& out);

// "float32 2ch 44100 Hz", "pcm24/32 6ch 48000 Hz", "tag 0x1610".
std::wstring FormatToString(const StreamFormat& f);

// IEEE float32 WAVEFORMATEXTENSIBLE; mask 0 picks the default speaker mask for the channel count.
WAVEFORMATEXTENSIBLE MakeFloatFormat(uint32_t sampleRate, uint32_t channels, uint32_t channelMask = 0);

uint32_t DefaultChannelMask(uint32_t channels);

}  // namespace dgmod
