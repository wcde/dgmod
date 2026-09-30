#pragma once

// Native DSD output through an ASIO driver (ASIO 2.2 DSD extension: kAsioSetIoFormat = kASIODSDFormat, sample rate =
// DSD bit rate, 1-bit samples packed 8 per byte). The ASIO interface is declared here from its published ABI; no SDK
// headers are needed.
//
// Threading: ASIO drivers are apartment-threaded COM objects without proxies, so every driver call except
// outputReady() runs on a private STA thread owned by this class. The driver calls bufferSwitch() on its own thread.
//
// Click protection: the render thread works QueueDepth() buffers ahead of the driver. It pushes finished buffers into
// a small queue, each with an emergency continuation (the same stream faded out to DSD silence); the callback only
// copies the oldest one into the driver's half and wakes the render thread (SwitchEvent), so it never waits and a
// late render thread has QueueDepth() periods of slack instead of none. If the queue is empty all the same, the callback
// plays the emergency continuation of the buffer it played last, then DSD silence, and advances the epoch: buffers the
// render thread built for the old epoch no longer join seamlessly and are refused (Push returns false), so it restarts
// the modulator and fades in. The DAC therefore never gets a replayed buffer or a cut in the bit stream.

#include "common/Win.h"

#include <array>
#include <atomic>
#include <cstdint>
#include <memory>
#include <mutex>
#include <string>
#include <vector>

namespace dgmod::bridge {

// DSD words (16 bits, earliest sample in the MSB, `channels` interleaved) -> one channel of an ASIO DSD buffer:
// ASIOSTDSDInt8MSB1 (earliest sample in the MSB of each byte) or, with `lsbFirst`, ASIOSTDSDInt8LSB1.
void PackDsdChannel(const uint16_t* words, uint32_t frames, uint32_t channels, uint32_t channel, bool lsbFirst, uint8_t* dst);

class AsioDsdOutput {
public:
    struct Request {
        CLSID clsid{};
        std::wstring name;
        uint32_t dsdMultiple = 0;   // 64..1024; 0 = highest accepted up to kNativeDsdAutoMax
        uint32_t channels = 2;      // outputs to use (clamped to the driver's)
        double periodMs = 0;        // wanted buffer duration; 0 = the driver's preferred size (its control panel)
    };
    struct Opened {
        uint32_t dsdMultiple = 0;
        uint32_t dsdRate = 0;
        uint32_t channels = 0;        // channels in use
        uint32_t deviceChannels = 0;  // outputs of the driver
        uint32_t bufferSamples = 0;   // DSD samples per buffer half (a multiple of 16)
        uint32_t frames = 0;          // 16-bit words per channel and buffer half = bufferSamples / 16
        uint32_t supportedMask = 0;   // DsdMaskBit of every rate the driver accepts
        double outputLatencyMs = 0;   // reported by the driver (usually one buffer plus its own)
        bool lsbFirst = false;
        bool outputReady = false;     // the driver supports outputReady()
        std::wstring driverName;      // as the driver names itself
    };
    // Result of Probe(): what the driver offers without creating buffers.
    struct Capabilities {
        std::wstring driverName;
        uint32_t inputs = 0, outputs = 0;
        bool dsd = false;              // accepts kASIODSDFormat
        uint32_t supportedMask = 0;
        long minSize = 0, maxSize = 0, preferredSize = 0, granularity = 0;  // at the highest DSD rate
        long sampleType = -1;          // of output 0 in DSD mode
        std::wstring details;          // channels, clocks and rates as reported in PCM and DSD mode (diagnostics)
    };

    AsioDsdOutput();
    ~AsioDsdOutput();
    AsioDsdOutput(const AsioDsdOutput&) = delete;
    AsioDsdOutput& operator=(const AsioDsdOutput&) = delete;

    // Loads the driver, switches it to DSD, sets the rate, creates the buffers (both halves hold DSD silence).
    // `supportedMask` is filled even when opening fails later (0 if the driver has no DSD mode).
    Result<Opened> Open(const Request& request, uint32_t& supportedMask);
    Result<void> Start();
    // Stops the stream and unloads the driver (safe to call at any time).
    void Close();
    static Result<Capabilities> Probe(const CLSID& clsid);

