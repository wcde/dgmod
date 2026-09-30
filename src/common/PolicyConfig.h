#pragma once

#include "common/AudioFormat.h"
#include "common/Win.h"

#include <wtypes.h>

#include <string>
#include <vector>

namespace dgmod {

// Property keys (defined here to avoid INITGUID / propsys link dependencies).
inline constexpr PROPERTYKEY kPkeyDeviceFriendlyName = {
    {0xa45c254e, 0xdf1c, 0x4efd, {0x80, 0x20, 0x67, 0xd1, 0x46, 0xa8, 0x50, 0xe0}}, 14};
inline constexpr PROPERTYKEY kPkeyAudioEngineDeviceFormat = {
    {0xf19f064d, 0x082c, 0x4e27, {0xbc, 0x73, 0x68, 0x82, 0xa1, 0xbb, 0x8e, 0x4c}}, 0};

struct DeviceFormatOption {
    uint32_t sampleRate = 0;
    uint32_t bits = 0;       // container bits
    uint32_t validBits = 0;
    std::wstring label;      // "24 bit, 192000 Hz"
    WAVEFORMATEXTENSIBLE format{};
};

// "24 bit (32-bit container), 192000 Hz"
std::wstring DeviceFormatLabel(uint32_t rate, uint32_t bits, uint32_t validBits);

// Formats the endpoint accepts in exclusive mode (what the Sound control panel offers as "Default Format").
std::vector<DeviceFormatOption> SupportedDeviceFormats(const std::wstring& deviceId);

// Changes the endpoint's shared-mode device format (the audio engine then mixes at this rate).
// Uses the undocumented IPolicyConfig interface of the Windows audio policy service, like mmsys.cpl.
Result<void> SetDeviceFormat(const std::wstring& deviceId, const WAVEFORMATEXTENSIBLE& format);

// Default render endpoint (console role) and changing it for all roles (IPolicyConfig::SetDefaultEndpoint).
std::wstring DefaultRenderEndpointId();
Result<void> SetDefaultRenderEndpoint(const std::wstring& deviceId);

struct RenderEndpoint {
    std::wstring id;
    std::wstring name;
    DWORD state = 0;
    StreamFormat mixFormat{};     // IAudioClient::GetMixFormat
    StreamFormat deviceFormat{};  // shared-mode device format (PKEY_AudioEngine_DeviceFormat)
};
// Active render endpoints.
std::vector<RenderEndpoint> ActiveRenderEndpoints();

}  // namespace dgmod
