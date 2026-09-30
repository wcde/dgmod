#include "bridge/AsioOutput.h"

#include "common/BridgeConfig.h"

#include <algorithm>
#include <cmath>
#include <condition_variable>
#include <cstring>
#include <format>
#include <functional>
#include <thread>

namespace dgmod::bridge {

namespace {

// ---- ASIO ABI (asio.h / iasiodrv.h of the ASIO SDK, Windows: long = 32 bit, IEEE double sample rates) ----
using ASIOBool = long;
using ASIOError = long;
using ASIOSampleRate = double;
struct ASIOSamples {
    unsigned long hi, lo;
};
struct ASIOTimeStamp {
    unsigned long hi, lo;
};
struct ASIOClockSource {
    long index, associatedChannel, associatedGroup;
    ASIOBool isCurrentSource;
    char name[32];
};
struct ASIOChannelInfo {
    long channel;
    ASIOBool isInput;
    ASIOBool isActive;
    long channelGroup;
    long type;
    char name[32];
};
struct ASIOBufferInfo {
    ASIOBool isInput;
    long channelNum;
    void* buffers[2];
};
struct ASIOCallbacks {
    void (*bufferSwitch)(long doubleBufferIndex, ASIOBool directProcess);
    void (*sampleRateDidChange)(ASIOSampleRate sRate);
    long (*asioMessage)(long selector, long value, void* message, double* opt);
    void* (*bufferSwitchTimeInfo)(void* params, long doubleBufferIndex, ASIOBool directProcess);  // ASIOTime*
};
struct ASIOIoFormat {
    long formatType;
    char future[512 - sizeof(long)];
};

constexpr ASIOError ASE_OK = 0;
constexpr ASIOError ASE_SUCCESS = 0x3f4847a0;
constexpr long kASIODSDFormat = 1;
constexpr long kAsioSetIoFormat = 0x23111961;
constexpr long kAsioCanDoIoFormat = 0x23112004;
constexpr long kASIOSTDSDInt8LSB1 = 32;
constexpr long kASIOSTDSDInt8MSB1 = 33;

enum : long {
    kAsioSelectorSupported = 1,
    kAsioEngineVersion,
    kAsioResetRequest,
    kAsioBufferSizeChange,
    kAsioResyncRequest,
    kAsioLatenciesChanged,
    kAsioSupportsTimeInfo,
    kAsioOverload = 15,
};

std::wstring AsioErrorText(ASIOError e) {
    switch (e) {
        case -1000: return L"ASE_NotPresent (no input/output present)";
        case -999: return L"ASE_HWMalfunction";
        case -998: return L"ASE_InvalidParameter";
        case -997: return L"ASE_InvalidMode";
        case -996: return L"ASE_SPNotAdvancing";
        case -995: return L"ASE_NoClock";
        case -994: return L"ASE_NoMemory";
        default: return std::format(L"ASIO error {}", e);
    }
}

int64_t Qpc() {
    LARGE_INTEGER v;
    ::QueryPerformanceCounter(&v);
    return v.QuadPart;
}

uint8_t Reverse8(uint8_t b) {
    b = static_cast<uint8_t>((b & 0xF0) >> 4 | (b & 0x0F) << 4);
    b = static_cast<uint8_t>((b & 0xCC) >> 2 | (b & 0x33) << 2);
    return static_cast<uint8_t>((b & 0xAA) >> 1 | (b & 0x55) << 1);
}

template <class D>
std::wstring DescribeDriver(D* d) {
    std::wstring s;
    long in = 0, out = 0;
    s += d->getChannels(&in, &out) == ASE_OK ? std::format(L"{} in / {} out", in, out) : std::wstring(L"getChannels failed");
    ASIOSampleRate rate = 0;
    if (d->getSampleRate(&rate) == ASE_OK) s += std::format(L", rate {:.0f}", rate);
    long minSize = 0, maxSize = 0, preferred = 0, granularity = 0;
    if (d->getBufferSize(&minSize, &maxSize, &preferred, &granularity) == ASE_OK)
        s += std::format(L", buffer {}..{} pref {} gran {}", minSize, maxSize, preferred, granularity);
    long inLat = 0, outLat = 0;
    if (d->getLatencies(&inLat, &outLat) == ASE_OK) s += std::format(L", latency out {}", outLat);
    ASIOClockSource clocks[8]{};
    long numClocks = 8;
    if (d->getClockSources(clocks, &numClocks) == ASE_OK)
        for (long i = 0; i < std::min(numClocks, 8L); ++i)
            s += std::format(L"\n      clock {} \"{}\"{}", clocks[i].index,
                             Widen(std::string(clocks[i].name, strnlen(clocks[i].name, 32))),
                             clocks[i].isCurrentSource ? L" (current)" : L"");
    for (long c = 0; c < std::min(out, 16L); ++c) {
        ASIOChannelInfo info{};
        info.channel = c;
        info.isInput = 0;
        if (d->getChannelInfo(&info) != ASE_OK) {
            s += std::format(L"\n      out {}: getChannelInfo failed", c);
            continue;
        }
        s += std::format(L"\n      out {} \"{}\": type {}, group {}, active {}", c,
                         Widen(std::string(info.name, strnlen(info.name, 32))), info.type, info.channelGroup, info.isActive);
    }
    return s + L"\n";
}

std::atomic<AsioDsdOutput*> g_active{nullptr};  // ASIO callbacks carry no context: one open driver per process

}  // namespace

// The IASIO interface (thiscall on x86, the only convention on x64).
struct AsioDsdOutput::Driver : public IUnknown {
    virtual ASIOBool init(void* sysHandle) = 0;
    virtual void getDriverName(char* name) = 0;  // up to 32 characters
    virtual long getDriverVersion() = 0;
    virtual void getErrorMessage(char* string) = 0;  // up to 124 characters
    virtual ASIOError start() = 0;
    virtual ASIOError stop() = 0;
    virtual ASIOError getChannels(long* numInputChannels, long* numOutputChannels) = 0;
    virtual ASIOError getLatencies(long* inputLatency, long* outputLatency) = 0;
    virtual ASIOError getBufferSize(long* minSize, long* maxSize, long* preferredSize, long* granularity) = 0;
    virtual ASIOError canSampleRate(ASIOSampleRate sampleRate) = 0;
    virtual ASIOError getSampleRate(ASIOSampleRate* sampleRate) = 0;
    virtual ASIOError setSampleRate(ASIOSampleRate sampleRate) = 0;
    virtual ASIOError getClockSources(ASIOClockSource* clocks, long* numSources) = 0;
    virtual ASIOError setClockSource(long reference) = 0;
    virtual ASIOError getSamplePosition(ASIOSamples* sPos, ASIOTimeStamp* tStamp) = 0;
    virtual ASIOError getChannelInfo(ASIOChannelInfo* info) = 0;
    virtual ASIOError createBuffers(ASIOBufferInfo* bufferInfos, long numChannels, long bufferSize,
                                    ASIOCallbacks* callbacks) = 0;
    virtual ASIOError disposeBuffers() = 0;
    virtual ASIOError controlPanel() = 0;
    virtual ASIOError future(long selector, void* opt) = 0;
    virtual ASIOError outputReady() = 0;
};

// Runs tasks on a single-threaded apartment with a message loop (ASIO drivers may post to their window).
class AsioDsdOutput::StaThread {
public:
    StaThread() : event_(::CreateEventW(nullptr, FALSE, FALSE, nullptr)) { thread_ = std::thread([this] { Loop(); }); }
    ~StaThread() {
        Run([this] { quit_ = true; });
        thread_.join();
    }
    StaThread(const StaThread&) = delete;
    StaThread& operator=(const StaThread&) = delete;