    // Buffers the render thread keeps queued ahead of the driver (added latency: this many periods minus one): enough
    // for kQueueSlackMs of slack, at least one, at most kMaxQueueDepth.
    static constexpr uint32_t kMaxQueueDepth = 3;
    static constexpr double kQueueSlackMs = 20.0;
    [[nodiscard]] uint32_t QueueDepth() const { return depth_; }

    // Render thread.
    [[nodiscard]] HANDLE SwitchEvent() const { return switchEvent_.Get(); }  // set after every buffer switch
    [[nodiscard]] uint32_t Space() const;   // buffers that may be pushed now
    [[nodiscard]] uint32_t Queued() const;  // buffers not yet handed to the driver
    // Incremented whenever the driver found the queue empty (the stream was faded out to silence).
    [[nodiscard]] uint64_t Epoch() const { return epoch_.load(std::memory_order_acquire); }
    // Queues one period (`frames` DSD words per channel, `channels` interleaved) and its emergency continuation.
    // Returns false (nothing queued) if the epoch is no longer `epoch`: the buffer would not join what was played.
    bool Push(const uint16_t* words, const uint16_t* emergency, uint32_t channels, uint64_t epoch);

    // Any thread.
    [[nodiscard]] HANDLE ResetEvent() const { return resetEvent_.Get(); }  // the driver asked to be re-opened
    [[nodiscard]] std::wstring ResetReason() const;
    // Buffer switches that found no queued buffer after playback had begun (covered by a fade-out to silence).
    [[nodiscard]] uint64_t LateSwitches() const { return late_.load(std::memory_order_relaxed); }
    [[nodiscard]] uint64_t Overloads() const { return overloads_.load(std::memory_order_relaxed); }
    // Mean interval of the buffer switches since Start (0 until enough were seen), in seconds.
    [[nodiscard]] double MeasuredSwitchSec() const;
    [[nodiscard]] uint64_t Switches() const { return switchSeq_.load(std::memory_order_relaxed); }

private:
    struct Driver;
    class StaThread;

    void OnSwitch(long index);
    long OnMessage(long selector, long value);
    void RequestReset(const std::wstring& reason);
    void FillSilence(long index);
    // Queue slot i: channels x bytesPerBuffer_ packed bytes, the period followed by its emergency continuation.
    [[nodiscard]] uint8_t* Slot(uint64_t i, bool emergency) {
        return slots_.data() + ((i % kSlots) * 2 + (emergency ? 1 : 0)) * size_t(bytesPerBuffer_) * opened_.channels;
    }
    void CopySlot(const uint8_t* src, long index);
    static void CbBufferSwitch(long index, long directProcess);
    static void CbSampleRateChanged(double rate);
    static long CbMessage(long selector, long value, void* message, double* opt);
    static void* CbBufferSwitchTimeInfo(void* params, long index, long directProcess);

    std::unique_ptr<StaThread> sta_;
    Driver* driver_ = nullptr;  // IASIO*, used on the STA thread (and outputReady on the callback thread)
    HWND window_ = nullptr;     // sysHandle for init(), owned by the STA thread
    bool buffersCreated_ = false, started_ = false;
    Opened opened_{};
    std::vector<std::array<void*, 2>> buffers_;
    uint32_t bytesPerBuffer_ = 0;
    uint8_t silence_ = 0x69;

    // The queue: slots head_ - 1 .. tail_ are pushed and waiting; slot tail_ - 1 was played last and keeps its
    // emergency continuation, so the producer never reuses it (one slot more than the depth plus that one).
    static constexpr uint32_t kSlots = kMaxQueueDepth + 2;
    uint32_t depth_ = 1;
    std::vector<uint8_t> slots_;
    mutable std::mutex queueMutex_;  // held for a few microseconds by either side
    uint64_t head_ = 0, tail_ = 0;
    bool emergencyReady_ = false;  // slot tail_ - 1 was played and its emergency continuation not yet
    std::atomic<uint64_t> epoch_{0};

    UniqueHandle switchEvent_, resetEvent_;
    std::atomic<uint64_t> switchSeq_{0}, late_{0}, overloads_{0};
    std::atomic<int64_t> firstSwitchQpc_{0}, lastSwitchQpc_{0};
    std::atomic<uint64_t> timedSwitches_{0};
    double qpcFreq_ = 1;
    mutable std::mutex reasonMutex_;
    std::wstring resetReason_;
};

}  // namespace dgmod::bridge
