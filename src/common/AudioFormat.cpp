#include "common/AudioFormat.h"

#include <ks.h>
#include <ksmedia.h>

#include <format>

namespace dgmod {

namespace {

// KSDATAFORMAT_SUBTYPE_PCM / _IEEE_FLOAT share the layout {tag-0000-0010-8000-00aa00389b71}.
constexpr GUID kSubtypeBase = {0x00000000, 0x0000, 0x0010, {0x80, 0x00, 0x00, 0xaa, 0x00, 0x38, 0x9b, 0x71}};

bool IsBaseSubtype(const GUID& g) {
    return g.Data2 == kSubtypeBase.Data2 && g.Data3 == kSubtypeBase.Data3 &&
           memcmp(g.Data4, kSubtypeBase.Data4, sizeof(g.Data4)) == 0 && g.Data1 <= 0xFFFF;
}

}  // namespace

bool DescribeFormat(const WAVEFORMATEX* wfx, StreamFormat& out) {
    out = {};
    if (!wfx) return false;
    out.tag = wfx->wFormatTag;
    out.channels = wfx->nChannels;
    out.sampleRate = wfx->nSamplesPerSec;
    out.bitsPerSample = wfx->wBitsPerSample;
    out.validBits = wfx->wBitsPerSample;
    if (wfx->wFormatTag == WAVE_FORMAT_EXTENSIBLE && wfx->cbSize >= sizeof(WAVEFORMATEXTENSIBLE) - sizeof(WAVEFORMATEX)) {
        const auto* ext = reinterpret_cast<const WAVEFORMATEXTENSIBLE*>(wfx);
        out.extensible = true;
        out.channelMask = ext->dwChannelMask;
        out.validBits = ext->Samples.wValidBitsPerSample ? ext->Samples.wValidBitsPerSample : wfx->wBitsPerSample;
        if (IsBaseSubtype(ext->SubFormat)) out.tag = static_cast<uint16_t>(ext->SubFormat.Data1);
        else out.tag = 0xFFFE;
    }
    out.isFloat = out.tag == WAVE_FORMAT_IEEE_FLOAT;
    out.isPcm = out.tag == WAVE_FORMAT_PCM;
    return out.isFloat || out.isPcm;
}

std::wstring FormatToString(const StreamFormat& f) {
    std::wstring kind;
    if (f.isFloat) kind = std::format(L"float{}", f.bitsPerSample);
    else if (f.isPcm) kind = f.validBits != f.bitsPerSample ? std::format(L"pcm{}/{}", f.validBits, f.bitsPerSample)
                                                             : std::format(L"pcm{}", f.bitsPerSample);
    else return std::format(L"tag 0x{:04X} {}ch {} Hz", f.tag, f.channels, f.sampleRate);
    return std::format(L"{} {}ch {} Hz", kind, f.channels, f.sampleRate);
}

uint32_t DefaultChannelMask(uint32_t channels) {
    switch (channels) {
        case 1: return KSAUDIO_SPEAKER_MONO;
        case 2: return KSAUDIO_SPEAKER_STEREO;
        case 3: return KSAUDIO_SPEAKER_STEREO | SPEAKER_FRONT_CENTER;
        case 4: return KSAUDIO_SPEAKER_QUAD;
        case 5: return KSAUDIO_SPEAKER_QUAD | SPEAKER_FRONT_CENTER;
        case 6: return KSAUDIO_SPEAKER_5POINT1;
        case 8: return KSAUDIO_SPEAKER_7POINT1_SURROUND;
        default: return 0;
    }
}

WAVEFORMATEXTENSIBLE MakeFloatFormat(uint32_t sampleRate, uint32_t channels, uint32_t channelMask) {
    WAVEFORMATEXTENSIBLE f{};
    f.Format.wFormatTag = WAVE_FORMAT_EXTENSIBLE;
    f.Format.nChannels = static_cast<WORD>(channels);
    f.Format.nSamplesPerSec = sampleRate;
    f.Format.wBitsPerSample = 32;
    f.Format.nBlockAlign = static_cast<WORD>(channels * 4);
    f.Format.nAvgBytesPerSec = sampleRate * f.Format.nBlockAlign;
    f.Format.cbSize = sizeof(WAVEFORMATEXTENSIBLE) - sizeof(WAVEFORMATEX);
    f.Samples.wValidBitsPerSample = 32;
    f.dwChannelMask = channelMask ? channelMask : DefaultChannelMask(channels);
    f.SubFormat = kSubtypeBase;
    f.SubFormat.Data1 = WAVE_FORMAT_IEEE_FLOAT;
    return f;
}

}  // namespace dgmod
