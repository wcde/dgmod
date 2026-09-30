#pragma once

// Live status of the dgmod bridge, published through a session-local shared memory block.

#include "common/Win.h"

#include <atomic>
#include <cstdint>
#include <cstring>
#include <string_view>
#include <vector>

namespace dgmod {

inline constexpr uint32_t kBridgeStatusMagic = 0x4D474744;  // "DGGM"
inline constexpr uint32_t kBridgeStatusVersion = 9;

// An exclusive-mode format of the output device, probed by the bridge before it opens the device (while the bridge
// holds the device, nobody else can probe it).
struct BridgeFormat {
    uint32_t rate = 0;
    uint16_t bits = 0, validBits = 0;
};
inline constexpr size_t kBridgeMaxFormats = 48;

// A plug-in of the chain as the bridge's plug-in host sees it.
enum class PluginRunState : uint32_t {
    Loading = 0,      // not loaded yet
    Ready = 1,        // loaded and active at the session format
    Idle = 2,         // loaded, not active (no session yet)
    Failed = 3,       // could not be loaded or activated (message says why)
    Crashed = 4,      // raised an exception: no longer called
    Quarantined = 5,  // crashed the bridge while loading: not loaded until the user retries
};
inline constexpr uint32_t kPluginFlagEditor = 1;      // has an editor
inline constexpr uint32_t kPluginFlagEditorOpen = 2;  // its editor window is open
inline constexpr uint32_t kPluginFlagEnabled = 4;     // not bypassed
inline constexpr uint32_t kPluginFlagProcessing = 8;  // in the running chain
inline constexpr uint32_t kBridgeMaxPlugins = 16;

struct BridgePluginStatus {
    uint32_t id = 0;
    PluginRunState state = PluginRunState::Loading;
    uint32_t flags = 0;
    uint32_t latencyFrames = 0;
    uint32_t inputs = 0, outputs = 0;  // channels of its main buses
    float cpuUs = 0, cpuMaxUs = 0;     // processing time per period (average, peak) since the previous status
    wchar_t name[64]{};
    wchar_t message[160]{};
};

enum class BridgeState : uint32_t { Stopped = 0, Starting = 1, Buffering = 2, Playing = 3, Waiting = 4, Error = 5 };

struct BridgeStatusData {
    uint32_t pid = 0;
    BridgeState state = BridgeState::Stopped;
    wchar_t sourceName[64]{};
    wchar_t outputName[64]{};
    uint32_t inRate = 0, outRate = 0, channels = 0;
    uint32_t outBits = 0, outValidBits = 0;
    uint32_t periodFrames = 0;
    uint32_t taps = 0, tablePhases = 0;
    double periodMs = 0, filterLatencyMs = 0;
    double fifoMs = 0, targetMs = 0, driftPpm = 0, totalLatencyMs = 0;
    uint32_t driftLocked = 0;  // the drift controller has settled on the clock ratio
    uint32_t bypass = 0;       // pass-through at the source rate (no oversampling)
    uint32_t dop = 0;          // DSD over PCM: outRate is the DoP frame rate, dsdRate the DSD bit rate
    uint32_t nativeDsd = 0;    // native DSD through ASIO: outRate is dsdRate / 16 (the rate of the DSP chain)
    uint32_t dsdRate = 0;
    wchar_t asioDriver[64]{};  // native DSD: ASIO driver in use (or the last one opened)
    uint32_t asioDsdMask = 0;  // native DSD rates that driver accepts (DsdMaskBit), 0 = not probed
    uint32_t asioBufferSamples = 0;  // ASIO buffer size in DSD samples
    double asioSwitchMs = 0;         // measured interval of the driver's buffer switches (0 = not measured yet)
    double deviceLatencyMs = 0;      // latency the output driver reports beyond one period (ASIO)
    uint64_t lateSwitches = 0;       // ASIO buffer switches that found the queue empty (covered by a fade to silence)
    uint32_t filterPhase = 0, apodizing = 0;  // dsp::FilterPhase (as used: NOS is always linear)
    uint32_t filterResponse = 0;              // dsp::FilterResponse (as used: NOS falls back to slow below the source rate)
    uint32_t filterDesign = 0;                // dsp::FilterDesign (as used: equiripple needs the two-stage cascade)
    uint32_t interpTaps = 0;                  // two-stage cascade: taps of the drift-following interpolator (0 = one stage)
    double dsdNoiseBandHz = 0;                // DSD: band the modulator keeps its noise out of
    uint32_t dsdLookAheadPaths = 0;           // DSD: look-ahead paths of the modulator (0 = off)
    uint32_t dsdLookAheadDepth = 0;           // DSD: look-ahead depth in bits
    uint32_t limiter = 0;      // true-peak limiter active
    uint32_t warmth = 0;       // 0 = off, else 1 + dsp::WarmthType
    float warmthAmount = 0;    // 0..1
    double limiterMs = 0;      // its look-ahead delay
    float limiterMinGain = 1;  // lowest limiter gain since the previous status (1 = not limiting)
    uint64_t limitedFrames = 0;
    uint64_t modulatorResets = 0;  // DSD modulator run-aways (the loop was reset)
    uint64_t modulatorClips = 0;   // DSD modulator samples held by its overload protection (no reset needed)
    uint64_t declicks = 0;         // glitches (underrun, re-sync, source gap) smoothed into a transition
    uint64_t declickGaps = 0;      // of which gaps in the captured source (discontinuity, capture overrun)
    uint64_t fades = 0;            // fade-ins/outs at playback start/stop and silent <-> sounding source edges
    uint64_t lastDeclickTime = 0;  // FILETIME of the latest glitch smoothed or invalid sample replaced (0 = none)
    uint64_t badSamples = 0;       // non-finite / out-of-range samples replaced (source or DSP fault)
    uint64_t lateWakeups = 0;      // render wake-ups more than 1.5 periods apart (the DAC may have run dry)
    uint64_t slowPeriods = 0;      // periods whose processing took longer than 80 % of the period
    double maxWakeGapMs = 0;       // longest gap between render wake-ups since the previous status
    double deliveryMs = 0;             // learned capture delivery delay (packet end -> capture thread)
    double deliveryMinMs = 0, deliveryMaxMs = 0;  // observed range since the previous status
    double levelBiasMs = 0;            // offset removed from the continuous level estimate (vs. the real FIFO)
    uint32_t formatCount = 0;
    BridgeFormat formats[kBridgeMaxFormats]{};
    wchar_t outputId[128]{};   // device the formats belong to
    uint64_t framesOut = 0, underruns = 0, overruns = 0, rebuffers = 0, clipped = 0, discontinuities = 0;
    uint64_t resyncs = 0;          // times the FIFO excess was dropped (render stall, DAC start-up)
    uint64_t droppedFrames = 0;    // input frames dropped by those re-syncs
    double cpuAvgUs = 0, cpuMaxUs = 0;
    float peakIn[2]{}, peakOut[2]{};
    wchar_t message[160]{};
    // Plug-in chain (at the source rate, after the tone stages).
    uint32_t pluginsOn = 0;        // master switch
    uint32_t pluginCount = 0;
    double pluginLatencyMs = 0;    // latency of the plug-ins in the running chain
    float pluginCpuUs = 0;         // their processing time per period (average)
    uint64_t pluginBadSamples = 0; // invalid samples the plug-ins produced (replaced)
    uint64_t pluginSkips = 0;      // periods the chain was passed by while the host changed it
    BridgePluginStatus plugins[kBridgeMaxPlugins]{};
    uint64_t heartbeat = 0;  // FILETIME
    uint64_t startTime = 0;  // FILETIME of the current session
};

// Level history for the live graphs: the render thread appends the peaks of every ~5 ms of audio (at least one entry
// per device period), stamped with the QPC time, so the UI can draw them at its own frame rate.
struct BridgeMeterEntry {
    int64_t qpc = 0;          // QueryPerformanceCounter ticks when the audio was handed to the output device
    float in[2]{}, out[2]{};  // linear L/R peaks of the filter input (source) and of the output
};
inline constexpr uint32_t kBridgeMeterEntries = 4096;  // ~20 s at 5 ms

struct BridgeMeterRing {
    uint64_t written = 0;  // entries ever written; entry i lives in slot i % kBridgeMeterEntries
    BridgeMeterEntry entries[kBridgeMeterEntries]{};
};

struct BridgeStatusBlock {
    uint32_t magic = 0;
    uint32_t version = 0;
    uint32_t seq = 0;
    uint32_t reserved = 0;
    BridgeStatusData data;
    BridgeMeterRing meter;
};

// Seqlock helpers over a plain uint32_t sequence field living in shared memory.
template <class T>
void SeqWrite(uint32_t& seq, T& dst, const T& src) {
    std::atomic_ref<uint32_t> s(seq);
    const uint32_t v = s.load(std::memory_order_relaxed);
    s.store(v + 1, std::memory_order_relaxed);
    std::atomic_thread_fence(std::memory_order_release);
    std::memcpy(static_cast<void*>(&dst), &src, sizeof(T));
    s.store(v + 2, std::memory_order_release);
}

template <class T>
bool SeqRead(const uint32_t& seq, const T& src, T& out) {
    std::atomic_ref<uint32_t> s(const_cast<uint32_t&>(seq));
    for (int attempt = 0; attempt < 64; ++attempt) {
        const uint32_t a = s.load(std::memory_order_acquire);
        if (a & 1u) continue;
        std::memcpy(static_cast<void*>(&out), &src, sizeof(T));
        std::atomic_thread_fence(std::memory_order_acquire);
        if (s.load(std::memory_order_relaxed) == a) return true;
    }
    return false;
}

uint64_t FileTimeNow();

// Copies text into a fixed-size shared-memory field, always null-terminated.
template <size_t N>
void CopyText(wchar_t (&dst)[N], std::wstring_view src) {
    const size_t n = src.size() < N - 1 ? src.size() : N - 1;
    std::memcpy(dst, src.data(), n * sizeof(wchar_t));
    dst[n] = L'\0';
}

class BridgeStatusMapping {
public:
    static Result<BridgeStatusMapping> CreateForWriter();
    static Result<BridgeStatusMapping> OpenForReader();

    void Publish(const BridgeStatusData& d);
    [[nodiscard]] bool Read(BridgeStatusData& out) const;
    // Level history. Single writer (the render thread); the reader appends the entries written since `cursor`
    // (at most the whole ring, overwritten ones dropped) and advances it.
    void PushMeter(const BridgeMeterEntry& e);
    void ReadMeter(uint64_t& cursor, std::vector<BridgeMeterEntry>& out) const;
    explicit operator bool() const { return view_.Get() != nullptr; }

private:
    [[nodiscard]] BridgeStatusBlock* Block() const { return static_cast<BridgeStatusBlock*>(view_.Get()); }
    UniqueHandle mapping_;
    UniqueView view_;
};

}  // namespace dgmod
