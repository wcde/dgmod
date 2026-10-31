#pragma once

// dgmod-receiver core: the server side of the dgmod network DSD protocol (common/NetProtocol.h). Portable C++20 (Linux
// and, for tests, Windows); the audio device is behind the Sink interface (AlsaSink.cpp on Linux).
//
// One sender at a time (a new connection replaces the old one). From Open on, an audio thread keeps the device fed
// period by period: with the sender's data, or with its DSD silence pattern when nothing is queued, and reports after
// every period how much is still queued. The sender paces its buffers by these reports, so the device clock is the
// master. When the sender is gone, the device keeps playing silence for Options::holdSec (a sender that reconnects in
// that time with the same format continues without the DAC re-locking), then it is closed.

#include "common/NetProtocol.h"

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

namespace dgmod::receiver {

// Sample layout of an ALSA DSD format: `sampleBytes` DSD bytes per channel and frame, earliest bits in the first byte
// (big-endian) or in the last one (little-endian).
struct DeviceFormat {
    std::string name;  // "DSD_U32_BE", ...
    uint32_t sampleBytes = 4;
    bool littleEndian = false;
};

class Sink {
public:
    virtual ~Sink() = default;
    // What the device accepts (Caps::dsdMask, maxChannels, device and format). False: no usable device (`error`).
    virtual bool Probe(net::Caps& caps, std::string& error) = 0;
    // Opens the device for DSD at 44.1 kHz x `dsdMultiple` with periods and a buffer near the given durations.
    virtual bool Open(uint32_t dsdMultiple, uint32_t channels, double periodMs, double bufferMs, std::string& error) = 0;
    virtual void Close() = 0;
    [[nodiscard]] virtual const DeviceFormat& Format() const = 0;
    [[nodiscard]] virtual uint32_t PeriodFrames() const = 0;
    [[nodiscard]] virtual uint32_t BufferFrames() const = 0;
    // Frames that can be written now, after waiting up to `timeoutMs` for at least one period; < 0: the device failed.
    virtual long WaitSpace(int timeoutMs) = 0;
    // Frames written but not yet played.
    virtual long Delay() = 0;
    // Writes `frames` interleaved frames (recovers from an under-run by itself). False: the device failed.
    virtual bool Write(const uint8_t* data, uint32_t frames) = 0;
    [[nodiscard]] virtual uint64_t Xruns() const = 0;
};

// Channel-planar DSD bytes (`channels` blocks of `bytesPerChannel`, earliest bit in the MSB) -> interleaved device
// samples. `bytesPerChannel` must be a multiple of format.sampleBytes.
void ToDevice(const uint8_t* planar, uint32_t bytesPerChannel, uint32_t channels, const DeviceFormat& format, uint8_t* dst);
// The inverse (tests).
void FromDevice(const uint8_t* device, uint32_t bytesPerChannel, uint32_t channels, const DeviceFormat& format, uint8_t* planar);

// Byte FIFO of fixed capacity (not thread-safe).
class ByteRing {
public:
    void Reset(size_t capacity);
    void Clear() { head_ = size_ = 0; }
    [[nodiscard]] size_t Size() const { return size_; }
    [[nodiscard]] size_t Free() const { return data_.size() - size_; }
    bool Push(const uint8_t* src, size_t n);  // false (nothing stored) if it does not fit
    size_t Pop(uint8_t* dst, size_t n);       // up to n bytes
private:
    std::vector<uint8_t> data_;
    size_t head_ = 0, size_ = 0;
};

struct Options {
    uint16_t port = net::kDefaultPort;  // 0: any free port (tests; Port() tells which)
    bool loopbackOnly = false;          // listen on 127.0.0.1 only (tests)
    double periodMs = 2.0;              // ALSA period: the report interval
    double bufferMs = 8.0;              // ALSA buffer
    double holdSec = 10.0;              // silence after the sender left, before the device is closed
    double queueSec = 1.0;              // capacity of the receive queue
    bool verbose = false;
    void (*onAudioThread)() = nullptr;  // called on the audio thread when it starts (real-time priority)
};

class Receiver {
public:
    Receiver(Sink& sink, const Options& options);
    ~Receiver();
    Receiver(const Receiver&) = delete;
    Receiver& operator=(const Receiver&) = delete;

    // Binds the listening socket. False: `error` says why.
    bool Listen(std::string& error);
    [[nodiscard]] uint16_t Port() const { return port_; }
    // Serves senders until Stop() (any thread, also from a signal handler).
    void Run();
    void Stop() { stop_.store(true); }

    // Statistics (any thread).
    [[nodiscard]] uint64_t Received() const { return received_.load(); }
    [[nodiscard]] uint64_t Underruns() const { return underruns_.load(); }
    [[nodiscard]] uint64_t Overflows() const { return overflows_.load(); }
    [[nodiscard]] bool DeviceOpen() const { return audio_.joinable(); }

private:
    using Clock = std::chrono::steady_clock;

    bool Serve();  // one message of the sender; false: drop it
    void DropClient(const char* why);
    void HandleHello();
    void HandleOpen(const net::Open& request);
    void CloseDevice();
    void AudioLoop();
    void FillIdle(uint8_t* dst, size_t bytes);  // m_ held
    bool SendMsg(net::MsgType type, const void* payload, uint32_t size, bool fromAudio);
    void Log(const char* fmt, ...);

    Sink& sink_;
    Options opt_;
    intptr_t listen_ = -1, client_ = -1;
    uint16_t port_ = 0;
    std::atomic<bool> stop_{false};
    std::vector<uint8_t> payload_;
    std::vector<uint8_t> converted_;
    Clock::time_point clientGone_{};

    // Device session (Open): format, the receive queue (device format) and the silence pattern.
    net::Caps caps_{};       // of the open device
    uint32_t dsdMultiple_ = 0, channels_ = 0, bytesPerBuffer_ = 0;
    DeviceFormat format_;
    std::mutex m_;
    std::condition_variable dataCv_;
    ByteRing ring_;
    std::vector<uint8_t> idle_;  // device format, one sender buffer of all channels
    size_t idlePos_ = 0;
    bool streaming_ = false;     // the sender's data is expected (a Data message arrived since Open)
    std::thread audio_;
    std::atomic<bool> audioQuit_{false};

    std::mutex sendMutex_;
    intptr_t statusTo_ = -1;  // sendMutex_: the client the audio thread reports to
    std::atomic<uint64_t> received_{0}, underruns_{0}, overflows_{0};
};

}  // namespace dgmod::receiver
