#include "bridge/Bridge.h"

#include "common/AsioDrivers.h"
#include "common/AudioFormat.h"
#include "common/PolicyConfig.h"

#include <avrt.h>
#include <xmmintrin.h>

#include <algorithm>
#include <cmath>
#include <format>
#include <limits>
#include <vector>

namespace dgmod::bridge {

namespace {

constexpr uint32_t kCandidateRates[] = {768000, 705600, 384000, 352800, 192000, 176400, 96000, 88200, 48000, 44100};
constexpr REFERENCE_TIME kLoopbackBuffer = 1000000;  // 100 ms
constexpr double kLoopbackLatencyMs = 10.0;          // engine period of the source endpoint (typical)
constexpr float kPcmCeiling = 0.98855309f;           // -0.1 dBFS: limiter ceiling for PCM output

struct BitsOption {
    uint32_t container, valid;
};

uint64_t Qpc() {
    LARGE_INTEGER v;
    ::QueryPerformanceCounter(&v);
    return static_cast<uint64_t>(v.QuadPart);
}

template <class T>
void StoreMax(std::atomic<T>& a, T v) {
    T cur = a.load(std::memory_order_relaxed);
    while (v > cur && !a.compare_exchange_weak(cur, v, std::memory_order_relaxed)) {
    }
}

// Largest |sample| of the first two channels; a mono stream is shown on both.
void Peaks(const float* s, uint32_t frames, uint32_t channels, float peak[2]) {
    const uint32_t used = std::min<uint32_t>(channels, 2);
    for (uint32_t i = 0; i < frames; ++i)
        for (uint32_t c = 0; c < used; ++c) peak[c] = std::max(peak[c], std::abs(s[size_t(i) * channels + c]));
    if (used == 1) peak[1] = peak[0];
}

template <class T>
void StoreMin(std::atomic<T>& a, T v) {
    T cur = a.load(std::memory_order_relaxed);
    while (v < cur && !a.compare_exchange_weak(cur, v, std::memory_order_relaxed)) {
    }
}

WAVEFORMATEXTENSIBLE MakePcm(uint32_t rate, uint32_t channels, uint32_t mask, uint32_t bits, uint32_t valid) {
    WAVEFORMATEXTENSIBLE f = MakeFloatFormat(rate, channels, mask);
    f.Format.wBitsPerSample = static_cast<WORD>(bits);
    f.Format.nBlockAlign = static_cast<WORD>(channels * bits / 8);
    f.Format.nAvgBytesPerSec = rate * f.Format.nBlockAlign;
    f.Samples.wValidBitsPerSample = static_cast<WORD>(valid);
    f.SubFormat.Data1 = WAVE_FORMAT_PCM;
    return f;
}

std::wstring DeviceName(IMMDevice* d) {
    ComPtr<IPropertyStore> props;
    std::wstring name;
    if (SUCCEEDED(d->OpenPropertyStore(STGM_READ, &props))) {
        PROPVARIANT v;
        PropVariantInit(&v);
        if (SUCCEEDED(props->GetValue(kPkeyDeviceFriendlyName, &v)) && v.vt == VT_LPWSTR) name = v.pwszVal;
        PropVariantClear(&v);
    }
    return name;
}

void Log(const std::wstring& text) { AppendBridgeLog(text); }

// Pro Audio MMCSS class for the lifetime of a thread. Also flushes denormals to zero: decaying filter states would
// otherwise slow the thread down by orders of magnitude and make it miss periods.
class MmcssScope {
public:
    MmcssScope() {
        _mm_setcsr(_mm_getcsr() | 0x8040);  // FTZ | DAZ
        DWORD index = 0;
        handle_ = ::AvSetMmThreadCharacteristicsW(L"Pro Audio", &index);
        if (handle_) ::AvSetMmThreadPriority(handle_, AVRT_PRIORITY_HIGH);
    }
    ~MmcssScope() {
        if (handle_) ::AvRevertMmThreadCharacteristics(handle_);
    }
    MmcssScope(const MmcssScope&) = delete;
    MmcssScope& operator=(const MmcssScope&) = delete;

private:
    HANDLE handle_ = nullptr;
};

}  // namespace

void BridgeEngine::SetState(BridgeState s, const std::wstring& message) {
    state_.store(s);
    if (!message.empty()) message_ = message;
}

void BridgeEngine::Fail(const std::wstring& message) {
    if (failed_.exchange(true)) return;
    failMessage_ = message;
    ::SetEvent(failEvent_.Get());
}

bool BridgeEngine::ConfigChanged(BridgeConfig& current) {
    BridgeConfig next = LoadBridgeConfig();
    next.previousDefaultId = current.previousDefaultId;  // maintained by the bridge itself
    next.sourceName = current.sourceName;
    next.outputName = current.outputName;
    return !(next == current);
}

void BridgeEngine::PublishTone(const dsp::ToneSettings& t) {
    base_.warmth = t.warmth ? 1u + static_cast<uint32_t>(t.warmthType) : 0u;
    base_.warmthAmount = static_cast<float>(t.warmthAmount);
}

void BridgeEngine::PostTone(const dsp::ToneSettings& t) {
    {
        std::lock_guard lock(toneMutex_);
        pendingTone_ = t;
    }
    toneDirty_.store(true, std::memory_order_release);
    PublishTone(t);
    Log(std::format(L"tone: warmth {}", t.warmth ? std::format(L"{} {:.0f} %", dsp::WarmthName(t.warmthType), t.warmthAmount * 100.0)
                                               : std::wstring(L"off")));
}

void BridgeEngine::RebindDevices(BridgeConfig& cfg) {
    const auto active = ActiveRenderEndpoints();
    auto isActive = [&](const std::wstring& id) {
        return std::any_of(active.begin(), active.end(), [&](const RenderEndpoint& e) { return e.id == id; });
    };
    bool changed = false;
    auto rebind = [&](std::wstring& id, const std::wstring& name, const std::wstring& other, const wchar_t* role) {
        if (name.empty() || isActive(id)) return;
        const std::wstring wanted = NormalizeEndpointName(name);
        for (const auto& e : active) {
            if (e.id == other || NormalizeEndpointName(e.name) != wanted) continue;
            Log(std::format(L"{} \"{}\" is gone; using \"{}\" ({})", role, name, e.name, e.id));
            id = e.id;
            changed = true;
            return;
        }
    };
    rebind(cfg.outputId, cfg.outputName, cfg.sourceId, L"output");
    rebind(cfg.sourceId, cfg.sourceName, cfg.outputId, L"source");
    if (changed) SaveBridgeDeviceIds(cfg.sourceId, cfg.outputId);
}

void BridgeEngine::Run(HANDLE stopEvent, HANDLE reloadEvent, BridgeStatusMapping* status, plugins::PluginHost* plugins) {
    status_ = status;
    plugins_ = plugins;
    LARGE_INTEGER f;
    ::QueryPerformanceFrequency(&f);
    qpcToUs_ = 1e6 / double(f.QuadPart);
    qpcToSec_ = 1.0 / double(f.QuadPart);
    ComScope com(COINIT_MULTITHREADED);
    Log(L"bridge started");

    BridgeConfig cfg = LoadBridgeConfig();
    if (cfg.bypass) {  // bypass is a temporary A/B switch: a new bridge process starts oversampling
        SaveBridgeBypass(false);
        cfg.bypass = false;
    }
    for (;;) {
        RebindDevices(cfg);
        const SessionEnd end = RunSession(cfg, stopEvent, reloadEvent);
        if (end == SessionEnd::Stop) break;
        if (end == SessionEnd::Retry) {
            // Retry in 3 s; keep the heartbeat alive so the UI shows the waiting state and its reason.
            HANDLE handles[] = {stopEvent, reloadEvent};
            DWORD w = WAIT_TIMEOUT;
            for (int i = 0; i < 6 && w == WAIT_TIMEOUT; ++i) {
                PublishStatus();
                w = ::WaitForMultipleObjects(2, handles, FALSE, 500);
            }
            if (w == WAIT_OBJECT_0) break;
        }
        BridgeConfig next = LoadBridgeConfig();
        if (!(next.NativeDsd() && next.asioDriver == cfg.asioDriver && next.dsdMultiple == cfg.dsdMultiple &&
              next.periodMs == cfg.periodMs))
            asioTimingError_.clear();  // another driver, rate or buffer: native DSD may be tried again
        cfg = std::move(next);
    }

    // Give the default playback device back.
    const BridgeConfig latest = LoadBridgeConfig();
    if (latest.switchDefault && !latest.previousDefaultId.empty() && DefaultRenderEndpointId() == latest.sourceId) {
        if (auto r = SetDefaultRenderEndpoint(latest.previousDefaultId); !r) Log(L"restore default failed: " + r.error().Message());
    }
    SaveBridgePreviousDefault(L"");
    SetState(BridgeState::Stopped, L"Stopped");
    PublishStatus();
    Log(L"bridge stopped");
}

BridgeEngine::SessionEnd BridgeEngine::RunSession(const BridgeConfig& cfg, HANDLE stopEvent, HANDLE reloadEvent) {
    framesOut_ = underruns_ = overruns_ = rebuffers_ = clipped_ = discontinuities_ = resyncs_ = droppedFrames_ = 0;
    limitedFrames_ = modulatorResets_ = modulatorClips_ = lateWakeups_ = slowPeriods_ = maxWakeGap_ = 0;
    declicks_ = badSamples_ = declickGaps_ = fades_ = lastDeclick_ = 0;
    stopping_ = false;
    logged_ = {};
    sessionMaxWakeGap_ = 0;
    limiterMinGain_ = 1.0f;
    cpuTicks_ = cpuCalls_ = cpuMax_ = 0;
    lastCalls_ = lastTicks_ = 0;
    ppm_ = 0;
    driftLocked_ = false;
    levelBias_ = 0;
    fifoFrames_ = 0;
    failed_ = false;
    failMessage_.clear();
    base_ = {};
    base_.pid = ::GetCurrentProcessId();
    base_.startTime = FileTimeNow();
    SetState(BridgeState::Starting, L"Starting");
    PublishStatus();

    auto wait = [&](const std::wstring& why) {
        SetState(BridgeState::Waiting, why);
        PublishStatus();
        Log(L"waiting: " + why);
        return SessionEnd::Retry;
    };
    if (cfg.sourceId.empty() || cfg.outputId.empty()) return wait(L"Choose the source (virtual) and output devices");
    if (cfg.sourceId == cfg.outputId) return wait(L"Source and output must be different devices");
    if (FAILED(::CoCreateInstance(__uuidof(MMDeviceEnumerator), nullptr, CLSCTX_ALL, IID_PPV_ARGS(&enumerator_))))
        return wait(L"MMDeviceEnumerator unavailable");

    if (cfg.switchDefault) {
        const std::wstring current = DefaultRenderEndpointId();
        if (current != cfg.sourceId) {
            if (!current.empty()) SaveBridgePreviousDefault(current);
            if (auto r = SetDefaultRenderEndpoint(cfg.sourceId); !r) Log(L"set default failed: " + r.error().Message());
        }
    }

    if (cfg.NativeDsd() && !cfg.bypass && !asioTimingError_.empty()) return wait(asioTimingError_);
    if (auto r = OpenCapture(cfg); !r) return wait(r.error().Message());
    if (auto r = OpenRender(cfg); !r) {
        asio_.Close();
        captureClient_.Reset();
        capture_.Reset();
        return wait(r.error().Message());
    }

    if (cfg.sourceName != base_.sourceName || cfg.outputName != base_.outputName)
        SaveBridgeDeviceNames(base_.sourceName, base_.outputName);

    // DSP: the oversampling ASRC, or the bit-exact pass-through when bypassed at the source rate.
    bypass_ = cfg.bypass && outRate_ == inRate_;
    if (bypass_) {
        slip_.Configure(channels_);
    } else {
        if (!resampler_.Configure(inRate_, outRate_, cfg.Spec(), channels_, MaxInputFrames()))
            return wait(L"Resampler setup failed");
        resampler_.SetCorrectionPpm(0);
    }
    const dsp::OversamplerPlan* plan = bypass_ ? nullptr : &resampler_.Info();
    const uint32_t maxIn = MaxInputFrames();
    // At least two periods of input, or for long periods one plus a capture packet (10 ms) and delivery jitter (5 ms):
    // right before a render the FIFO can sit a packet below its continuous level.
    const auto periodIn = static_cast<uint32_t>(uint64_t{bufferFrames_} * inRate_ / outRate_) + 1;
    targetFrames_ = std::max<uint32_t>(cfg.bufferMs * inRate_ / 1000, std::min(maxIn, periodIn + inRate_ * 15 / 1000));
    fifo_.Allocate(channels_, std::max<uint32_t>(inRate_, targetFrames_ * 4));
    drift_.Reset(targetFrames_, inRate_);
    arrival_.Configure(double(inRate_));
    calibrator_.Configure(double(inRate_));
    if (!native_) writer_.Configure(base_.outBits, base_.outValidBits, false, cfg.dither && !bypass_, deviceChannels_);  // bypass: no dither
    gain_ = static_cast<float>(std::pow(10.0, -cfg.headroomDb / 20.0));
    // True-peak limiter on the oversampled signal (DSD: at the DSD rate / 16, before the modulator, whose input it
    // also keeps inside the stable range).
    limiterOn_ = !bypass_ && cfg.peakLimiter;
    // Character stages (off in bypass, which stays bit-exact).
    warmthEq_.Configure(channels_, inRate_);
    warmth_.Configure(channels_, outRate_);
    toneDirty_.store(false);
    tone_ = cfg.tone;
    // Click protection: transitions of kDeclickMs at the source rate, on both sides of the FIFO.
    {
        const auto fade = static_cast<uint32_t>(std::lround(kDeclickMs * inRate_ / 1000.0));
        const uint32_t history = std::clamp<uint32_t>(inRate_ / 50, 256, 4096);  // ~20 ms analysed for the continuation
        inSplice_.Configure(channels_, fade, history);
        captureSplice_.Configure(channels_, fade, history);
    }
    base_.warmth = 0;
    if (!bypass_) {
        warmthEq_.Set(cfg.tone.warmth, cfg.tone.warmthType, cfg.tone.warmthAmount);
        warmth_.Set(cfg.tone.warmth, cfg.tone.warmthType, cfg.tone.warmthAmount);
        PublishTone(cfg.tone);
        if (cfg.tone.warmth && outRate_ < 2 * inRate_)
            Log(std::format(L"warmth at {} Hz: harmonics above {} Hz alias (oversample 2x or more for clean harmonics)",
                            outRate_, outRate_ / 2));
    }
    if (limiterOn_) limiter_.Configure(channels_, outRate_, Dsd() ? 1.0f : kPcmCeiling);
    // Plug-ins: activated for this format by the host thread (they run on the resampler input, blocks of up to maxIn).
    if (plugins_ && !bypass_) plugins_->PrepareSession(double(inRate_), maxIn, channels_);
    if (Dsd()) {
        dsd_.Configure(channels_, outRate_, inRate_ * 0.5, bufferFrames_);
        dsdWords_.assign(size_t(bufferFrames_) * channels_, 0);
        dsdScale_ = cfg.dsdHighLevel ? 0.70710678f : 0.5f;
        if (native_) {
            dsdSpare_.Configure(channels_, outRate_, inRate_ * 0.5, bufferFrames_);
            emergencyWords_.assign(dsdWords_.size(), 0);
            modPcm_.assign(dsdWords_.size(), 0.0f);
            emergencyPcm_.assign(dsdWords_.size(), 0.0f);
            fadeFrames_ = std::clamp<uint32_t>(static_cast<uint32_t>(std::lround(kDeclickMs * outRate_ / 1000.0)), 16,
                                               std::max<uint32_t>(bufferFrames_, 17) - 1);
            fadeInPos_ = fadeFrames_;
            holdPcm_.assign(size_t(fadeFrames_) * channels_, 0.0f);
            asioEpoch_ = asio_.Epoch();
        }
    }
    base_.inRate = inRate_;
    base_.outRate = outRate_;
    base_.channels = channels_;
    base_.bypass = bypass_ ? 1u : 0u;
    base_.dop = dop_ ? 1u : 0u;
    base_.nativeDsd = native_ ? 1u : 0u;
    base_.dsdRate = Dsd() ? dsd_.DsdRate() : 0u;
    base_.dsdNoiseBandHz = Dsd() ? dsd_.NoiseBandHz() : 0.0;
    // The shape actually built (NOS is linear phase, and falls back to the slow roll-off without oversampling; the
    // equiripple design needs the two-stage cascade).
    const dsp::FilterSpec used = plan ? plan->filter.spec : dsp::FilterSpec{};
    base_.filterPhase = plan ? static_cast<uint32_t>(used.phase) : 0u;
    base_.filterResponse = plan ? static_cast<uint32_t>(used.response) : 0u;
    base_.filterDesign = plan ? static_cast<uint32_t>(used.design) : 0u;
    base_.apodizing = plan &&
                              (used.response == dsp::FilterResponse::Sharp || used.response == dsp::FilterResponse::Gaussian) &&
                              used.stopband < 1.0
                          ? 1u
                          : 0u;
    base_.limiter = limiterOn_ ? 1u : 0u;
    base_.limiterMs = limiterOn_ ? limiter_.LatencyFrames() * 1000.0 / outRate_ : 0.0;
    if (plan) {
        base_.taps = plan->filter.taps;
        base_.tablePhases = plan->Cascade() ? plan->interp.tablePhases : plan->filter.tablePhases;
        base_.interpTaps = plan->interp.taps;
        base_.filterLatencyMs = plan->LatencySeconds() * 1e3;
    }
    base_.targetMs = targetFrames_ * 1000.0 / inRate_;
    std::wstring dsp = bypass_ ? std::wstring(L"bypass (bit-exact)")
                               : std::format(L"filter {} taps{}, {} roll-off, {} phase{}{}{}", base_.taps,
                                             base_.interpTaps ? std::format(L" at {} Hz + {}-tap interpolator",
                                                                            plan->filter.outRate, base_.interpTaps)
                                                              : std::wstring(),
                                             dsp::ResponseName(used.response), dsp::PhaseName(used.phase),
                                             base_.apodizing ? L" apodizing" : L"",
                                             used.design == dsp::FilterDesign::Equiripple ? L" equiripple" : L"",
                                             limiterOn_ ? L", limiter" : L"");
    if (Dsd())
        dsp += std::format(L", {} DSD{} ({} Hz, modulator order {}, noise shaped to {:.0f} kHz, {} % level)",
                           dop_ ? L"DoP" : L"native", dsd_.DsdRate() / 44100, dsd_.DsdRate(), dsd_.Ntf().order,
                           dsd_.NoiseBandHz() / 1000.0, std::lround(dsdScale_ * 100.0f));
    if (native_) dsp += std::format(L" via ASIO \"{}\"", base_.asioDriver);
    Log(std::format(L"session: {} ({} Hz, {} ch) -> {} ({} Hz, {}/{} bit, {} frames/period), {}, target {:.1f} ms",
                    base_.sourceName, inRate_, captureChannels_, base_.outputName, outRate_, base_.outValidBits,
                    base_.outBits, bufferFrames_, dsp, base_.targetMs));
    if (plugins_ && !bypass_) {
        BridgeStatusData p{};
        plugins_->Snapshot(p);
        uint32_t active = 0;
        for (uint32_t i = 0; i < p.pluginCount; ++i)
            active += (p.plugins[i].flags & kPluginFlagProcessing) && (p.plugins[i].flags & kPluginFlagEnabled) ? 1 : 0;
        if (p.pluginCount)
            Log(std::format(L"plug-ins: {} of {} active{}, latency {:.1f} ms", active, p.pluginCount, p.pluginsOn ? L"" : L" (off)",
                            p.pluginLatencyMs));
    }
    if (cfg.bypass && !bypass_)
        Log(std::format(L"bypass unavailable: the output does not accept {} Hz in exclusive mode", inRate_));

    quitEvent_.Reset(::CreateEventW(nullptr, TRUE, FALSE, nullptr));
    drainedEvent_.Reset(::CreateEventW(nullptr, TRUE, FALSE, nullptr));
    failEvent_.Reset(::CreateEventW(nullptr, TRUE, FALSE, nullptr));
    SetState(BridgeState::Buffering, L"Waiting for audio on the source device");
    std::thread capture([this] { CaptureThread(); });
    std::thread render([this] { RenderThread(); });

    SessionEnd end = SessionEnd::Stop;
    BridgeConfig live = cfg;  // cfg plus the tone settings applied since
    asioChecked_ = false;
    for (;;) {
        HANDLE handles[] = {stopEvent, reloadEvent, failEvent_.Get(), native_ ? asio_.ResetEvent() : nullptr};
        const DWORD w = ::WaitForMultipleObjects(native_ ? 4 : 3, handles, FALSE, 100);
        if (w == WAIT_OBJECT_0 + 3) {  // the ASIO driver wants to be re-opened (rate or buffer changed in its panel)
            Log(L"session: " + asio_.ResetReason());
            end = SessionEnd::Reload;
            break;
        }
        if (w == WAIT_OBJECT_0) {
            end = SessionEnd::Stop;
            break;
        }
        if (w == WAIT_OBJECT_0 + 1) {
            BridgeConfig current = live;
            if (ConfigChanged(current)) {
                // Tone settings alone change live; anything else rebuilds the session.
                BridgeConfig next = LoadBridgeConfig();
                const dsp::ToneSettings tone = next.tone;
                next.tone = live.tone;
                next.previousDefaultId = live.previousDefaultId;
                next.sourceName = live.sourceName;
                next.outputName = live.outputName;
                if (!(next == live)) {
                    end = SessionEnd::Reload;
                    break;
                }
                live.tone = tone;
                if (!bypass_) PostTone(tone);
            }
        } else if (w == WAIT_OBJECT_0 + 2) {
            end = SessionEnd::Retry;
            break;
        }
        if (native_) CheckAsioTiming();
        PublishStatus();
        LogGlitches();
    }
    if (end != SessionEnd::Retry) {
        // Fade out instead of cutting the DAC off mid-signal: the render thread plays the faded tail, lets the filter
        // empty and hands one silent period to the device before the stream is stopped.
        stopping_ = true;
        HANDLE drain[] = {drainedEvent_.Get(), failEvent_.Get()};
        ::WaitForMultipleObjects(2, drain, FALSE, 500);
    }
    ::SetEvent(quitEvent_.Get());
    capture.join();
    render.join();
    asio_.Close();
    if (renderClient_) renderClient_->Stop();
    if (captureClient_) captureClient_->Stop();
    render_.Reset();
    renderClient_.Reset();
    capture_.Reset();
    captureClient_.Reset();
    renderEvent_.Reset();
    if (end == SessionEnd::Retry) {
        Log(L"session failed: " + failMessage_);
        SetState(BridgeState::Error, failMessage_);
    } else {
        SetState(BridgeState::Stopped, end == SessionEnd::Reload ? L"Applying new settings" : L"Stopped");
    }
    PublishStatus();
    return end;
}

Result<void> BridgeEngine::OpenCapture(const BridgeConfig& cfg) {
    ComPtr<IMMDevice> dev;
    if (FAILED(enumerator_->GetDevice(cfg.sourceId.c_str(), &dev))) return dgmod::Fail(E_FAIL, L"Source device not found");
    DWORD state = 0;
    dev->GetState(&state);
    if (state != DEVICE_STATE_ACTIVE)
        return dgmod::Fail(E_FAIL, L"Source device is not active: enable it in the Windows Sound settings");
    base_.sourceName[0] = L'\0';
    CopyText(base_.sourceName, DeviceName(dev.Get()));
    HRESULT hr = dev->Activate(__uuidof(IAudioClient), CLSCTX_ALL, nullptr, reinterpret_cast<void**>(&captureClient_));
    if (FAILED(hr)) return dgmod::Fail(hr, L"Source: Activate");
    WAVEFORMATEX* mix = nullptr;
    if (FAILED(hr = captureClient_->GetMixFormat(&mix))) return dgmod::Fail(hr, L"Source: GetMixFormat");
    StreamFormat mf;
    DescribeFormat(mix, mf);
    if (!mf.IsFloat32()) {
        ::CoTaskMemFree(mix);
        return dgmod::Fail(E_FAIL, L"Source mix format is not float32");
    }
    captureChannels_ = mf.channels;
    inRate_ = mf.sampleRate;
    hr = captureClient_->Initialize(AUDCLNT_SHAREMODE_SHARED, AUDCLNT_STREAMFLAGS_LOOPBACK, kLoopbackBuffer, 0, mix, nullptr);
    ::CoTaskMemFree(mix);
    if (FAILED(hr)) return dgmod::Fail(hr, L"Source: loopback Initialize");
    if (FAILED(hr = captureClient_->GetService(IID_PPV_ARGS(&capture_)))) return dgmod::Fail(hr, L"Source: capture client");
    return {};
}

void BridgeEngine::PublishOutputFormats(const BridgeConfig& cfg) {
    const auto all = SupportedDeviceFormats(cfg.outputId);
    base_.formatCount = 0;
    for (const auto& f : all) {
        if (base_.formatCount == kBridgeMaxFormats) break;
        base_.formats[base_.formatCount++] = {f.sampleRate, static_cast<uint16_t>(f.bits), static_cast<uint16_t>(f.validBits)};
    }
    CopyText(base_.outputId, cfg.outputId);
}

Result<void> BridgeEngine::OpenAsio(const BridgeConfig& cfg) {
    // The WASAPI endpoint stays closed (the ASIO driver talks to the DAC); it still names the DAC for re-binding and
    // for finding its driver, and its formats are published for the PCM and DoP choices in the UI.
    std::wstring endpointName = cfg.outputName;
    if (ComPtr<IMMDevice> dev; SUCCEEDED(enumerator_->GetDevice(cfg.outputId.c_str(), &dev))) {
        DWORD state = 0;
        dev->GetState(&state);
        if (state == DEVICE_STATE_ACTIVE) {
            endpointName = DeviceName(dev.Get());
            PublishOutputFormats(cfg);
        }
    }
    CopyText(base_.outputName, endpointName);
    const auto drivers = ListAsioDrivers();
    const AsioDriverEntry* driver = FindAsioDriver(drivers, cfg.asioDriver, endpointName);
    if (!driver)
        return dgmod::Fail(E_FAIL, drivers.empty() ? std::wstring(L"No ASIO driver is installed: native DSD needs the DAC maker's "
                                                                   L"ASIO driver")
                                    : cfg.asioDriver.empty()
                                        ? std::format(L"No ASIO driver matches \"{}\": choose the DAC's driver", endpointName)
                                        : std::format(L"The ASIO driver \"{}\" is not installed", cfg.asioDriver));
    CopyText(base_.asioDriver, driver->name);
    AsioDsdOutput::Request req;
    req.clsid = driver->clsid;
    req.name = driver->name;
    req.dsdMultiple = cfg.dsdMultiple;
    req.channels = captureChannels_;
    // Automatic: the driver's own buffer setting. The render thread keeps AsioDsdOutput::QueueDepth() buffers queued
    // ahead of the driver.
    req.periodMs = double(cfg.periodMs);
    uint32_t mask = 0;
    auto opened = asio_.Open(req, mask);
    base_.asioDsdMask = mask;
    if (!opened) return std::unexpected(opened.error());
    const AsioDsdOutput::Opened& o = *opened;
    native_ = true;
    dop_ = false;
    deviceChannels_ = o.deviceChannels;
    channels_ = o.channels;
    bufferFrames_ = o.frames;
    outRate_ = DopFrameRate(o.dsdMultiple);
    base_.outBits = base_.outValidBits = 1;
    base_.periodFrames = bufferFrames_;
    base_.periodMs = bufferFrames_ * 1000.0 / outRate_;
    base_.asioBufferSamples = o.bufferSamples;
    // Beyond one period: the driver's own latency, the buffers queued ahead of it and the frames held back for the
    // emergency fade-out.
    base_.deviceLatencyMs = std::max(0.0, o.outputLatencyMs - base_.periodMs) +
                            (asio_.QueueDepth() - 1) * base_.periodMs + std::min(double(kDeclickMs), base_.periodMs);
    Log(std::format(L"asio: \"{}\" ({}), DSD{} native, {} of {} outputs, {} samples/buffer ({:.2f} ms, {} queued ahead), {}, "
                    L"output latency {:.2f} ms, outputReady {}, accepts{}{}{}{}{}",
                    driver->name, o.driverName, o.dsdMultiple, o.channels, o.deviceChannels, o.bufferSamples, base_.periodMs,
                    asio_.QueueDepth(),
                    o.lsbFirst ? L"LSB first" : L"MSB first", o.outputLatencyMs, o.outputReady ? L"yes" : L"no",
                    (mask & DsdMaskBit(64)) ? L" DSD64" : L"", (mask & DsdMaskBit(128)) ? L" DSD128" : L"",
                    (mask & DsdMaskBit(256)) ? L" DSD256" : L"", (mask & DsdMaskBit(512)) ? L" DSD512" : L"",
                    (mask & DsdMaskBit(1024)) ? L" DSD1024" : L""));
    return {};
}

void BridgeEngine::CheckAsioTiming() {
    if (asioChecked_) return;
    const double measured = asio_.MeasuredSwitchSec();
    if (measured <= 0) return;
    asioChecked_ = true;
    base_.asioSwitchMs = measured * 1e3;
    const double ratio = measured * 1e3 / base_.periodMs;
    Log(std::format(L"asio: buffer switches every {:.3f} ms (expected {:.3f} ms)", measured * 1e3, base_.periodMs));
    // A driver that counted the buffer size in bytes instead of DSD samples would switch 8 times slower (and play the
    // unfilled 7/8 of every buffer): stop instead of playing that.
    if (ratio > 1.5 || ratio < 0.67) {
        asioTimingError_ = std::format(L"The ASIO driver switches buffers every {:.1f} ms instead of {:.1f} ms: its DSD buffer "
                                       L"layout is not supported. Use DSD (DoP) with this DAC.",
                                       measured * 1e3, base_.periodMs);
        Fail(asioTimingError_);
    }
}

Result<void> BridgeEngine::OpenRender(const BridgeConfig& cfg) {
    native_ = false;
    dop_ = false;
    if (cfg.NativeDsd() && !cfg.bypass) return OpenAsio(cfg);
    ComPtr<IMMDevice> dev;
    if (FAILED(enumerator_->GetDevice(cfg.outputId.c_str(), &dev))) return dgmod::Fail(E_FAIL, L"Output device not found");
    DWORD state = 0;
    dev->GetState(&state);
    if (state != DEVICE_STATE_ACTIVE) return dgmod::Fail(E_FAIL, L"Output device is not active");
    CopyText(base_.outputName, DeviceName(dev.Get()));
    auto activate = [&](ComPtr<IAudioClient>& client) {
        return dev->Activate(__uuidof(IAudioClient), CLSCTX_ALL, nullptr, reinterpret_cast<void**>(client.ReleaseAndGetAddressOf()));
    };
    HRESULT hr = activate(renderClient_);
    if (FAILED(hr)) return dgmod::Fail(hr, L"Output: Activate");
    WAVEFORMATEX* mix = nullptr;
    StreamFormat mf;
    if (SUCCEEDED(renderClient_->GetMixFormat(&mix))) DescribeFormat(mix, mf);
    ::CoTaskMemFree(mix);
    deviceChannels_ = mf.channels ? mf.channels : 2;
    const uint32_t mask = mf.channelMask;
    channels_ = std::min(captureChannels_, deviceChannels_);

    PublishOutputFormats(cfg);

    // Candidate exclusive-mode formats in order of preference: the source rate when bypassed, else the configured rate
    // or the highest the DAC accepts; configured depth first. DoP: the DoP frame rate of the chosen (or highest) DSD
    // rate with 24+ bits. Drivers may accept a format in IsFormatSupported and still refuse it in Initialize (for
    // example a USB DAC on a port that cannot carry its highest rate), so every accepted format is tried in turn.
    const bool dop = cfg.Dop() && !cfg.bypass;
    std::vector<uint32_t> rates;
    std::vector<BitsOption> options;
    if (dop) {
        for (const uint32_t m : kDsdMultiples)
            if (!cfg.dsdMultiple || cfg.dsdMultiple == m) rates.push_back(DopFrameRate(m));
        options = {{32, 24}, {24, 24}, {32, 32}};
    } else {
        if (cfg.bypass) rates.push_back(inRate_);
        if (cfg.outputRate) rates.push_back(cfg.outputRate);
        else rates.insert(rates.end(), std::begin(kCandidateRates), std::end(kCandidateRates));
        options = {{cfg.outputBits, cfg.outputValidBits}, {32, 24}, {24, 24}, {32, 32}, {16, 16}};
    }
    std::vector<WAVEFORMATEXTENSIBLE> candidates;
    for (const uint32_t rate : rates) {
        for (const BitsOption& b : options) {
            const WAVEFORMATEXTENSIBLE f = MakePcm(rate, deviceChannels_, mask, b.container, b.valid);
            const bool dup = std::any_of(candidates.begin(), candidates.end(), [&](const WAVEFORMATEXTENSIBLE& c) {
                return c.Format.nSamplesPerSec == rate && c.Format.wBitsPerSample == b.container &&
                       c.Samples.wValidBitsPerSample == b.valid;
            });
            if (!dup && renderClient_->IsFormatSupported(AUDCLNT_SHAREMODE_EXCLUSIVE, &f.Format, nullptr) == S_OK)
                candidates.push_back(f);
        }
    }
    const bool found = !candidates.empty();
    if (!found && dop)
        return dgmod::Fail(AUDCLNT_E_UNSUPPORTED_FORMAT,
                            cfg.dsdMultiple ? std::format(L"The output device does not accept DSD{} over PCM ({}, 24-bit) in "
                                                          L"exclusive mode",
                                                          cfg.dsdMultiple, DopFrameRate(cfg.dsdMultiple))
                                            : std::wstring(L"The output device accepts no DoP format (176.4 / 352.8 / 705.6 kHz, "
                                                           L"24-bit) in exclusive mode"));
    if (!found)
        return dgmod::Fail(AUDCLNT_E_UNSUPPORTED_FORMAT,
                            L"The output device accepts none of the exclusive-mode formats tried (is exclusive mode allowed?)");
    dop_ = dop;

    REFERENCE_TIME defPeriod = 0, minPeriod = 0;
    renderClient_->GetDevicePeriod(&defPeriod, &minPeriod);
    // Automatic period: 5 ms (low latency, few wake-ups), within what the device supports.
    // DoP: 10 ms. A period delivered late makes the driver send non-DoP data, and the DAC then drops out of DSD mode
    // for about a second; a longer period leaves more time against scheduling hiccups.
    const REFERENCE_TIME wanted = cfg.periodMs ? REFERENCE_TIME{cfg.periodMs} * 10000 : dop ? 100000 : 50000;
    REFERENCE_TIME basePeriod = std::max<REFERENCE_TIME>(minPeriod, wanted);
    if (!cfg.periodMs && defPeriod > 0) basePeriod = std::min(basePeriod, defPeriod);
    auto label = [](const WAVEFORMATEXTENSIBLE& f) {
        return std::format(L"{} Hz {}/{} bit", f.Format.nSamplesPerSec, f.Samples.wValidBitsPerSample, f.Format.wBitsPerSample);
    };
    WAVEFORMATEXTENSIBLE fmt{};
    std::wstring refused;
    hr = E_FAIL;
    for (const WAVEFORMATEXTENSIBLE& candidate : candidates) {
        fmt = candidate;
        if (FAILED(hr = activate(renderClient_))) return dgmod::Fail(hr, L"Output: Activate");
        REFERENCE_TIME period = basePeriod;
        hr = renderClient_->Initialize(AUDCLNT_SHAREMODE_EXCLUSIVE, AUDCLNT_STREAMFLAGS_EVENTCALLBACK, period, period, &fmt.Format,
                                       nullptr);
        if (hr == AUDCLNT_E_BUFFER_SIZE_NOT_ALIGNED) {
            UINT32 frames = 0;
            renderClient_->GetBufferSize(&frames);
            period = static_cast<REFERENCE_TIME>(10000.0 * 1000.0 * frames / fmt.Format.nSamplesPerSec + 0.5);
            if (FAILED(hr = activate(renderClient_))) return dgmod::Fail(hr, L"Output: Activate (aligned)");
            hr = renderClient_->Initialize(AUDCLNT_SHAREMODE_EXCLUSIVE, AUDCLNT_STREAMFLAGS_EVENTCALLBACK, period, period,
                                           &fmt.Format, nullptr);
        }
        if (SUCCEEDED(hr)) break;
        if (hr == AUDCLNT_E_DEVICE_IN_USE)
            return dgmod::Fail(hr, L"The output device is used exclusively by another application");
        if (hr == AUDCLNT_E_EXCLUSIVE_MODE_NOT_ALLOWED)
            return dgmod::Fail(hr, L"Exclusive mode is not allowed for the output device (Sound settings -> Advanced)");
        refused += std::format(L"{}{} ({})", refused.empty() ? L"" : L", ", label(fmt), HResultText(hr));
    }
    if (FAILED(hr)) {
        Log(L"output refused every format: " + refused);
        return dgmod::Fail(hr, dop ? L"The output device refused every DoP format when opening it (see the log)"
                                    : L"The output device refused every exclusive format when opening it (see the log)");
    }
    if (!refused.empty()) Log(std::format(L"output refused {}; using {}", refused, label(fmt)));
    renderEvent_.Reset(::CreateEventW(nullptr, FALSE, FALSE, nullptr));
    if (FAILED(hr = renderClient_->SetEventHandle(renderEvent_.Get()))) return dgmod::Fail(hr, L"Output: SetEventHandle");
    if (FAILED(hr = renderClient_->GetBufferSize(&bufferFrames_))) return dgmod::Fail(hr, L"Output: GetBufferSize");
    if (FAILED(hr = renderClient_->GetService(IID_PPV_ARGS(&render_)))) return dgmod::Fail(hr, L"Output: render client");
    outRate_ = fmt.Format.nSamplesPerSec;
    base_.outBits = fmt.Format.wBitsPerSample;
    base_.outValidBits = fmt.Samples.wValidBitsPerSample;
    base_.periodFrames = bufferFrames_;
    base_.periodMs = bufferFrames_ * 1000.0 / outRate_;
    return {};
}

void BridgeEngine::CaptureThread() {
    ::SetThreadDescription(::GetCurrentThread(), L"dgmod bridge capture");
    ComScope com(COINIT_MULTITHREADED);
    MmcssScope mmcss;
    UniqueHandle timer(::CreateWaitableTimerExW(nullptr, nullptr, CREATE_WAITABLE_TIMER_HIGH_RESOLUTION, TIMER_ALL_ACCESS));
    if (!timer) timer.Reset(::CreateWaitableTimerExW(nullptr, nullptr, 0, TIMER_ALL_ACCESS));
    LARGE_INTEGER due{};
    due.QuadPart = -20000;  // 2 ms
    ::SetWaitableTimerEx(timer.Get(), &due, 2, nullptr, nullptr, nullptr, 0);

    std::vector<float> tmp(size_t(inRate_) * channels_);
    const auto tmpFrames = static_cast<uint32_t>(tmp.size() / channels_);
    bool lastSilent = true;  // the previous packet was silent (or none came yet)
    bool gap = false;        // frames were lost before the next packet (overrun)
    if (HRESULT hr = captureClient_->Start(); FAILED(hr)) {
        Fail(L"Source: Start failed: " + HResultText(hr));
        return;
    }
    for (;;) {
        HANDLE handles[] = {quitEvent_.Get(), timer.Get()};
        if (::WaitForMultipleObjects(2, handles, FALSE, 1000) == WAIT_OBJECT_0) break;
        for (;;) {
            UINT32 packet = 0;
            HRESULT hr = capture_->GetNextPacketSize(&packet);
            if (FAILED(hr)) {
                Fail(L"Source lost: " + HResultText(hr));
                return;
            }
            if (packet == 0) break;
            BYTE* data = nullptr;
            UINT32 frames = 0;
            DWORD flags = 0;
            UINT64 qpcPosition = 0;  // 100 ns units: when the first frame of the packet was recorded
            if (FAILED(hr = capture_->GetBuffer(&data, &frames, &flags, nullptr, &qpcPosition))) {
                Fail(L"Source read failed: " + HResultText(hr));
                return;
            }
            if (flags & AUDCLNT_BUFFERFLAGS_DATA_DISCONTINUITY) {
                discontinuities_.fetch_add(1, std::memory_order_relaxed);
                gap = true;
            }
            const double received = double(Qpc()) * qpcToSec_;
            const double firstFrameTime = qpcPosition ? double(qpcPosition) * 1e-7 : received - double(frames) / inRate_;
            const bool silentPacket = (flags & AUDCLNT_BUFFERFLAGS_SILENT) || !data;
            // Jumps in the captured stream become transitions: gaps (a source discontinuity, frames lost to an
            // overrun) and the edges between silent and sounding packets (a stream starting or stopping mid-waveform).
            if (gap && !lastSilent) {  // a jump in sounding audio
                CountDeclick();
                declickGaps_.fetch_add(1, std::memory_order_relaxed);
            } else if (silentPacket != lastSilent) {
                fades_.fetch_add(1, std::memory_order_relaxed);
            }
            if (gap || silentPacket != lastSilent) captureSplice_.Splice();
            gap = false;
            lastSilent = silentPacket;
            uint32_t written = 0;
            if (silentPacket && !captureSplice_.Transitioning()) {
                captureSplice_.PassSilence(frames);
                written = fifo_.WriteSilence(frames);
            } else {
                const uint32_t n = std::min(frames, tmpFrames);
                if (silentPacket) {
                    std::fill_n(tmp.begin(), size_t(n) * channels_, 0.0f);
                } else {
                    const auto* src = reinterpret_cast<const float*>(data);
                    for (uint32_t i = 0; i < n; ++i)
                        for (uint32_t c = 0; c < channels_; ++c)
                            tmp[size_t(i) * channels_ + c] = src[size_t(i) * captureChannels_ + c];
                    // Garbage from a misbehaving application (NaN, infinities, absurd levels) must not reach the
                    // filters: NaN would silence them for good, a huge value would be a loud spike.
                    if (const uint32_t bad = SanitizeSamples(tmp.data(), size_t(n) * channels_, kInputMax)) {
                        badSamples_.fetch_add(bad, std::memory_order_relaxed);
                        lastDeclick_.store(FileTimeNow(), std::memory_order_relaxed);
                    }
                    float peak[2]{};
                    for (uint32_t i = 0; i < n; ++i)
                        for (uint32_t c = 0; c < std::min<uint32_t>(channels_, 2); ++c)
                            peak[c] = std::max(peak[c], std::abs(tmp[size_t(i) * channels_ + c]));
                    StoreMax(peakIn_[0], peak[0]);
                    StoreMax(peakIn_[1], peak[1]);
                }
                captureSplice_.Process(tmp.data(), n);
                written = fifo_.Write(tmp.data(), n);
                if (written == n && n < frames) written += fifo_.WriteSilence(frames - n);
            }
            if (written < frames) {
                overruns_.fetch_add(frames - written, std::memory_order_relaxed);
                gap = true;
            }
            arrival_.Publish(fifo_.TotalWritten(), written, firstFrameTime, received);
            capture_->ReleaseBuffer(frames);
        }
    }
}

void BridgeEngine::RenderThread() {
    ::SetThreadDescription(::GetCurrentThread(), L"dgmod bridge render");
    ComScope com(COINIT_MULTITHREADED);
    MmcssScope mmcss;
    const uint32_t maxIn = MaxInputFrames();
    std::vector<float> in(size_t(maxIn) * channels_), out(size_t(bufferFrames_) * channels_);
    const double periodSec = double(bufferFrames_) / outRate_;

    BYTE* data = nullptr;
    // Native DSD: the queue is filled before the driver starts (both buffer halves hold DSD silence until then); once
    // this thread is gone, whatever way it leaves, the callback plays the queue out, the emergency fade, then silence.
    bool asioStarted = false;
    if (!native_) {
        if (SUCCEEDED(render_->GetBuffer(bufferFrames_, &data))) {
            WriteIdle(out, data);
            render_->ReleaseBuffer(bufferFrames_, dop_ ? 0 : AUDCLNT_BUFFERFLAGS_SILENT);
        }
        if (HRESULT hr = renderClient_->Start(); FAILED(hr)) {
            Fail(L"Output: Start failed: " + HResultText(hr));
            return;
        }
    }
    uint64_t lastWake = 0;
    const auto periodTicks = static_cast<uint64_t>(periodSec / qpcToSec_);
    // Level history for the UI graphs: peaks accumulated over at least 5 ms, one entry per period at most.
    const auto meterTicks = static_cast<uint64_t>(0.005 / qpcToSec_);
    BridgeMeterEntry meter{};
    uint64_t meterStart = Qpc();
    LevelGuard guard;
    guard.Configure(targetFrames_, std::max<uint32_t>(targetFrames_, inRate_ / 20));  // re-sync above target + 50 ms
    // After an underrun or on stop the DSP chain keeps running on the faded tail until the filter has emptied (the
    // resampler reports silence), bounded by twice its length plus 50 ms (IIR tails of the warmth stage).
    bool tail = false;
    uint64_t tailFrames = 0;
    const auto tailLimit = static_cast<uint64_t>(
        (2.0 * (bypass_ ? 0.0 : resampler_.Info().LengthSeconds()) + 0.05) * outRate_ + bufferFrames_);
    uint32_t drainedPeriods = 0;
    // Stopping: once the tail has played, one silent period queued behind it lets the device run it out (native DSD:
    // and the driver has taken every queued buffer).
    auto checkDrained = [&] {
        if (stopping_.load(std::memory_order_acquire) && drainedPeriods >= 2 && (!native_ || asio_.Queued() == 0))
            ::SetEvent(drainedEvent_.Get());
    };
    bool again = native_;  // native DSD: render the next period without waiting (the queue has room)
    for (;;) {
        if (!again) {
            HANDLE handles[] = {quitEvent_.Get(), native_ ? asio_.SwitchEvent() : renderEvent_.Get()};
            const DWORD w = ::WaitForMultipleObjects(2, handles, FALSE, 2000);
            if (w == WAIT_OBJECT_0) break;
            if (w == WAIT_TIMEOUT) {
                Fail(L"The output device stopped requesting audio");
                return;
            }
        } else if (::WaitForSingleObject(quitEvent_.Get(), 0) == WAIT_OBJECT_0) {
            break;
        }
        const uint64_t t0 = Qpc();
        if (!again) {
            if (lastWake) {
                const uint64_t gap = t0 - lastWake;
                if (gap > periodTicks * 3 / 2) lateWakeups_.fetch_add(1, std::memory_order_relaxed);
                StoreMax(maxWakeGap_, gap);
            }
            lastWake = t0;
        }
        again = false;
        if (native_) {
            // Full queue, or stopping with the silent periods queued: let the driver take what is there.
            if (asio_.Space() == 0 || (stopping_.load(std::memory_order_acquire) && drainedPeriods >= 2)) {
                if (!asioStarted) {
                    if (auto r = asio_.Start(); !r) {
                        Fail(r.error().Message());
                        return;
                    }
                    asioStarted = true;
                }
                checkDrained();
                continue;
            }
        } else if (HRESULT hr = render_->GetBuffer(bufferFrames_, &data); FAILED(hr)) {
            Fail(L"Output lost: " + HResultText(hr));
            return;
        }
        if (toneDirty_.load(std::memory_order_acquire)) {
            std::unique_lock lock(toneMutex_, std::try_to_lock);  // never wait on the main thread
            if (lock.owns_lock()) {
                toneDirty_.store(false, std::memory_order_relaxed);
                const dsp::ToneSettings t = pendingTone_;
                lock.unlock();
                warmthEq_.Set(t.warmth, t.warmthType, t.warmthAmount);
                warmth_.Set(t.warmth, t.warmthType, t.warmthAmount);
                tone_ = t;
            }
        }
        bool processed = false;
        const bool stopping = stopping_.load(std::memory_order_acquire);
        const uint32_t need =
            std::min(bypass_ ? slip_.InputFramesFor(bufferFrames_) : resampler_.InputFramesFor(bufferFrames_), maxIn);
        const LevelGuard::Decision decision = stopping ? guard.Halt() : guard.Before(fifo_, need);
        if (decision.skipped) droppedFrames_.fetch_add(decision.skipped, std::memory_order_relaxed);
        if (decision.resynced) resyncs_.fetch_add(1, std::memory_order_relaxed);
        if (decision.started || decision.resynced) drift_.RestartLevel();
        if (decision.started) state_.store(BridgeState::Playing);
        if (decision.underrun && !stopping) {
            underruns_.fetch_add(1, std::memory_order_relaxed);
            rebuffers_.fetch_add(1, std::memory_order_relaxed);
            state_.store(BridgeState::Buffering);
        }
        if ((decision.underrun && !stopping) || decision.resynced) CountDeclick();
        if (decision.started || (decision.underrun && stopping)) fades_.fetch_add(1, std::memory_order_relaxed);
        float inPeak[2]{}, outPeak[2]{};
        if (decision.play || decision.underrun || tail) {
            processed = true;
            // Resampler input for this period, every jump turned into a transition: fade-in on (re)start, a crossfade
            // over the frames dropped by a re-sync, and on an underrun or stop the frames still there followed by
            // their predicted continuation, faded out.
            if (decision.play) {
                const uint32_t got = fifo_.Read(in.data(), need);
                std::fill(in.begin() + size_t(got) * channels_, in.begin() + size_t(need) * channels_, 0.0f);
                if (decision.started || decision.resynced) inSplice_.Splice();
                inSplice_.Process(in.data(), need);
                tail = false;
            } else {
                uint32_t got = 0;
                if (decision.underrun) {
                    got = stopping ? 0 : fifo_.Read(in.data(), need);
                    inSplice_.Process(in.data(), got);
                    inSplice_.Splice();
                    tail = true;
                    tailFrames = 0;
                }
                std::fill(in.begin() + size_t(got) * channels_, in.begin() + size_t(need) * channels_, 0.0f);
                inSplice_.Process(in.data() + size_t(got) * channels_, need - got);
            }
            Peaks(in.data(), need, channels_, inPeak);  // before the warmth EQ, which works in place
            if (bypass_) {
                slip_.Process(in.data(), need, out.data(), bufferFrames_);
            } else {
                warmthEq_.Process(in.data(), need);
                if (plugins_) plugins_->Chain().Process(in.data(), need);
                const auto res = resampler_.Process(in.data(), need, out.data(), bufferFrames_, gain_);
                if (res.produced < bufferFrames_) std::fill(out.begin() + size_t(res.produced) * channels_, out.end(), 0.0f);
                warmth_.Process(out.data(), bufferFrames_);
                if (limiterOn_) {
                    limiter_.Process(out.data(), bufferFrames_);
                    StoreMin(limiterMinGain_, limiter_.TakeMinGain());
                    limitedFrames_.store(limiter_.LimitedFrames(), std::memory_order_relaxed);
                }
                if (tail && !inSplice_.Transitioning() && res.silent) tail = false;
            }
            if (tail) {
                tailFrames += bufferFrames_;
                if (!inSplice_.Transitioning() && (bypass_ || tailFrames >= tailLimit)) tail = false;
            }
            GuardOutput(out);
            if (decision.play) {
                // Continuous level (packet staircase removed); frozen while the source is silent/idle.
                double arrived = 0, age = 0;
                if (arrival_.Estimate(double(Qpc()) * qpcToSec_, arrived, age) && age < 0.1) {
                    const double level =
                        calibrator_.Correct(arrived - double(fifo_.TotalRead()), double(fifo_.Available()), periodSec);
                    levelBias_.store(calibrator_.BiasFrames(), std::memory_order_relaxed);
                    const double ppm = drift_.Update(level, periodSec);
                    if (bypass_) slip_.SetCorrectionPpm(ppm);
                    else resampler_.SetCorrectionPpm(ppm);
                    ppm_.store(ppm, std::memory_order_relaxed);
                    driftLocked_.store(drift_.Locked(), std::memory_order_relaxed);
                }
            }
            Peaks(out.data(), bufferFrames_, channels_, outPeak);
            StoreMax(peakOut_[0], outPeak[0]);
            StoreMax(peakOut_[1], outPeak[1]);
            clipped_.fetch_add(WritePeriod(out, data), std::memory_order_relaxed);
            if (decision.play) framesOut_.fetch_add(bufferFrames_, std::memory_order_relaxed);
        } else {
            WriteIdle(out, data);
            if (stopping) ++drainedPeriods;
        }
        for (int c = 0; c < 2; ++c) {
            meter.in[c] = std::max(meter.in[c], inPeak[c]);
            meter.out[c] = std::max(meter.out[c], outPeak[c]);
        }
        if (t0 - meterStart >= meterTicks) {
            meter.qpc = static_cast<int64_t>(t0);
            if (status_) status_->PushMeter(meter);
            meter = {};
            meterStart = t0;
        }
        fifoFrames_.store(double(fifo_.Available()), std::memory_order_relaxed);
        if (native_) again = true;  // until the queue is full (checked at the top)
        else render_->ReleaseBuffer(bufferFrames_, (!processed && !dop_) ? AUDCLNT_BUFFERFLAGS_SILENT : 0);
        checkDrained();
        const uint64_t dt = Qpc() - t0;
        if (dt > periodTicks * 4 / 5) slowPeriods_.fetch_add(1, std::memory_order_relaxed);
        cpuTicks_.fetch_add(dt, std::memory_order_relaxed);
        cpuCalls_.fetch_add(1, std::memory_order_relaxed);
        StoreMax(cpuMax_, dt);
    }
}

void BridgeEngine::LogGlitches() {
    const GlitchSnapshot now{underruns_.load(),       resyncs_.load(),     overruns_.load(),    discontinuities_.load(),
                             modulatorResets_.load(), lateWakeups_.load(), slowPeriods_.load(), badSamples_.load(),
                             native_ ? asio_.LateSwitches() : 0};
    auto delta = [](const wchar_t* name, uint64_t a, uint64_t b) {
        return a > b ? std::format(L" {} +{}", name, a - b) : std::wstring();
    };
    std::wstring what = delta(L"underruns", now.underruns, logged_.underruns) + delta(L"resyncs", now.resyncs, logged_.resyncs) +
                        delta(L"overruns", now.overruns, logged_.overruns) +
                        delta(L"source discontinuities", now.discontinuities, logged_.discontinuities) +
                        delta(L"modulator resets", now.modulatorResets, logged_.modulatorResets) +
                        delta(L"late render wake-ups", now.late, logged_.late) +
                        delta(L"slow periods", now.slow, logged_.slow) +
                        delta(L"invalid samples replaced", now.badSamples, logged_.badSamples) +
                        delta(L"ASIO buffers missed", now.lateSwitches, logged_.lateSwitches);
    if (what.empty()) return;
    logged_ = now;
    Log(std::format(L"glitch:{} (longest wake-up gap {:.1f} ms, period {:.1f} ms, fifo {:.1f} ms)", what,
                    double(sessionMaxWakeGap_) * qpcToSec_ * 1e3, base_.periodMs,
                    inRate_ ? fifoFrames_.load() * 1000.0 / inRate_ : 0.0));
    sessionMaxWakeGap_ = 0;
}

uint64_t BridgeEngine::WritePeriod(std::vector<float>& out, BYTE* data) {
    if (!Dsd()) return writer_.Write(out.data(), bufferFrames_, channels_, data);
    // DSD: PCM full scale maps to dsdScale_ of the modulator's range; anything beyond full scale (limiter off) is
    // clipped so the 1-bit loop stays stable.
    uint64_t clipped = 0;
    const size_t n = size_t(bufferFrames_) * channels_;
    for (size_t i = 0; i < n; ++i) {
        float v = out[i];
        if (std::abs(v) > 1.0f) {
            v = std::copysign(1.0f, v);
            ++clipped;
        }
        out[i] = v * dsdScale_;
    }
    if (native_) {
        QueueNative(out);
    } else {
        dsd_.Process(out.data(), bufferFrames_, dsdWords_.data());
        writer_.WriteDop(dsdWords_.data(), bufferFrames_, channels_, data);
    }
    modulatorResets_.store(dsd_.Resets(), std::memory_order_relaxed);
    modulatorClips_.store(dsd_.Clips(), std::memory_order_relaxed);
    return clipped;
}

void BridgeEngine::QueueNative(const std::vector<float>& out) {
    // The PCM is modulated fadeFrames_ (L) late: the period sent now is the L held frames plus the first n - L of
    // `out`, and the last L frames of `out`, faded out, are its emergency continuation (the real signal), so a dropout
    // never ends the stream abruptly.
    const uint32_t n = bufferFrames_, ch = channels_, hold = fadeFrames_;
    constexpr double kPi = 3.14159265358979323846;
    // Raised-cosine gains: fade-in at position p after a restart, fade-out at k.
    auto fadeIn = [&](uint64_t p) {
        return p < fadeFrames_ ? float(0.5 - 0.5 * std::cos(kPi * double(p + 1) / (fadeFrames_ + 1))) : 1.0f;
    };
    auto fadeOut = [&](uint32_t k) {
        return k < fadeFrames_ ? float(0.5 + 0.5 * std::cos(kPi * (k + 1) / (fadeFrames_ + 1))) : 0.0f;
    };
    // Emergency continuation: modulated fade-out, ~1/4 of the fade of modulated silence, then the DSD idle pattern.
    const uint32_t tail = std::min(n, fadeFrames_ + fadeFrames_ / 4 + 16);
    for (;;) {
        const uint64_t epoch = asio_.Epoch();
        if (epoch != asioEpoch_) {
            // The driver ran dry: what it played last faded out to silence, so the stream restarts from silence.
            asioEpoch_ = epoch;
            dsd_.Reset();
            fadeInPos_ = 0;
        }
        const bool fading = fadeInPos_ < fadeFrames_;
        for (uint32_t i = 0; i < n; ++i) {
            const float* src = i < hold ? &holdPcm_[size_t(i) * ch] : &out[size_t(i - hold) * ch];
            const float g = fading ? fadeIn(uint64_t(fadeInPos_) + i) : 1.0f;
            for (uint32_t c = 0; c < ch; ++c) modPcm_[size_t(i) * ch + c] = src[c] * g;
        }
        dsd_.Process(modPcm_.data(), n, dsdWords_.data());
        // The spare encoder continues from the state this period leaves (bit-exact) with the held-back frames faded
        // out (from the level a fade-in still running has reached).
        for (uint32_t k = 0; k < tail; ++k) {
            const float g = k < hold ? fadeOut(k) * fadeIn(uint64_t(fadeInPos_) + n + k) : 0.0f;
            for (uint32_t c = 0; c < ch; ++c)
                emergencyPcm_[size_t(k) * ch + c] = k < hold ? out[size_t(n - hold + k) * ch + c] * g : 0.0f;
        }
        dsdSpare_.CopyStateFrom(dsd_);
        dsdSpare_.Process(emergencyPcm_.data(), tail, emergencyWords_.data());
        std::fill(emergencyWords_.begin() + size_t(tail) * ch, emergencyWords_.end(), uint16_t{0x6969});
        // Refused only when the driver ran dry meanwhile: build the period again on the restarted stream.
        if (asio_.Push(dsdWords_.data(), emergencyWords_.data(), ch, epoch) || asio_.Epoch() == epoch) break;
    }
    if (fadeInPos_ < fadeFrames_) fadeInPos_ = std::min(fadeFrames_, fadeInPos_ + n);
    std::copy(out.end() - ptrdiff_t(hold) * ch, out.end(), holdPcm_.begin());
}

void BridgeEngine::WriteIdle(std::vector<float>& out, BYTE* data) {
    if (!Dsd()) {
        writer_.WriteSilence(bufferFrames_, data);
        return;
    }
    std::fill(out.begin(), out.end(), 0.0f);
    WritePeriod(out, data);
}

void BridgeEngine::GuardOutput(std::vector<float>& out) {
    // The input is sanitized, so this only trips on a numerical fault inside a stage; the stages are then restarted
    // (a NaN would otherwise stay in the IIR states and the filter history).
    const uint32_t bad = SanitizeSamples(out.data(), out.size(), std::numeric_limits<float>::max());
    if (!bad) return;
    badSamples_.fetch_add(bad, std::memory_order_relaxed);
    lastDeclick_.store(FileTimeNow(), std::memory_order_relaxed);
    std::fill(out.begin(), out.end(), 0.0f);
    inSplice_.Reset();
    if (bypass_) return;
    resampler_.Reset();
    warmthEq_.Configure(channels_, inRate_);
    warmth_.Configure(channels_, outRate_);
    warmthEq_.Set(tone_.warmth, tone_.warmthType, tone_.warmthAmount);
    warmth_.Set(tone_.warmth, tone_.warmthType, tone_.warmthAmount);
    if (limiterOn_) limiter_.Reset();
}

uint32_t BridgeEngine::MaxInputFrames() const {
    return bypass_ ? bufferFrames_ + 1 : static_cast<uint32_t>(uint64_t{bufferFrames_} * inRate_ / outRate_ * 2 + 64);
}

void BridgeEngine::PublishStatus() {
    if (!status_) return;
    BridgeStatusData d = base_;
    d.state = state_.load();
    if (inRate_) d.fifoMs = fifoFrames_.load() * 1000.0 / inRate_;
    d.driftPpm = ppm_.load();
    d.driftLocked = driftLocked_.load() ? 1u : 0u;
    d.deliveryMs = arrival_.DelaySec() * 1e3;
    if (double lo = 0, hi = 0; arrival_.TakeObservedRange(lo, hi)) {
        d.deliveryMinMs = lo * 1e3;
        d.deliveryMaxMs = hi * 1e3;
    }
    if (inRate_) d.levelBiasMs = levelBias_.load() * 1000.0 / inRate_;
    if (plugins_) {
        plugins_->Snapshot(d);
        if (bypass_) d.pluginLatencyMs = 0;
    }
    d.totalLatencyMs = d.state == BridgeState::Playing
                           ? kLoopbackLatencyMs + d.fifoMs + d.filterLatencyMs + d.limiterMs + d.periodMs +
                                 d.deviceLatencyMs + d.pluginLatencyMs
                           : 0.0;
    d.lateWakeups = lateWakeups_.load();
    if (native_) d.lateSwitches = asio_.LateSwitches();
    d.slowPeriods = slowPeriods_.load();
    {
        const uint64_t gap = maxWakeGap_.exchange(0);
        sessionMaxWakeGap_ = std::max(sessionMaxWakeGap_, gap);
        d.maxWakeGapMs = double(gap) * qpcToSec_ * 1e3;
    }
    d.limiterMinGain = limiterMinGain_.exchange(1.0f);
    d.limitedFrames = limitedFrames_.load();
    d.modulatorResets = modulatorResets_.load();
    d.modulatorClips = modulatorClips_.load();
    d.declicks = declicks_.load();
    d.declickGaps = declickGaps_.load();
    d.fades = fades_.load();
    d.lastDeclickTime = lastDeclick_.load();
    d.badSamples = badSamples_.load();
    d.framesOut = framesOut_.load();
    d.underruns = underruns_.load();
    d.overruns = overruns_.load();
    d.rebuffers = rebuffers_.load();
    d.clipped = clipped_.load();
    d.discontinuities = discontinuities_.load();
    d.resyncs = resyncs_.load();
    d.droppedFrames = droppedFrames_.load();
    const uint64_t calls = cpuCalls_.load(), ticks = cpuTicks_.load();
    if (calls > lastCalls_) d.cpuAvgUs = double(ticks - lastTicks_) / double(calls - lastCalls_) * qpcToUs_;
    lastCalls_ = calls;
    lastTicks_ = ticks;
    d.cpuMaxUs = double(cpuMax_.exchange(0)) * qpcToUs_;
    for (int c = 0; c < 2; ++c) {
        d.peakIn[c] = peakIn_[c].exchange(0.f);
        d.peakOut[c] = peakOut_[c].exchange(0.f);
    }
    CopyText(d.message, message_);
    d.heartbeat = FileTimeNow();
    status_->Publish(d);
}

}  // namespace dgmod::bridge