    void Run(std::function<void()> fn) {
        std::lock_guard call(callMutex_);
        std::unique_lock lock(mutex_);
        task_ = std::move(fn);
        pending_ = true;
        ::SetEvent(event_.Get());
        done_.wait(lock, [&] { return !pending_; });
    }

private:
    void Loop() {
        ::SetThreadDescription(::GetCurrentThread(), L"dgmod ASIO");
        ComScope com(COINIT_APARTMENTTHREADED);
        while (!quit_) {
            HANDLE h = event_.Get();
            const DWORD w = ::MsgWaitForMultipleObjects(1, &h, FALSE, INFINITE, QS_ALLINPUT);
            MSG msg;
            while (::PeekMessageW(&msg, nullptr, 0, 0, PM_REMOVE)) {
                ::TranslateMessage(&msg);
                ::DispatchMessageW(&msg);
            }
            if (w != WAIT_OBJECT_0) continue;
            std::function<void()> fn;
            {
                std::lock_guard lock(mutex_);
                fn = std::move(task_);
            }
            if (fn) fn();
            {
                std::lock_guard lock(mutex_);
                pending_ = false;
            }
            done_.notify_all();
        }
    }

    UniqueHandle event_;
    std::thread thread_;
    std::mutex callMutex_, mutex_;
    std::condition_variable done_;
    std::function<void()> task_;
    bool pending_ = false;
    bool quit_ = false;  // STA thread only
};

void PackDsdChannel(const uint16_t* words, uint32_t frames, uint32_t channels, uint32_t channel, bool lsbFirst, uint8_t* dst) {
    const uint16_t* w = words + channel;
    if (lsbFirst) {
        for (uint32_t i = 0; i < frames; ++i, w += channels) {
            dst[2 * i] = Reverse8(static_cast<uint8_t>(*w >> 8));
            dst[2 * i + 1] = Reverse8(static_cast<uint8_t>(*w));
        }
    } else {
        for (uint32_t i = 0; i < frames; ++i, w += channels) {
            dst[2 * i] = static_cast<uint8_t>(*w >> 8);
            dst[2 * i + 1] = static_cast<uint8_t>(*w);
        }
    }
}

AsioDsdOutput::AsioDsdOutput() = default;
AsioDsdOutput::~AsioDsdOutput() { Close(); }

Result<AsioDsdOutput::Opened> AsioDsdOutput::Open(const Request& req, uint32_t& supportedMask) {
    Close();
    supportedMask = 0;
    LARGE_INTEGER f;
    ::QueryPerformanceFrequency(&f);
    qpcFreq_ = double(f.QuadPart);
    switchEvent_.Reset(::CreateEventW(nullptr, FALSE, FALSE, nullptr));
    resetEvent_.Reset(::CreateEventW(nullptr, TRUE, FALSE, nullptr));
    switchSeq_ = late_ = overloads_ = timedSwitches_ = 0;
    firstSwitchQpc_ = lastSwitchQpc_ = 0;
    {
        std::lock_guard lock(queueMutex_);
        head_ = tail_ = 0;
        emergencyReady_ = false;
        epoch_ = 0;
    }
    {
        std::lock_guard lock(reasonMutex_);
        resetReason_.clear();
    }
    AsioDsdOutput* expected = nullptr;
    if (!g_active.compare_exchange_strong(expected, this))
        return dgmod::Fail(E_FAIL, L"Another ASIO driver is already open in this process");

    sta_ = std::make_unique<StaThread>();
    Result<Opened> result = dgmod::Fail(E_FAIL, L"ASIO: not opened");
    sta_->Run([&] {
        auto fail = [&](const std::wstring& what) { result = dgmod::Fail(E_FAIL, what); };
        auto driverError = [&](const std::wstring& what) {
            char text[128]{};
            driver_->getErrorMessage(text);
            fail(text[0] ? std::format(L"{}: {}", what, Widen(text)) : what);
        };
        HRESULT hr = ::CoCreateInstance(req.clsid, nullptr, CLSCTX_INPROC_SERVER, req.clsid, reinterpret_cast<void**>(&driver_));
        if (FAILED(hr) || !driver_) {
            driver_ = nullptr;
            result = dgmod::Fail(FAILED(hr) ? hr : E_NOINTERFACE, std::format(L"The ASIO driver \"{}\" could not be loaded", req.name));
            return;
        }
        window_ = ::CreateWindowExW(0, L"STATIC", L"dgmod ASIO", WS_POPUP, 0, 0, 0, 0, nullptr, nullptr,
                                    ::GetModuleHandleW(nullptr), nullptr);
        if (driver_->init(window_) != 1) return driverError(std::format(L"The ASIO driver \"{}\" did not start (is the DAC connected?)", req.name));
        Opened o;
        {
            char name[64]{};
            driver_->getDriverName(name);
            o.driverName = name[0] ? Widen(name) : req.name;
        }
        long inputs = 0, outputs = 0;
        if (driver_->getChannels(&inputs, &outputs) != ASE_OK || outputs <= 0) return driverError(L"The ASIO driver reports no outputs");
        o.deviceChannels = static_cast<uint32_t>(outputs);
        o.channels = std::clamp<uint32_t>(req.channels, 1, o.deviceChannels);

        ASIOIoFormat fmt{};
        fmt.formatType = kASIODSDFormat;
        if (driver_->future(kAsioCanDoIoFormat, &fmt) != ASE_SUCCESS)
            return fail(std::format(L"The ASIO driver \"{}\" has no native DSD mode", o.driverName));
        if (driver_->future(kAsioSetIoFormat, &fmt) != ASE_SUCCESS)
            return driverError(L"The ASIO driver refused to switch to DSD");
        for (const uint32_t m : kNativeDsdMultiples)
            if (driver_->canSampleRate(44100.0 * m) == ASE_OK) o.supportedMask |= DsdMaskBit(m);
        supportedMask = o.supportedMask;
        if (!o.supportedMask) return fail(L"The ASIO driver accepts no DSD rate in DSD mode");
        for (const uint32_t m : kNativeDsdMultiples) {
            if (!(o.supportedMask & DsdMaskBit(m))) continue;
            if (req.dsdMultiple ? m == req.dsdMultiple : m <= kNativeDsdAutoMax) {
                o.dsdMultiple = m;
                break;
            }
        }
        if (!o.dsdMultiple && !req.dsdMultiple)  // only rates above the automatic limit
            for (const uint32_t m : kNativeDsdMultiples)
                if (o.supportedMask & DsdMaskBit(m)) o.dsdMultiple = m;
        if (!o.dsdMultiple) return fail(std::format(L"The ASIO driver does not accept DSD{} (native)", req.dsdMultiple));
        o.dsdRate = 44100u * o.dsdMultiple;
        if (driver_->setSampleRate(double(o.dsdRate)) != ASE_OK)
            return driverError(std::format(L"The ASIO driver refused DSD{}", o.dsdMultiple));

        // Sample format of the outputs in DSD mode.
        for (uint32_t c = 0; c < o.channels; ++c) {
            ASIOChannelInfo info{};
            info.channel = static_cast<long>(c);
            info.isInput = 0;
            if (driver_->getChannelInfo(&info) != ASE_OK) return driverError(L"ASIO getChannelInfo failed");
            if (info.type != kASIOSTDSDInt8MSB1 && info.type != kASIOSTDSDInt8LSB1)
                return fail(std::format(L"The ASIO driver uses an unsupported DSD sample type ({})", info.type));
            if (c == 0) o.lsbFirst = info.type == kASIOSTDSDInt8LSB1;
            else if (o.lsbFirst != (info.type == kASIOSTDSDInt8LSB1)) return fail(L"The ASIO outputs mix DSD bit orders");
        }

        // Buffer size in DSD samples: the allowed size closest to the wanted period, whole 16-bit words.
        long minSize = 0, maxSize = 0, preferred = 0, granularity = 0;
        if (driver_->getBufferSize(&minSize, &maxSize, &preferred, &granularity) != ASE_OK)
            return driverError(L"ASIO getBufferSize failed");
        // Automatic: the size set in the driver's control panel, unless it is below 2 ms (then 10 ms).
        const double preferredMs = preferred > 0 ? preferred * 1000.0 / o.dsdRate : 0.0;
        const double wantedMs = req.periodMs > 0 ? req.periodMs : preferredMs >= 2.0 ? preferredMs : 10.0;
        const double target = o.dsdRate * wantedMs / 1000.0;
        long best = 0;
        auto consider = [&](long s) {
            if (s <= 0 || s % 16 != 0) return;
            if (!best || std::abs(std::log(double(s) / target)) < std::abs(std::log(double(best) / target))) best = s;
        };
        if (granularity == -1) {
            for (long s = 1; s > 0 && s <= maxSize; s *= 2)
                if (s >= minSize) consider(s);
        } else if (granularity > 0) {
            for (long s = minSize; s <= maxSize; s += granularity) consider(s);
        }
        consider(preferred);
        consider(minSize);
        consider(maxSize);
        if (!best)
            return fail(std::format(L"No ASIO buffer size is a multiple of 16 DSD samples (min {}, max {}, preferred {}, "
                                    L"granularity {})",
                                    minSize, maxSize, preferred, granularity));
        o.bufferSamples = static_cast<uint32_t>(best);
        o.frames = o.bufferSamples / 16;
        const double bufferMs = o.bufferSamples * 1000.0 / o.dsdRate;
        depth_ = std::clamp<uint32_t>(static_cast<uint32_t>(std::ceil(kQueueSlackMs / bufferMs - 1e-9)), 1, kMaxQueueDepth);

        std::vector<ASIOBufferInfo> infos(o.channels);
        for (uint32_t c = 0; c < o.channels; ++c) infos[c] = {0, static_cast<long>(c), {nullptr, nullptr}};
        static ASIOCallbacks callbacks{&CbBufferSwitch, &CbSampleRateChanged, &CbMessage, &CbBufferSwitchTimeInfo};
        opened_ = o;  // the callbacks may fire from createBuffers on
        if (const ASIOError e = driver_->createBuffers(infos.data(), static_cast<long>(o.channels), best, &callbacks); e != ASE_OK)
            return driverError(std::format(L"ASIO createBuffers ({} samples) failed: {}", best, AsioErrorText(e)));
        buffersCreated_ = true;
        buffers_.assign(o.channels, {});
        for (uint32_t c = 0; c < o.channels; ++c) {
            if (!infos[c].buffers[0] || !infos[c].buffers[1]) return fail(L"The ASIO driver returned no buffers");
            buffers_[c] = {infos[c].buffers[0], infos[c].buffers[1]};
        }
        bytesPerBuffer_ = o.bufferSamples / 8;
        silence_ = o.lsbFirst ? Reverse8(0x69) : uint8_t{0x69};
        {
            std::lock_guard lock(queueMutex_);
            slots_.assign(size_t(kSlots) * 2 * bytesPerBuffer_ * o.channels, silence_);
        }
        FillSilence(0);
        FillSilence(1);
        long inLatency = 0, outLatency = 0;
        if (driver_->getLatencies(&inLatency, &outLatency) == ASE_OK) o.outputLatencyMs = outLatency * 1000.0 / o.dsdRate;
        o.outputReady = driver_->outputReady() == ASE_OK;
        opened_ = o;
        result = o;
    });
    if (!result) Close();
    return result;
}

Result<void> AsioDsdOutput::Start() {
    if (!sta_ || !driver_ || !buffersCreated_) return dgmod::Fail(E_UNEXPECTED, L"ASIO: not open");
    ASIOError e = ASE_OK;
    sta_->Run([&] {
        e = driver_->start();
        started_ = e == ASE_OK;
    });
    if (e != ASE_OK) return dgmod::Fail(E_FAIL, L"The ASIO driver did not start: " + AsioErrorText(e));
    return {};
}

void AsioDsdOutput::Close() {
    if (sta_) {
        sta_->Run([&] {
            if (driver_) {
                if (started_) driver_->stop();
                if (buffersCreated_) driver_->disposeBuffers();
                driver_->Release();
            }
            driver_ = nullptr;
            started_ = buffersCreated_ = false;
            if (window_) ::DestroyWindow(window_);
            window_ = nullptr;
        });
        sta_.reset();
    }
    buffers_.clear();
    AsioDsdOutput* self = this;
    g_active.compare_exchange_strong(self, nullptr);
}

Result<AsioDsdOutput::Capabilities> AsioDsdOutput::Probe(const CLSID& clsid) {
    StaThread sta;
    Result<Capabilities> result = dgmod::Fail(E_FAIL, L"ASIO: not probed");
    sta.Run([&] {
        Driver* d = nullptr;
        const HRESULT hr = ::CoCreateInstance(clsid, nullptr, CLSCTX_INPROC_SERVER, clsid, reinterpret_cast<void**>(&d));
        if (FAILED(hr) || !d) {
            result = dgmod::Fail(FAILED(hr) ? hr : E_NOINTERFACE, L"The ASIO driver could not be loaded");
            return;
        }
        HWND window = ::CreateWindowExW(0, L"STATIC", L"dgmod ASIO", WS_POPUP, 0, 0, 0, 0, nullptr, nullptr,
                                        ::GetModuleHandleW(nullptr), nullptr);
        Capabilities c;
        if (d->init(window) != 1) {
            char text[128]{};
            d->getErrorMessage(text);
            result = dgmod::Fail(E_FAIL, L"init failed: " + Widen(text));
        } else {
            char name[64]{};
            d->getDriverName(name);
            c.driverName = Widen(name);
            long in = 0, out = 0;
            d->getChannels(&in, &out);
            c.inputs = static_cast<uint32_t>(std::max(0L, in));
            c.outputs = static_cast<uint32_t>(std::max(0L, out));
            c.details = L"PCM mode: " + DescribeDriver(d);
            ASIOIoFormat fmt{};
            fmt.formatType = kASIODSDFormat;
            c.dsd = d->future(kAsioCanDoIoFormat, &fmt) == ASE_SUCCESS && d->future(kAsioSetIoFormat, &fmt) == ASE_SUCCESS;
            if (c.dsd) {
                uint32_t highest = 0;
                for (const uint32_t m : kNativeDsdMultiples)
                    if (d->canSampleRate(44100.0 * m) == ASE_OK) {
                        c.supportedMask |= DsdMaskBit(m);
                        highest = std::max(highest, m);
                    }
                if (highest) d->setSampleRate(44100.0 * highest);
                d->getBufferSize(&c.minSize, &c.maxSize, &c.preferredSize, &c.granularity);
                ASIOChannelInfo info{};
                info.channel = 0;
                if (c.outputs && d->getChannelInfo(&info) == ASE_OK) c.sampleType = info.type;
                c.details += L"    DSD mode: " + DescribeDriver(d);
                // Leave the driver in PCM mode for other hosts.
                ASIOIoFormat pcm{};
                pcm.formatType = 0;
                d->future(kAsioSetIoFormat, &pcm);
            }
            result = c;
        }
        d->Release();
        if (window) ::DestroyWindow(window);
    });
    return result;
}

uint32_t AsioDsdOutput::Space() const {
    std::lock_guard lock(queueMutex_);
    return depth_ - std::min(depth_, static_cast<uint32_t>(head_ - tail_));
}

uint32_t AsioDsdOutput::Queued() const {
    std::lock_guard lock(queueMutex_);
    return static_cast<uint32_t>(head_ - tail_);
}

bool AsioDsdOutput::Push(const uint16_t* words, const uint16_t* emergency, uint32_t channels, uint64_t epoch) {
    uint64_t slot = 0;
    {
        std::lock_guard lock(queueMutex_);
        if (head_ - tail_ >= depth_ || slots_.empty()) return false;
        slot = head_;
    }
    // Slot head_ is neither queued nor the last played one, so the callback does not read it meanwhile.
    for (int e = 0; e < 2; ++e) {
        uint8_t* dst = Slot(slot, e == 1);
        for (uint32_t c = 0; c < opened_.channels; ++c)
            PackDsdChannel(e ? emergency : words, opened_.frames, channels, std::min(c, channels - 1), opened_.lsbFirst,
                           dst + size_t(c) * bytesPerBuffer_);
    }
    std::lock_guard lock(queueMutex_);
    if (epoch_.load(std::memory_order_relaxed) != epoch) return false;
    ++head_;
    return true;
}

void AsioDsdOutput::CopySlot(const uint8_t* src, long index) {
    for (uint32_t c = 0; c < opened_.channels; ++c)
        std::memcpy(buffers_[c][index & 1], src + size_t(c) * bytesPerBuffer_, bytesPerBuffer_);
}

void AsioDsdOutput::FillSilence(long index) {
    for (auto& b : buffers_) std::memset(b[index & 1], silence_, bytesPerBuffer_);
}

double AsioDsdOutput::MeasuredSwitchSec() const {
    const uint64_t n = timedSwitches_.load(std::memory_order_relaxed);
    if (n < 20) return 0.0;
    return double(lastSwitchQpc_.load() - firstSwitchQpc_.load()) / double(n - 1) / qpcFreq_;
}

std::wstring AsioDsdOutput::ResetReason() const {
    std::lock_guard lock(reasonMutex_);
    return resetReason_;
}

void AsioDsdOutput::RequestReset(const std::wstring& reason) {
    {
        std::lock_guard lock(reasonMutex_);
        if (resetReason_.empty()) resetReason_ = reason;
    }
    ::SetEvent(resetEvent_.Get());
}

void AsioDsdOutput::OnSwitch(long index) {
    const int64_t now = Qpc();
    if (!firstSwitchQpc_.load(std::memory_order_relaxed)) firstSwitchQpc_.store(now, std::memory_order_relaxed);
    lastSwitchQpc_.store(now, std::memory_order_relaxed);
    timedSwitches_.fetch_add(1, std::memory_order_relaxed);
    switchSeq_.fetch_add(1, std::memory_order_relaxed);
    {
        std::lock_guard lock(queueMutex_);
        if (tail_ < head_) {
            CopySlot(Slot(tail_, false), index);
            ++tail_;
            emergencyReady_ = true;
        } else {
            // Nothing ready: never replay the stale half. The stream of the buffer played last fades out to silence,
            // and whatever the render thread built on the old stream is refused.
            if (emergencyReady_) CopySlot(Slot(tail_ - 1, true), index);
            else FillSilence(index);
            if (emergencyReady_) late_.fetch_add(1, std::memory_order_relaxed);
            emergencyReady_ = false;
            epoch_.fetch_add(1, std::memory_order_release);
        }
    }
    ::SetEvent(switchEvent_.Get());
    if (opened_.outputReady) driver_->outputReady();
}

long AsioDsdOutput::OnMessage(long selector, long value) {
    switch (selector) {
        case kAsioSelectorSupported:
            return value == kAsioEngineVersion || value == kAsioResetRequest || value == kAsioBufferSizeChange ||
                           value == kAsioResyncRequest || value == kAsioLatenciesChanged || value == kAsioOverload
                       ? 1
                       : 0;
        case kAsioEngineVersion: return 2;
        case kAsioResetRequest: RequestReset(L"The ASIO driver asked to be restarted"); return 1;
        case kAsioBufferSizeChange: RequestReset(L"The ASIO driver changed its buffer size"); return 1;
        case kAsioResyncRequest:
        case kAsioLatenciesChanged: return 1;
        case kAsioOverload: overloads_.fetch_add(1, std::memory_order_relaxed); return 1;
        case kAsioSupportsTimeInfo:
        default: return 0;
    }
}

void AsioDsdOutput::CbBufferSwitch(long index, long) {
    if (AsioDsdOutput* self = g_active.load(std::memory_order_acquire); self && self->buffersCreated_) self->OnSwitch(index);
}

void AsioDsdOutput::CbSampleRateChanged(double rate) {
    if (AsioDsdOutput* self = g_active.load(std::memory_order_acquire); self && self->opened_.dsdRate &&
                                                                         std::abs(rate - self->opened_.dsdRate) > 0.5)
        self->RequestReset(std::format(L"The ASIO driver switched to {} Hz", rate));
}

long AsioDsdOutput::CbMessage(long selector, long value, void*, double*) {
    AsioDsdOutput* self = g_active.load(std::memory_order_acquire);
    return self ? self->OnMessage(selector, value) : 0;
}

void* AsioDsdOutput::CbBufferSwitchTimeInfo(void* params, long index, long directProcess) {
    CbBufferSwitch(index, directProcess);
    return params;
}

}  // namespace dgmod::bridge
