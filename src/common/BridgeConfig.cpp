#include "common/BridgeConfig.h"

#include "common/Registry.h"

#include <algorithm>
#include <cmath>
#include <cwctype>
#include <format>
#include <span>

namespace dgmod {

namespace {

constexpr wchar_t kRunKey[] = L"Software\\Microsoft\\Windows\\CurrentVersion\\Run";

bool ValidBits(uint32_t b) { return b == 16 || b == 24 || b == 32; }

}  // namespace

void BridgeConfig::Validate() {
    if (static_cast<uint32_t>(quality) >= dsp::kPresetCount) quality = dsp::QualityPreset::High;
    custom = dsp::ClampSpec(custom);
    if (!std::isfinite(headroomDb)) headroomDb = 0.0;
    headroomDb = std::clamp(headroomDb, 0.0, 12.0);
    if (outputRate && (outputRate < 8000 || outputRate > 768000)) outputRate = 0;
    if (!ValidBits(outputBits)) outputBits = 32;
    if (!ValidBits(outputValidBits) || outputValidBits > outputBits) outputValidBits = std::min<uint32_t>(24, outputBits);
    bufferMs = std::clamp<uint32_t>(bufferMs, 10, 500);
    if (periodMs > 100) periodMs = 0;
    if (static_cast<uint32_t>(filterPhase) > 2) filterPhase = dsp::FilterPhase::Linear;
    if (static_cast<uint32_t>(filterResponse) > 3) filterResponse = dsp::FilterResponse::Sharp;
    if (static_cast<uint32_t>(filterDesign) > 1) filterDesign = dsp::FilterDesign::Kaiser;
    if (outputMode != OutputMode::Dop && outputMode != OutputMode::DsdNative) outputMode = OutputMode::Pcm;
    const std::span<const uint32_t> rates =
        NativeDsd() ? std::span<const uint32_t>(kNativeDsdMultiples) : std::span<const uint32_t>(kDsdMultiples);
    if (std::find(rates.begin(), rates.end(), dsdMultiple) == rates.end()) dsdMultiple = 0;
    if (static_cast<uint32_t>(dsdLookAhead) >= dsp::kLookAheadLevels) dsdLookAhead = dsp::LookAhead::Off;
    tone.Validate();
}

BridgeConfig LoadBridgeConfig() {
    BridgeConfig c;
    auto key = reg::Open(HKEY_CURRENT_USER, kRegBridge, KEY_QUERY_VALUE);
    if (!key) return c;
    const HKEY k = key->Get();
    if (auto v = reg::ReadString(k, L"Source")) c.sourceId = *v;
    if (auto v = reg::ReadString(k, L"Output")) c.outputId = *v;
    if (auto v = reg::ReadDword(k, L"OutputRate")) c.outputRate = *v;
    if (auto v = reg::ReadDword(k, L"OutputBits")) c.outputBits = *v;
    if (auto v = reg::ReadDword(k, L"OutputValidBits")) c.outputValidBits = *v;
    if (auto v = reg::ReadDword(k, L"Quality")) c.quality = static_cast<dsp::QualityPreset>(*v);
    if (auto v = reg::ReadDword(k, L"CustomAttenuationCentiDb")) c.custom.attenuationDb = *v / 100.0;
    if (auto v = reg::ReadDword(k, L"CustomPassbandPpm")) c.custom.passband = *v / 1e6;
    if (auto v = reg::ReadDword(k, L"FilterPhase")) c.filterPhase = static_cast<dsp::FilterPhase>(*v);
    if (auto v = reg::ReadDword(k, L"Apodizing")) c.apodizing = *v != 0;
    if (auto v = reg::ReadDword(k, L"FilterResponse")) c.filterResponse = static_cast<dsp::FilterResponse>(*v);
    if (auto v = reg::ReadDword(k, L"FilterDesign")) c.filterDesign = static_cast<dsp::FilterDesign>(*v);
    if (auto v = reg::ReadDword(k, L"HeadroomCentiDb")) c.headroomDb = *v / 100.0;
    if (auto v = reg::ReadDword(k, L"PeakLimiter")) c.peakLimiter = *v != 0;
    if (auto v = reg::ReadDword(k, L"Warmth")) c.tone.warmth = *v != 0;
    if (auto v = reg::ReadDword(k, L"WarmthCharacter")) c.tone.warmthType = static_cast<dsp::WarmthType>(*v);
    if (auto v = reg::ReadDword(k, L"WarmthAmountPercent")) c.tone.warmthAmount = *v / 100.0;
    if (auto v = reg::ReadDword(k, L"OutputMode")) c.outputMode = static_cast<OutputMode>(*v);
    if (auto v = reg::ReadDword(k, L"DsdMultiple")) c.dsdMultiple = *v;
    if (auto v = reg::ReadDword(k, L"DsdHighLevel")) c.dsdHighLevel = *v != 0;
    if (auto v = reg::ReadDword(k, L"DsdLookAhead")) c.dsdLookAhead = static_cast<dsp::LookAhead>(*v);
    if (auto v = reg::ReadString(k, L"AsioDriver")) c.asioDriver = *v;
    if (auto v = reg::ReadDword(k, L"BufferMs")) c.bufferMs = *v;
    if (auto v = reg::ReadDword(k, L"PeriodMs")) c.periodMs = *v;
    if (auto v = reg::ReadDword(k, L"Dither")) c.dither = *v != 0;
    if (auto v = reg::ReadDword(k, L"SwitchDefault")) c.switchDefault = *v != 0;
    if (auto v = reg::ReadDword(k, L"Bypass")) c.bypass = *v != 0;
    if (auto v = reg::ReadString(k, L"PreviousDefault")) c.previousDefaultId = *v;
    if (auto v = reg::ReadString(k, L"SourceName")) c.sourceName = *v;
    if (auto v = reg::ReadString(k, L"OutputName")) c.outputName = *v;
    c.Validate();
    return c;
}

HRESULT SaveBridgeConfig(const BridgeConfig& in) {
    BridgeConfig c = in;
    c.Validate();
    auto key = reg::Create(HKEY_CURRENT_USER, kRegBridge, KEY_SET_VALUE);
    if (!key) return key.error().hr;
    const HKEY k = key->Get();
    HRESULT hr = S_OK;
    auto dword = [&](const wchar_t* n, DWORD v) {
        if (SUCCEEDED(hr)) hr = reg::WriteDword(k, n, v);
    };
    auto str = [&](const wchar_t* n, const std::wstring& v) {
        if (SUCCEEDED(hr)) hr = reg::WriteString(k, n, v);
    };
    str(L"Source", c.sourceId);
    str(L"Output", c.outputId);
    dword(L"OutputRate", c.outputRate);
    dword(L"OutputBits", c.outputBits);
    dword(L"OutputValidBits", c.outputValidBits);
    dword(L"Quality", static_cast<DWORD>(c.quality));
    dword(L"CustomAttenuationCentiDb", static_cast<DWORD>(std::lround(c.custom.attenuationDb * 100.0)));
    dword(L"CustomPassbandPpm", static_cast<DWORD>(std::lround(c.custom.passband * 1e6)));
    dword(L"FilterPhase", static_cast<DWORD>(c.filterPhase));
    dword(L"Apodizing", c.apodizing ? 1u : 0u);
    dword(L"FilterResponse", static_cast<DWORD>(c.filterResponse));
    dword(L"FilterDesign", static_cast<DWORD>(c.filterDesign));
    dword(L"HeadroomCentiDb", static_cast<DWORD>(std::lround(c.headroomDb * 100.0)));
    dword(L"PeakLimiter", c.peakLimiter ? 1u : 0u);
    dword(L"Warmth", c.tone.warmth ? 1u : 0u);
    dword(L"WarmthCharacter", static_cast<DWORD>(c.tone.warmthType));
    dword(L"WarmthAmountPercent", static_cast<DWORD>(std::lround(c.tone.warmthAmount * 100.0)));
    dword(L"OutputMode", static_cast<DWORD>(c.outputMode));
    dword(L"DsdMultiple", c.dsdMultiple);
    dword(L"DsdHighLevel", c.dsdHighLevel ? 1u : 0u);
    dword(L"DsdLookAhead", static_cast<DWORD>(c.dsdLookAhead));
    str(L"AsioDriver", c.asioDriver);
    dword(L"BufferMs", c.bufferMs);
    dword(L"PeriodMs", c.periodMs);
    dword(L"Dither", c.dither ? 1u : 0u);
    dword(L"SwitchDefault", c.switchDefault ? 1u : 0u);
    dword(L"Bypass", c.bypass ? 1u : 0u);
    return hr;
}

HRESULT SaveBridgePreviousDefault(const std::wstring& id) {
    auto key = reg::Create(HKEY_CURRENT_USER, kRegBridge, KEY_SET_VALUE);
    if (!key) return key.error().hr;
    return id.empty() ? reg::DeleteValue(key->Get(), L"PreviousDefault") : reg::WriteString(key->Get(), L"PreviousDefault", id);
}

HRESULT SaveBridgeBypass(bool bypass) {
    auto key = reg::Create(HKEY_CURRENT_USER, kRegBridge, KEY_SET_VALUE);
    if (!key) return key.error().hr;
    return reg::WriteDword(key->Get(), L"Bypass", bypass ? 1u : 0u);
}

HRESULT SaveBridgeDeviceNames(const std::wstring& sourceName, const std::wstring& outputName) {
    auto key = reg::Create(HKEY_CURRENT_USER, kRegBridge, KEY_SET_VALUE);
    if (!key) return key.error().hr;
    HRESULT hr = reg::WriteString(key->Get(), L"SourceName", sourceName);
    if (SUCCEEDED(hr)) hr = reg::WriteString(key->Get(), L"OutputName", outputName);
    return hr;
}

HRESULT SaveBridgeDeviceIds(const std::wstring& sourceId, const std::wstring& outputId) {
    auto key = reg::Create(HKEY_CURRENT_USER, kRegBridge, KEY_SET_VALUE);
    if (!key) return key.error().hr;
    HRESULT hr = reg::WriteString(key->Get(), L"Source", sourceId);
    if (SUCCEEDED(hr)) hr = reg::WriteString(key->Get(), L"Output", outputId);
    return hr;
}

std::wstring NormalizeEndpointName(std::wstring_view name) {
    std::wstring out(name);
    for (size_t open = out.find(L'('); open != std::wstring::npos; open = out.find(L'(', open + 1)) {
        size_t i = open + 1;
        while (i < out.size() && iswdigit(out[i])) ++i;
        if (i > open + 1 && i + 1 < out.size() && out[i] == L'-' && out[i + 1] == L' ') out.erase(open + 1, i + 2 - (open + 1));
    }
    return out;
}

std::wstring DataDirectory() {
    wchar_t base[MAX_PATH]{};
    if (!::GetEnvironmentVariableW(L"LOCALAPPDATA", base, MAX_PATH)) return {};
    std::wstring dir = std::wstring(base) + L"\\dgmod";
    ::CreateDirectoryW(dir.c_str(), nullptr);
    return dir;
}

std::wstring BridgeLogPath() {
    const std::wstring dir = DataDirectory();
    return dir.empty() ? std::wstring() : dir + L"\\bridge.log";
}

void AppendBridgeLog(const std::wstring& text) {
    const std::wstring path = BridgeLogPath();
    if (path.empty()) return;
    WIN32_FILE_ATTRIBUTE_DATA attr{};
    if (::GetFileAttributesExW(path.c_str(), GetFileExInfoStandard, &attr) && attr.nFileSizeLow > (1u << 20))
        ::MoveFileExW(path.c_str(), (path.substr(0, path.size() - 4) + L".1.log").c_str(), MOVEFILE_REPLACE_EXISTING);
    SYSTEMTIME st{};
    ::GetLocalTime(&st);
    const std::string line = Narrow(std::format(L"{:04}-{:02}-{:02} {:02}:{:02}:{:02}.{:03} {}\r\n", st.wYear, st.wMonth,
                                                st.wDay, st.wHour, st.wMinute, st.wSecond, st.wMilliseconds, text));
    UniqueHandle f(::CreateFileW(path.c_str(), FILE_APPEND_DATA, FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr, OPEN_ALWAYS,
                                 FILE_ATTRIBUTE_NORMAL, nullptr));
    if (!f) return;
    DWORD written = 0;
    ::WriteFile(f.Get(), line.data(), static_cast<DWORD>(line.size()), &written, nullptr);
}

bool BridgeAutostartEnabled() { return reg::ReadString(HKEY_CURRENT_USER, kRunKey, kBridgeRunValue).has_value(); }

HRESULT SetBridgeAutostart(bool enable, const std::wstring& exePath) {
    auto key = reg::Create(HKEY_CURRENT_USER, kRunKey, KEY_SET_VALUE);
    if (!key) return key.error().hr;
    return enable ? reg::WriteString(key->Get(), kBridgeRunValue, std::format(L"\"{}\"", exePath))
                  : reg::DeleteValue(key->Get(), kBridgeRunValue);
}

}  // namespace dgmod
