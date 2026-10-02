#pragma once

// dgmod bridge engine: captures the mix of a source render endpoint (a virtual cable) through WASAPI loopback,
// oversamples it with the ASRC resampler and renders it to the DAC in WASAPI exclusive mode (PCM or DoP), or as native
// DSD through the DAC's ASIO driver.

#include "bridge/AsioOutput.h"
#include "bridge/Declicker.h"
#include "bridge/DriftController.h"
#include "bridge/FrameFifo.h"
#include "bridge/LevelGuard.h"
#include "bridge/SampleWriter.h"
#include "bridge/SlipCopier.h"
#include "common/BridgeConfig.h"
#include "common/BridgeStatus.h"
#include "common/Win.h"
#include "dsp/DeltaSigma.h"
#include "dsp/Oversampler.h"
#include "dsp/PeakLimiter.h"
#include "dsp/Tone.h"
#include "plugins/PluginHost.h"

#include <audioclient.h>
#include <mmdeviceapi.h>

#include <atomic>
#include <mutex>
#include <string>
#include <thread>

namespace dgmod::bridge {

class DsdJobThread;

class BridgeEngine {
public:
    // Runs until `stopEvent` is signaled. `reloadEvent` (auto-reset) restarts the pipeline if the configuration changed.
    // Restores the previous default playback device on exit. `plugins` (optional) runs the plug-in chain at the source
    // rate, after the tone stages.
    void Run(HANDLE stopEvent, HANDLE reloadEvent, BridgeStatusMapping* status, plugins::PluginHost* plugins = nullptr);

private:
    enum class SessionEnd { Stop, Reload, Retry };

    SessionEnd RunSession(const BridgeConfig& cfg, HANDLE stopEvent, HANDLE reloadEvent);
    Result<void> OpenCapture(const BridgeConfig& cfg);
    Result<void> OpenRender(const BridgeConfig& cfg);
    // Native DSD: opens the ASIO driver of the DAC instead of its WASAPI endpoint.
    Result<void> OpenAsio(const BridgeConfig& cfg);
    // Publishes every exclusive format the DAC accepts (the GUI cannot probe it while the bridge holds it).
    void PublishOutputFormats(const BridgeConfig& cfg);
    // Native DSD: compares the driver's measured buffer switch interval with the buffer size (once per session).
    void CheckAsioTiming();
    void CaptureThread();
    void RenderThread();
    void Fail(const std::wstring& message);
    void PublishStatus();
    void SetState(BridgeState s, const std::wstring& message = {});
    bool ConfigChanged(BridgeConfig& current);
    // Hands new tone settings to the render thread (applied at its next period) and to the status.
    void PostTone(const dsp::ToneSettings& tone);
    void PublishTone(const dsp::ToneSettings& tone);
    // Re-finds configured endpoints that are gone (new endpoint ID after a USB re-plug) by their saved names.
    void RebindDevices(BridgeConfig& cfg);
    [[nodiscard]] uint32_t MaxInputFrames() const;
    // Converts one period of float output (`out`, channels_ interleaved) into the device buffer: PCM, or DSD through
    // the modulator for DoP and native DSD (queued for the ASIO driver). Returns the number of clipped samples. `out`
    // may be modified.
    uint64_t WritePeriod(std::vector<float>& out, BYTE* data);
    // Native DSD, at the start of a period: modulates its held head (the last fadeFrames_ frames of the period before)
    // on the head thread while the render thread runs the DSP chain.
    void BeginNative();
    // Native DSD: modulates the rest of the period (scaled PCM), queues it and has its emergency fade-out rendered on
    // the spare thread (attached to the queued buffer when done).
    void QueueNative(const std::vector<float>& out);
    void FillHead();  // modPcm_ head: the held frames with the fade-in gain
    [[nodiscard]] float FadeIn(uint64_t p) const;
    [[nodiscard]] float FadeOut(uint32_t k) const;
    // Idle period: digital silence, or modulated silence for DSD (a DoP DAC must keep receiving DoP markers).
    void WriteIdle(std::vector<float>& out, BYTE* data);
    // Replaces non-finite output samples by silence and resets the stateful stages that produced them.
    void GuardOutput(std::vector<float>& out);

    BridgeStatusMapping* status_ = nullptr;
    plugins::PluginHost* plugins_ = nullptr;
    BridgeStatusData base_{};  // static part of the status for the current session

    // Session objects.
    ComPtr<IMMDeviceEnumerator> enumerator_;
    ComPtr<IAudioClient> captureClient_;
    ComPtr<IAudioCaptureClient> capture_;
    ComPtr<IAudioClient> renderClient_;
    ComPtr<IAudioRenderClient> render_;
    UniqueHandle renderEvent_;
    UniqueHandle quitEvent_;    // tells the worker threads to leave
    UniqueHandle failEvent_;    // a worker thread hit a fatal error
    UniqueHandle drainedEvent_; // the render thread faded out after stopping_ was set
    std::atomic<bool> stopping_{false};
    std::wstring failMessage_;
    std::atomic<bool> failed_{false};

