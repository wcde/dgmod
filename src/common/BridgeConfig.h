#pragma once

// Configuration of the dgmod bridge (HKCU\Software\dgmod): applications play into a source endpoint (a virtual
// cable) at its mix rate; the bridge captures that mix, oversamples it and renders it to the output endpoint in
// exclusive mode.

#include "common/Win.h"
#include "dsp/DeltaSigma.h"
#include "dsp/Quality.h"
#include "dsp/Tone.h"

#include <string>

namespace dgmod {

inline constexpr wchar_t kRegBridge[] = L"Software\\dgmod";  // HKCU
inline constexpr wchar_t kBridgeExeName[] = L"dgmod-bridge.exe";
inline constexpr wchar_t kBridgeRunValue[] = L"dgmod";
inline constexpr wchar_t kBridgeStopEventName[] = L"Local\\dgmod.Bridge.Stop";
inline constexpr wchar_t kBridgeInstanceMutexName[] = L"Local\\dgmod.Bridge.Instance";
// The status block is named per layout version (".v<kBridgeStatusVersion>"): builds with different layouts never share
// a block, whose size is fixed by whoever creates it first.
inline constexpr wchar_t kBridgeStatusMappingPrefix[] = L"Local\\dgmod.Bridge.Status";

// PCM: oversampled PCM at the output rate. DoP: DSD made by dgmod's own delta-sigma modulator, sent as DSD over PCM
// (DoP v1.1) so the DAC's interpolation filter and modulator are bypassed. DsdNative: the same DSD stream sent as raw
// bits through the DAC's ASIO driver (no DoP markers; higher rates than DoP fits into the PCM formats of WASAPI).
enum class OutputMode : uint32_t { Pcm = 0, Dop = 1, DsdNative = 2 };

// DoP frame rate (PCM rate carrying the DSD stream) for DSD64/128/256; the DSD bit rate is 16 times higher. The DSP
// chain runs at this rate for native DSD too (the modulator turns every frame into one 16-bit word per channel).
constexpr uint32_t DopFrameRate(uint32_t dsdMultiple) { return 44100u * dsdMultiple / 16u; }
inline constexpr uint32_t kDsdMultiples[] = {256, 128, 64};
// Native DSD rates, highest first. "Highest available" stops at DSD512: DSD1024 costs about half a CPU core.
inline constexpr uint32_t kNativeDsdMultiples[] = {1024, 512, 256, 128, 64};
inline constexpr uint32_t kNativeDsdAutoMax = 512;
// Bit of a native DSD rate in the masks the bridge publishes (DSD64 = bit 0 ... DSD1024 = bit 4).
constexpr uint32_t DsdMaskBit(uint32_t dsdMultiple) {
    uint32_t bit = 0;
    for (uint32_t m = dsdMultiple / 64; m > 1; m >>= 1) ++bit;
    return 1u << bit;
}

struct BridgeConfig {
    std::wstring sourceId;           // endpoint the applications play to (virtual cable)
    std::wstring outputId;           // DAC, opened in exclusive mode
    uint32_t outputRate = 0;         // 0 = highest rate the DAC accepts
    uint32_t outputBits = 32;        // container bits: 16, 24 or 32
    uint32_t outputValidBits = 24;   // 16, 24 or 32
    dsp::QualityPreset quality = dsp::QualityPreset::High;
    dsp::FilterSpec custom{140.0, 0.96};
    dsp::FilterPhase filterPhase = dsp::FilterPhase::Linear;
    bool apodizing = false;          // stop band below the source Nyquist (removes the recording's filter ringing)
    dsp::FilterResponse filterResponse = dsp::FilterResponse::Sharp;  // roll-off: sharp, slow, NOS or Gaussian
    dsp::FilterDesign filterDesign = dsp::FilterDesign::Kaiser;        // sharp roll-off: Kaiser or equiripple
    double headroomDb = 0.0;         // 0..12
    bool peakLimiter = true;         // true-peak limiter on the oversampled signal instead of clipping
    dsp::ToneSettings tone;          // analog warmth; applied live, without restarting the session
    OutputMode outputMode = OutputMode::Pcm;
    uint32_t dsdMultiple = 0;        // DSD rate / 44.1 kHz: 64..256 (DoP), 64..1024 (native); 0 = highest the DAC accepts
    bool dsdHighLevel = false;       // DSD: PCM full scale -> 71 % modulation instead of 50 % (SACD reference)
    dsp::LookAhead dsdLookAhead = dsp::LookAhead::Off;  // DSD: pruned tree search of the modulator (up to DSD256)
    std::wstring asioDriver;         // native DSD: ASIO driver name; empty = the one matching the output device
    uint32_t bufferMs = 20;          // FIFO target between capture and render (latency vs. robustness)
    uint32_t periodMs = 0;           // output period; 0 = automatic (5 ms or the device minimum, DoP 10 ms, ASIO: the
                                     // driver's buffer setting)
    bool dither = true;              // TPDF dither when writing 16/24-bit PCM
    bool switchDefault = true;       // make the source the default playback device while running
    bool bypass = false;             // no oversampling: DAC at the source rate, samples passed through unchanged
    std::wstring previousDefaultId;  // restored when the bridge stops (managed by the bridge)
    // Friendly names of the two endpoints when they last opened (managed by the bridge). A device plugged into another
    // USB port becomes a new endpoint with a new ID; the bridge then finds it again by name.
    std::wstring sourceName, outputName;

    [[nodiscard]] dsp::FilterSpec Spec() const {
        return dsp::ShapeSpec(dsp::SpecFor(quality, custom), filterPhase, apodizing, filterResponse, filterDesign);
    }
    [[nodiscard]] bool Dop() const { return outputMode == OutputMode::Dop; }
    [[nodiscard]] bool NativeDsd() const { return outputMode == OutputMode::DsdNative; }
    [[nodiscard]] bool Dsd() const { return Dop() || NativeDsd(); }
    void Validate();
    friend bool operator==(const BridgeConfig&, const BridgeConfig&) = default;
};

BridgeConfig LoadBridgeConfig();
HRESULT SaveBridgeConfig(const BridgeConfig& c);
// Only the value the bridge itself maintains.
HRESULT SaveBridgePreviousDefault(const std::wstring& id);
HRESULT SaveBridgeBypass(bool bypass);
HRESULT SaveBridgeDeviceNames(const std::wstring& sourceName, const std::wstring& outputName);
HRESULT SaveBridgeDeviceIds(const std::wstring& sourceId, const std::wstring& outputId);

// Endpoint name without the instance number Windows adds to duplicates: "Speakers (2- FiiO K9)" -> "Speakers (FiiO K9)".
std::wstring NormalizeEndpointName(std::wstring_view name);

// %LOCALAPPDATA%\dgmod (created if missing; empty if LOCALAPPDATA is not set) and the bridge log in it.
std::wstring DataDirectory();
std::wstring BridgeLogPath();
// Appends a time-stamped line to the bridge log (any thread; rotated at 1 MB).
void AppendBridgeLog(const std::wstring& text);

// Autostart at logon (HKCU\...\Run).
bool BridgeAutostartEnabled();
HRESULT SetBridgeAutostart(bool enable, const std::wstring& exePath);

}  // namespace dgmod
