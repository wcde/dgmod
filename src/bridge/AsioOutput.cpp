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

// The requested DSD rate if `mask` has it; for 0 the highest up to kNativeDsdAutoMax (else the lowest above). 0: none.
uint32_t ChooseDsdMultiple(uint32_t mask, uint32_t requested) {
    for (const uint32_t m : kNativeDsdMultiples)
        if ((mask & DsdMaskBit(m)) && (requested ? m == requested : m <= kNativeDsdAutoMax)) return m;
    uint32_t chosen = 0;
    if (!requested)  // only rates above the automatic limit
        for (const uint32_t m : kNativeDsdMultiples)
            if (mask & DsdMaskBit(m)) chosen = m;
    return chosen;
}

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
    switchSeq_ = late_ = missing_ = overloads_ = resyncs_ = stalls_ = timedSwitches_ = 0;
    request_ = req;
    firstSwitchQpc_ = lastSwitchQpc_ = 0;
    {
        std::lock_guard lock(queueMutex_);
        head_ = tail_ = 0;
        emergencyReady_ = false;
        fed_ = true;
        emergencySeq_.fill(~uint64_t{0});
        epoch_ = 0;
    }
    {
        std::lock_guard lock(reasonMutex_);
        resetReason_.clear();
    }
    if (!req.network.empty()) {
        auto result = OpenNetwork(req, supportedMask);
        if (!result) Close();
        return result;
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
        o.dsdMultiple = ChooseDsdMultiple(o.supportedMask, req.dsdMultiple);
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
        InitBuffers();
        long inLatency = 0, outLatency = 0;
        if (driver_->getLatencies(&inLatency, &outLatency) == ASE_OK) o.outputLatencyMs = outLatency * 1000.0 / o.dsdRate;
        o.outputReady = driver_->outputReady() == ASE_OK;
        opened_ = o;
        result = o;
    });
    if (!result) Close();
    return result;
}

void AsioDsdOutput::InitBuffers() {
    {
        std::lock_guard lock(queueMutex_);
        slots_.assign(size_t(kSlots) * 2 * bytesPerBuffer_ * opened_.channels, uint8_t{0x69});
        idle_.assign(bytesPerBuffer_, opened_.lsbFirst ? Reverse8(0x69) : uint8_t{0x69});
        endSeq_ = ~uint64_t{0};
        rulesPending_ = false;
    }
    minRun_ = 1;
    maxRun_ = 32;
    checkReset_ = true;
    checkedBuffers_ = violations_ = 0;
    lastBadRun_ = lastBadSource_ = 0;
    run_.assign(opened_.channels, 0);
    runBit_.assign(opened_.channels, 0);
    tap_.assign(size_t(kTapSlots) * bytesPerBuffer_ * opened_.channels, 0);
    tapHead_ = 0;
    FillSilence(0);
    FillSilence(1);
}

Result<AsioDsdOutput::Opened> AsioDsdOutput::OpenNetwork(const Request& req, uint32_t& supportedMask) {
    net_ = std::make_unique<NetworkLink>();
    auto caps = net_->Connect(req.network, 3000, [this](const std::wstring& why) { RequestReset(why); });
    if (!caps) return std::unexpected(caps.error());
    Opened o;
    o.network = true;
    o.driverName = std::format(L"{} ({})", Widen(std::string(caps->device, strnlen(caps->device, sizeof(caps->device)))),
                               Widen(std::string(caps->format, strnlen(caps->format, sizeof(caps->format)))));
    uint32_t known = 0;
    for (const uint32_t m : kNativeDsdMultiples) known |= DsdMaskBit(m);
    o.supportedMask = supportedMask = caps->dsdMask & known;
    if (!o.supportedMask) return dgmod::Fail(E_FAIL, std::format(L"The DAC of the receiver at {} accepts no native DSD rate", req.network));
    if (!caps->maxChannels) return dgmod::Fail(E_FAIL, L"The receiver reports a DAC without channels");
    o.deviceChannels = caps->maxChannels;
    o.channels = std::clamp<uint32_t>(req.channels, 1, o.deviceChannels);
    o.dsdMultiple = ChooseDsdMultiple(o.supportedMask, req.dsdMultiple);
    if (!o.dsdMultiple) return dgmod::Fail(E_FAIL, std::format(L"The DAC of the receiver does not accept DSD{} (native)", req.dsdMultiple));
    o.dsdRate = 44100u * o.dsdMultiple;
    // Whole 64-bit groups per channel: every ALSA DSD format (8, 16 or 32 bits per sample) divides a buffer.
    const double wantedMs = req.periodMs > 0 ? req.periodMs : kNetworkPeriodMs;
    o.bufferSamples = std::max<uint32_t>(64, static_cast<uint32_t>(std::lround(o.dsdRate * wantedMs / 1000.0 / 64.0)) * 64);
    o.frames = o.bufferSamples / 16;
    bytesPerBuffer_ = o.bufferSamples / 8;
    const double bufferMs = o.bufferSamples * 1000.0 / o.dsdRate;
    depth_ = std::clamp<uint32_t>(static_cast<uint32_t>(std::ceil(kQueueSlackMs / bufferMs - 1e-9)), 1, kMaxQueueDepth);

    netHalves_.assign(size_t(2) * o.channels * bytesPerBuffer_, uint8_t{0x69});
    buffers_.assign(o.channels, {});
    for (uint32_t c = 0; c < o.channels; ++c)
        buffers_[c] = {netHalves_.data() + size_t(c) * bytesPerBuffer_,
                       netHalves_.data() + size_t(o.channels + c) * bytesPerBuffer_};
    opened_ = o;
    InitBuffers();
    net::Open open{o.dsdMultiple, o.channels, bytesPerBuffer_};
    const size_t halfBytes = size_t(o.channels) * bytesPerBuffer_;
    auto ready = net_->Open(open, [this, halfBytes](long half) {
        OnSwitch(half);
        return netHalves_.data() + (half & 1) * halfBytes;
    });
    if (!ready) return std::unexpected(ready.error());
    const double bytesPerMs = o.dsdRate / 8000.0;
    netTarget_ = uint64_t{ready->bufferBytes} + bytesPerBuffer_ + static_cast<uint64_t>(std::lround(kNetworkLeadMs * bytesPerMs));
    o.outputLatencyMs = double(netTarget_) / bytesPerMs;
    o.kept = ready->kept != 0;
    buffersCreated_ = true;
    opened_ = o;
    request_ = req;
    return o;
}