    uint32_t captureChannels_ = 0;
    uint32_t inRate_ = 0, outRate_ = 0;
    uint32_t channels_ = 0;        // channels carried through the FIFO and the resampler
    uint32_t deviceChannels_ = 0;  // DAC channels
    uint32_t bufferFrames_ = 0;    // exclusive-mode period in frames
    uint32_t targetFrames_ = 0;
    float gain_ = 1.0f;
    FrameFifo fifo_;
    dsp::Oversampler resampler_;
    SlipCopier slip_;
    bool bypass_ = false;  // pass-through session (DAC at the source rate)
    bool dop_ = false;     // DSD over PCM session (outRate_ is the DoP frame rate)
    bool native_ = false;  // native DSD session through ASIO (outRate_ is the DSD rate / 16)
    [[nodiscard]] bool Dsd() const { return dop_ || native_; }
    AsioDsdOutput asio_;
    // Native DSD click protection (render thread): the ASIO epoch the modulator stream belongs to, the fade-in
    // position after the driver ran dry (fadeFrames_ = none running), and the emergency fade-out rendered with every
    // period by a spare encoder continuing from the state the period leaves. The PCM is modulated fadeFrames_ late
    // (holdPcm_): the held frames are the real signal the emergency fade-out of the period before fades.
    uint64_t asioEpoch_ = 0;
    uint32_t fadeFrames_ = 0, fadeInPos_ = 0;
    dsp::DsdEncoder dsdSpare_;
    std::vector<float> holdPcm_, modPcm_, emergencyPcm_;
    std::vector<uint16_t> emergencyWords_;
    // Worker threads of the render thread (alive while it runs): the head of each period, and the emergency fade-out
    // of the period queued last (spareSeq_, spareTail_ frames).
    DsdJobThread* headJob_ = nullptr;
    DsdJobThread* spareJob_ = nullptr;
    bool headPending_ = false;  // BeginNative started the head of the period being rendered
    uint64_t spareSeq_ = 0;
    uint32_t spareTail_ = 0;
    bool asioChecked_ = false; // main thread: the switch interval was checked this session
    std::wstring asioTimingError_;  // the driver's buffers do not match their declared size: native DSD is refused
    dsp::DsdEncoder dsd_;
    std::vector<uint16_t> dsdWords_;
    float dsdScale_ = 0.5f;  // modulation depth for PCM full scale
    dsp::PeakLimiter limiter_;
    dsp::WarmthEq warmthEq_;   // at the source rate, before the resampler
    dsp::Warmth warmth_;       // harmonics at the output rate, before the limiter
    std::mutex toneMutex_;
    dsp::ToneSettings pendingTone_;
    dsp::ToneSettings tone_;   // render thread: settings in use
    Splicer inSplice_;         // render thread: transitions of the resampler input
    Splicer captureSplice_;    // capture thread: transitions at gaps in the captured stream
    std::atomic<bool> toneDirty_{false};
    bool limiterOn_ = false;
    DriftController drift_;
    ArrivalClock arrival_;
    LevelCalibrator calibrator_;  // render thread
    double qpcToSec_ = 0;
    SampleWriter writer_;

    // Live counters (render/capture threads -> status).
    std::atomic<BridgeState> state_{BridgeState::Stopped};
    std::atomic<uint64_t> framesOut_{0}, underruns_{0}, overruns_{0}, rebuffers_{0}, clipped_{0}, discontinuities_{0};
    std::atomic<uint64_t> resyncs_{0}, droppedFrames_{0};
    std::atomic<uint64_t> limitedFrames_{0}, modulatorResets_{0}, modulatorClips_{0};
    std::atomic<uint64_t> declicks_{0}, badSamples_{0}, declickGaps_{0}, fades_{0}, lastDeclick_{0};
    // Counts one smoothed glitch (any thread).
    void CountDeclick() {
        declicks_.fetch_add(1, std::memory_order_relaxed);
        lastDeclick_.store(FileTimeNow(), std::memory_order_relaxed);
    }
    std::atomic<uint64_t> lateWakeups_{0}, slowPeriods_{0}, maxWakeGap_{0};
    // Last counter values reported to the log (main thread).
    struct GlitchSnapshot {
        uint64_t underruns = 0, resyncs = 0, overruns = 0, discontinuities = 0, modulatorResets = 0, late = 0, slow = 0;
        uint64_t badSamples = 0, lateSwitches = 0, missingEmergencies = 0;
    } logged_;
    uint64_t sessionMaxWakeGap_ = 0;
    void LogGlitches();
    std::atomic<float> limiterMinGain_{1.0f};
    std::atomic<uint64_t> cpuTicks_{0}, cpuCalls_{0}, cpuMax_{0};
    std::atomic<double> ppm_{0}, fifoFrames_{0};
    std::atomic<bool> driftLocked_{false};
    std::atomic<double> levelBias_{0};
    std::atomic<float> peakIn_[2]{}, peakOut_[2]{};
    uint64_t lastCalls_ = 0, lastTicks_ = 0;
    double qpcToUs_ = 0;
    std::wstring message_;
};

}  // namespace dgmod::bridge
