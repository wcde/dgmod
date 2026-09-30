#include "common/PolicyConfig.h"

#include <audioclient.h>
#include <mmdeviceapi.h>

#include <format>

namespace dgmod {

namespace {

// Undocumented, stable since Windows 7 (used by mmsys.cpl, SoundSwitch, EarTrumpet and others).
struct DECLSPEC_UUID("f8679f50-850a-41cf-9c72-430f290290c8") IPolicyConfig : public IUnknown {
    virtual HRESULT STDMETHODCALLTYPE GetMixFormat(PCWSTR, WAVEFORMATEX**) = 0;
    virtual HRESULT STDMETHODCALLTYPE GetDeviceFormat(PCWSTR, INT, WAVEFORMATEX**) = 0;
    virtual HRESULT STDMETHODCALLTYPE ResetDeviceFormat(PCWSTR) = 0;
    virtual HRESULT STDMETHODCALLTYPE SetDeviceFormat(PCWSTR, WAVEFORMATEX* endpointFormat, WAVEFORMATEX* mixFormat) = 0;
    virtual HRESULT STDMETHODCALLTYPE GetProcessingPeriod(PCWSTR, INT, PINT64, PINT64) = 0;
    virtual HRESULT STDMETHODCALLTYPE SetProcessingPeriod(PCWSTR, PINT64) = 0;
    virtual HRESULT STDMETHODCALLTYPE GetShareMode(PCWSTR, void*) = 0;
    virtual HRESULT STDMETHODCALLTYPE SetShareMode(PCWSTR, void*) = 0;
    virtual HRESULT STDMETHODCALLTYPE GetPropertyValue(PCWSTR, const PROPERTYKEY&, PROPVARIANT*) = 0;
    virtual HRESULT STDMETHODCALLTYPE SetPropertyValue(PCWSTR, const PROPERTYKEY&, PROPVARIANT*) = 0;
    virtual HRESULT STDMETHODCALLTYPE SetDefaultEndpoint(PCWSTR, ERole) = 0;
    virtual HRESULT STDMETHODCALLTYPE SetEndpointVisibility(PCWSTR, INT) = 0;
};

constexpr CLSID kClsidPolicyConfigClient = {0x870af99c, 0x171d, 0x4f9e, {0xaf, 0x0d, 0xe6, 0x3d, 0xf4, 0x0c, 0x2b, 0xc9}};

WAVEFORMATEXTENSIBLE MakePcm(uint32_t rate, uint32_t channels, uint32_t mask, uint32_t bits, uint32_t validBits) {
    WAVEFORMATEXTENSIBLE f = MakeFloatFormat(rate, channels, mask);
    f.Format.wBitsPerSample = static_cast<WORD>(bits);
    f.Format.nBlockAlign = static_cast<WORD>(channels * bits / 8);
    f.Format.nAvgBytesPerSec = rate * f.Format.nBlockAlign;
    f.Samples.wValidBitsPerSample = static_cast<WORD>(validBits);
    f.SubFormat.Data1 = WAVE_FORMAT_PCM;
    return f;
}

}  // namespace

std::wstring DeviceFormatLabel(uint32_t rate, uint32_t bits, uint32_t validBits) {
    const std::wstring depth = bits == validBits ? std::format(L"{} bit", bits)
                                                 : std::format(L"{} bit ({}-bit container)", validBits, bits);
    return std::format(L"{}, {} Hz", depth, rate);
}

std::vector<DeviceFormatOption> SupportedDeviceFormats(const std::wstring& deviceId) {
    std::vector<DeviceFormatOption> out;
    ComPtr<IMMDeviceEnumerator> e;
    if (FAILED(::CoCreateInstance(__uuidof(MMDeviceEnumerator), nullptr, CLSCTX_ALL, IID_PPV_ARGS(&e)))) return out;
    ComPtr<IMMDevice> device;
    if (FAILED(e->GetDevice(deviceId.c_str(), &device))) return out;
    ComPtr<IAudioClient> client;
    if (FAILED(device->Activate(__uuidof(IAudioClient), CLSCTX_ALL, nullptr, reinterpret_cast<void**>(&client)))) return out;
    WAVEFORMATEX* mix = nullptr;
    StreamFormat mf;
    if (SUCCEEDED(client->GetMixFormat(&mix))) DescribeFormat(mix, mf);
    ::CoTaskMemFree(mix);
    const uint32_t channels = mf.channels ? mf.channels : 2;
    const uint32_t mask = mf.channelMask;
    struct Depth {
        uint32_t bits, valid;
        const wchar_t* name;
    };
    constexpr Depth kDepths[] = {{16, 16, L"16 bit"}, {24, 24, L"24 bit"}, {32, 24, L"24 bit (32-bit container)"}, {32, 32, L"32 bit"}};
    for (const uint32_t rate : {44100u, 48000u, 88200u, 96000u, 176400u, 192000u, 352800u, 384000u, 705600u, 768000u}) {
        bool any24 = false;
        for (const Depth& d : kDepths) {
            if (d.bits == 32 && d.valid == 24 && any24) continue;  // show the 32-bit container only if packed 24 fails
            const auto f = MakePcm(rate, channels, mask, d.bits, d.valid);
            if (client->IsFormatSupported(AUDCLNT_SHAREMODE_EXCLUSIVE, &f.Format, nullptr) != S_OK) continue;
            if (d.bits == 24) any24 = true;
            out.push_back({rate, d.bits, d.valid, DeviceFormatLabel(rate, d.bits, d.valid), f});
        }
    }
    return out;
}

Result<void> SetDeviceFormat(const std::wstring& deviceId, const WAVEFORMATEXTENSIBLE& format) {
    ComPtr<IPolicyConfig> policy;
    HRESULT hr = ::CoCreateInstance(kClsidPolicyConfigClient, nullptr, CLSCTX_ALL, __uuidof(IPolicyConfig),
                                    reinterpret_cast<void**>(&policy));
    if (FAILED(hr)) return Fail(hr, L"PolicyConfig is not available");
    WAVEFORMATEXTENSIBLE endpoint = format;
    WAVEFORMATEXTENSIBLE mix = MakeFloatFormat(format.Format.nSamplesPerSec, format.Format.nChannels, format.dwChannelMask);
    hr = policy->SetDeviceFormat(deviceId.c_str(), &endpoint.Format, &mix.Format);
    if (FAILED(hr)) return Fail(hr, L"SetDeviceFormat");
    return {};
}

std::wstring DefaultRenderEndpointId() {
    ComPtr<IMMDeviceEnumerator> e;
    if (FAILED(::CoCreateInstance(__uuidof(MMDeviceEnumerator), nullptr, CLSCTX_ALL, IID_PPV_ARGS(&e)))) return {};
    ComPtr<IMMDevice> d;
    LPWSTR id = nullptr;
    if (FAILED(e->GetDefaultAudioEndpoint(eRender, eConsole, &d)) || FAILED(d->GetId(&id))) return {};
    std::wstring out = id;
    ::CoTaskMemFree(id);
    return out;
}

Result<void> SetDefaultRenderEndpoint(const std::wstring& deviceId) {
    ComPtr<IPolicyConfig> policy;
    HRESULT hr = ::CoCreateInstance(kClsidPolicyConfigClient, nullptr, CLSCTX_ALL, __uuidof(IPolicyConfig),
                                    reinterpret_cast<void**>(&policy));
    if (FAILED(hr)) return Fail(hr, L"PolicyConfig is not available");
    for (const ERole role : {eConsole, eMultimedia, eCommunications}) {
        hr = policy->SetDefaultEndpoint(deviceId.c_str(), role);
        if (FAILED(hr)) return Fail(hr, L"SetDefaultEndpoint");
    }
    return {};
}

std::vector<RenderEndpoint> ActiveRenderEndpoints() {
    std::vector<RenderEndpoint> out;
    ComPtr<IMMDeviceEnumerator> e;
    if (FAILED(::CoCreateInstance(__uuidof(MMDeviceEnumerator), nullptr, CLSCTX_ALL, IID_PPV_ARGS(&e)))) return out;
    ComPtr<IMMDeviceCollection> list;
    if (FAILED(e->EnumAudioEndpoints(eRender, DEVICE_STATE_ACTIVE, &list))) return out;
    UINT count = 0;
    list->GetCount(&count);
    for (UINT i = 0; i < count; ++i) {
        ComPtr<IMMDevice> d;
        if (FAILED(list->Item(i, &d))) continue;
        RenderEndpoint ep;
        LPWSTR id = nullptr;
        if (FAILED(d->GetId(&id))) continue;
        ep.id = id;
        ::CoTaskMemFree(id);
        d->GetState(&ep.state);
        ComPtr<IPropertyStore> props;
        if (SUCCEEDED(d->OpenPropertyStore(STGM_READ, &props))) {
            PROPVARIANT v;
            PropVariantInit(&v);
            if (SUCCEEDED(props->GetValue(kPkeyDeviceFriendlyName, &v)) && v.vt == VT_LPWSTR) ep.name = v.pwszVal;
            PropVariantClear(&v);
            if (SUCCEEDED(props->GetValue(kPkeyAudioEngineDeviceFormat, &v)) && v.vt == VT_BLOB &&
                v.blob.cbSize >= sizeof(WAVEFORMATEX))
                DescribeFormat(reinterpret_cast<const WAVEFORMATEX*>(v.blob.pBlobData), ep.deviceFormat);
            PropVariantClear(&v);
        }
        ComPtr<IAudioClient> client;
        if (SUCCEEDED(d->Activate(__uuidof(IAudioClient), CLSCTX_ALL, nullptr, reinterpret_cast<void**>(&client)))) {
            WAVEFORMATEX* mix = nullptr;
            if (SUCCEEDED(client->GetMixFormat(&mix))) DescribeFormat(mix, ep.mixFormat);
            ::CoTaskMemFree(mix);
        }
        out.push_back(std::move(ep));
    }
    return out;
}

}  // namespace dgmod