bool AsioDsdOutput::Running(const Request& req) const {
    if (net_)
        return buffersCreated_ && started_ && !net_->Failed() && !ResetRequested() && req.network == request_.network &&
               req.dsdMultiple == request_.dsdMultiple && req.channels == request_.channels && req.periodMs == request_.periodMs;
    return sta_ && driver_ && buffersCreated_ && started_ && !ResetRequested() && req.network.empty() &&
           ::IsEqualGUID(req.clsid, request_.clsid) && req.dsdMultiple == request_.dsdMultiple &&
           req.channels == request_.channels && req.periodMs == request_.periodMs;
}

bool AsioDsdOutput::ResetRequested() const {
    return resetEvent_ && ::WaitForSingleObject(resetEvent_.Get(), 0) == WAIT_OBJECT_0;
}

Result<void> AsioDsdOutput::Start() {
    if (net_) {
        if (!buffersCreated_) return dgmod::Fail(E_UNEXPECTED, L"Network: not open");
        if (net_->Failed()) return dgmod::Fail(E_FAIL, net_->Error());
        if (!started_) net_->Start(netTarget_);  // the pump fills the halves itself
        started_ = true;
        return {};
    }
    if (!sta_ || !driver_ || !buffersCreated_) return dgmod::Fail(E_UNEXPECTED, L"ASIO: not open");
    if (started_) return {};
    {
        std::lock_guard lock(queueMutex_);
        FillSilence(0);
        FillSilence(1);
    }
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
    if (net_) {
        net_->Close();  // joins the pump: no buffer switch runs after this
        net_.reset();
        started_ = buffersCreated_ = false;
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

bool AsioDsdOutput::Push(const uint16_t* words, const uint16_t* emergency, uint32_t channels, uint64_t epoch,
                         uint64_t* seq) {
    uint64_t slot = 0;
    {
        std::lock_guard lock(queueMutex_);
        if (head_ - tail_ >= depth_ || slots_.empty()) return false;
        slot = head_;
        emergencySeq_[slot % kSlots] = ~uint64_t{0};
    }
    // Slot head_ is neither queued nor the last played one, so the callback does not read it meanwhile.
    for (int e = 0; e < (emergency ? 2 : 1); ++e) {
        uint8_t* dst = Slot(slot, e == 1);
        for (uint32_t c = 0; c < opened_.channels; ++c)
            PackDsdChannel(e ? emergency : words, opened_.frames, channels, std::min(c, channels - 1), opened_.lsbFirst,
                           dst + size_t(c) * bytesPerBuffer_);
    }
    std::lock_guard lock(queueMutex_);
    if (epoch_.load(std::memory_order_relaxed) != epoch) return false;
    if (emergency) emergencySeq_[slot % kSlots] = slot;
    if (seq) *seq = slot;
    ++head_;
    fed_ = true;
    return true;
}

void AsioDsdOutput::EndFeed() {
    std::lock_guard lock(queueMutex_);
    fed_ = false;
    endSeq_ = head_;
    epoch_.fetch_add(1, std::memory_order_release);
}

void AsioDsdOutput::SetEmergency(uint64_t seq, const uint16_t* emergency, uint32_t channels) {
    // The callback reads a slot's continuation only once it is marked, and the slot is not reused before the next
    // Push, so it is packed without the lock.
    uint8_t* dst = Slot(seq, true);
    for (uint32_t c = 0; c < opened_.channels; ++c)
        PackDsdChannel(emergency, opened_.frames, channels, std::min(c, channels - 1), opened_.lsbFirst,
                       dst + size_t(c) * bytesPerBuffer_);
    std::lock_guard lock(queueMutex_);
    emergencySeq_[seq % kSlots] = seq;
}

void AsioDsdOutput::CopySlot(const uint8_t* src, long index) {
    for (uint32_t c = 0; c < opened_.channels; ++c)
        std::memcpy(buffers_[c][index & 1], src + size_t(c) * bytesPerBuffer_, bytesPerBuffer_);
}

void AsioDsdOutput::SetIdle(const uint16_t* idle, uint32_t minRun, uint32_t maxRun) {
    std::vector<uint8_t> packed(bytesPerBuffer_);
    PackDsdChannel(idle, opened_.frames, 1, 0, opened_.lsbFirst, packed.data());
    // The receiver repeats it whenever its queue runs dry (and plays it until the first buffer arrives).
    if (net_) net_->SendIdle(packed);
    std::lock_guard lock(queueMutex_);
    // Other rules or another silence: the stream changes its kind with the next buffer pushed. Until that one plays,
    // what plays belongs to the stream before (or is silence of the new kind): not checked; its join with the new
    // stream is one no rule covers either.
    if (minRun != minRun_.load() || maxRun != maxRun_.load() || packed != idle_) {
        pendingMin_ = minRun;
        pendingMax_ = maxRun;
        pendingSeq_ = head_;
        rulesPending_ = true;
    }
    idle_ = std::move(packed);
}

AsioDsdOutput::BitCheck AsioDsdOutput::Checked() const {
    return {checkedBuffers_.load(std::memory_order_relaxed), violations_.load(std::memory_order_relaxed),
            lastBadRun_.load(std::memory_order_relaxed), lastBadSource_.load(std::memory_order_relaxed)};
}

bool AsioDsdOutput::ReadTap(uint64_t& next, std::vector<uint8_t>& bytes, uint32_t& source, bool& skipped) const {
    const uint64_t h = tapHead_.load(std::memory_order_acquire);
    skipped = false;
    if (next >= h) return false;
    if (h - next > kTapSlots - 8) {  // fell behind: keep a margin from the writer
        next = h - (kTapSlots - 8);
        skipped = true;
    }
    const size_t slotBytes = size_t(bytesPerBuffer_) * buffers_.size();
    const uint8_t* src = tap_.data() + (next % kTapSlots) * slotBytes;
    bytes.assign(src, src + slotBytes);
    source = tapSource_[next % kTapSlots];
    ++next;
    return true;
}

void AsioDsdOutput::FillSilence(long index) {
    for (auto& b : buffers_) std::memcpy(b[index & 1], idle_.data(), bytesPerBuffer_);
}

namespace {

// Per byte, earliest bit first: length of the leading and trailing run, and the runs that touch neither end.
struct RunTables {
    uint8_t lead[256], trail[256], innerCount[256], inner[256][6], rev[256];
    RunTables() {
        for (int b = 0; b < 256; ++b) {
            uint8_t runs[8]{};
            int n = 0, len = 1;
            for (int i = 6; i >= 0; --i) {
                if (((b >> i) & 1) == ((b >> (i + 1)) & 1)) {
                    ++len;
                } else {
                    runs[n++] = static_cast<uint8_t>(len);
                    len = 1;
                }
            }
            runs[n++] = static_cast<uint8_t>(len);
            lead[b] = runs[0];
            trail[b] = runs[n - 1];
            innerCount[b] = static_cast<uint8_t>(n > 2 ? n - 2 : 0);
            for (int i = 1; i + 1 < n; ++i) inner[b][i - 1] = runs[i];
            rev[b] = Reverse8(static_cast<uint8_t>(b));
        }
    }
};
const RunTables g_runs;

}  // namespace

void AsioDsdOutput::CheckBits(long index, uint32_t source) {
    if (checkReset_.exchange(false, std::memory_order_acquire)) std::fill(run_.begin(), run_.end(), 0u);
    const uint32_t lo = minRun_.load(std::memory_order_relaxed), hi = maxRun_.load(std::memory_order_relaxed);
    uint64_t bad = 0;
    uint32_t badRun = 0;
    auto check = [&](uint32_t r) {
        if (r && (r < lo || r > hi)) {
            ++bad;
            badRun = r;
        }
    };
    for (size_t c = 0; c < buffers_.size(); ++c) {
        const auto* p = static_cast<const uint8_t*>(buffers_[c][index & 1]);
        uint32_t run = run_[c];
        uint32_t bit = run ? runBit_[c] : 2u;
        bool partial = !run;  // the first run began before the check (re)started: its length is unknown
        auto ended = [&](uint32_t r) {
            if (!r) return;
            if (partial) partial = false;
            else check(r);
        };
        for (uint32_t i = 0; i < bytesPerBuffer_; ++i) {
            const uint8_t b = opened_.lsbFirst ? g_runs.rev[p[i]] : p[i];
            const uint32_t top = b >> 7;
            if (g_runs.lead[b] == 8) {  // 0x00 or 0xFF
                if (top == bit) {
                    run += 8;
                } else {
                    ended(run);
                    run = 8;
                    bit = top;
                }
                continue;
            }
            if (top == bit) {
                run += g_runs.lead[b];
            } else {
                ended(run);
                run = g_runs.lead[b];
            }
            ended(run);  // the leading run ends inside this byte
            for (uint32_t k = 0; k < g_runs.innerCount[b]; ++k) check(g_runs.inner[b][k]);
            run = g_runs.trail[b];
            bit = b & 1u;
        }
        run_[c] = run;
        runBit_[c] = static_cast<uint8_t>(bit);
    }
    checkedBuffers_.fetch_add(1, std::memory_order_relaxed);
    if (bad) {
        violations_.fetch_add(bad, std::memory_order_relaxed);
        lastBadRun_.store(badRun, std::memory_order_relaxed);
        lastBadSource_.store(source, std::memory_order_relaxed);
    }
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
    if (const int64_t prev = lastSwitchQpc_.load(std::memory_order_relaxed);
        prev && double(now - prev) > 1.5 * opened_.bufferSamples / opened_.dsdRate * qpcFreq_)
        stalls_.fetch_add(1, std::memory_order_relaxed);
    if (!firstSwitchQpc_.load(std::memory_order_relaxed)) firstSwitchQpc_.store(now, std::memory_order_relaxed);
    lastSwitchQpc_.store(now, std::memory_order_relaxed);
    timedSwitches_.fetch_add(1, std::memory_order_relaxed);
    switchSeq_.fetch_add(1, std::memory_order_relaxed);
    uint32_t source = 0;
    bool skipCheck = false;
    {
        std::lock_guard lock(queueMutex_);
        // The producer before ended here (EndFeed): its stream ends with the emergency continuation of its last buffer
        // before anything the next producer queued, which starts a stream of its own (the epoch advanced already).
        const bool ended = tail_ == endSeq_ && emergencyReady_;
        if (tail_ == endSeq_) endSeq_ = ~uint64_t{0};
        if (rulesPending_) {
            if (!ended && tail_ < head_ && tail_ >= pendingSeq_) {  // the first buffer of the new stream
                minRun_ = pendingMin_;
                maxRun_ = pendingMax_;
                rulesPending_ = false;
                checkReset_ = true;
            } else {
                skipCheck = true;
            }
        }
        if (!ended && tail_ < head_) {
            CopySlot(Slot(tail_, false), index);
            ++tail_;
            emergencyReady_ = true;
        } else {
            // Nothing ready: never replay the stale half. The stream of the buffer played last fades out to silence,
            // and whatever the render thread built on the old stream is refused.
            const bool attached = emergencyReady_ && emergencySeq_[(tail_ - 1) % kSlots] == tail_ - 1;
            if (attached) CopySlot(Slot(tail_ - 1, true), index);
            else FillSilence(index);
            source = attached ? 1 : 2;
            if (!ended && emergencyReady_ && fed_) late_.fetch_add(1, std::memory_order_relaxed);
            if (!ended && emergencyReady_ && fed_ && !attached) missing_.fetch_add(1, std::memory_order_relaxed);
            emergencyReady_ = false;
            if (!ended) epoch_.fetch_add(1, std::memory_order_release);
        }
    }
    if (skipCheck) checkReset_ = true;
    else CheckBits(index, source);
    {
        const uint64_t h = tapHead_.load(std::memory_order_relaxed);
        const size_t slotBytes = size_t(bytesPerBuffer_) * buffers_.size();
        uint8_t* dst = tap_.data() + (h % kTapSlots) * slotBytes;
        for (size_t c = 0; c < buffers_.size(); ++c) std::memcpy(dst + c * bytesPerBuffer_, buffers_[c][index & 1], bytesPerBuffer_);
        tapSource_[h % kTapSlots] = source;
        tapHead_.store(h + 1, std::memory_order_release);
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
        case kAsioResyncRequest: resyncs_.fetch_add(1, std::memory_order_relaxed); return 1;
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
