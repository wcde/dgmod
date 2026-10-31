#pragma once

// Client side of the dgmod network DSD protocol (common/NetProtocol.h): a TCP connection to dgmod-receiver. After
// Open, a pump thread reads the receiver's Status reports and, once started, asks for one buffer at a time (the
// `fill` callback, the equivalent of an ASIO buffer switch) whenever the receiver has room for it: the buffers follow
// the DAC clock behind the receiver.

#include "common/NetProtocol.h"
#include "common/Win.h"

#include <atomic>
#include <cstdint>
#include <functional>
#include <mutex>
#include <span>
#include <string>
#include <thread>
#include <vector>

namespace dgmod::bridge {

// "host", "host:port", "[v6]" or "[v6]:port" -> host and port (kDefaultPort if none). False: not an address.
bool ParseNetworkAddress(const std::wstring& address, std::wstring& host, uint16_t& port);

class NetworkLink {
public:
    NetworkLink() = default;
    ~NetworkLink();
    NetworkLink(const NetworkLink&) = delete;
    NetworkLink& operator=(const NetworkLink&) = delete;

    // Connects and asks the receiver what its DAC accepts. `onFail` runs once (on any thread) when the connection is lost
    // or the receiver stops reporting.
    Result<net::Caps> Connect(const std::wstring& address, uint32_t timeoutMs, std::function<void(const std::wstring&)> onFail);
    // Opens the DAC in the receiver (it plays DSD silence from then on) and starts the pump thread. `fill(half)` must
    // return the buffer to send for that half (0/1, alternating): channels x bytesPerBuffer bytes, channel-planar.
    Result<net::Opened> Open(const net::Open& request, std::function<const uint8_t*(long half)> fill);
    // Hands buffers over from now on, keeping `targetBytes` per channel in flight (sent but not yet played).
    void Start(uint64_t targetBytes) {
        target_.store(targetBytes, std::memory_order_relaxed);
        started_.store(true, std::memory_order_release);
    }
    [[nodiscard]] bool Started() const { return started_.load(std::memory_order_acquire); }
    // The silence pattern the receiver repeats when no data is queued (one channel, bytesPerBuffer bytes).
    bool SendIdle(std::span<const uint8_t> idle);
    void Close();

    // The connection was lost or the receiver stopped reporting (Error() says why).
    [[nodiscard]] bool Failed() const { return failed_.load(std::memory_order_acquire); }
    [[nodiscard]] std::wstring Error() const;
    [[nodiscard]] const std::wstring& Address() const { return address_; }

    // Counters reported by the receiver.
    [[nodiscard]] uint64_t Underruns() const { return underruns_.load(std::memory_order_relaxed); }
    [[nodiscard]] uint64_t Xruns() const { return xruns_.load(std::memory_order_relaxed); }
    [[nodiscard]] uint64_t QueuedBytes() const { return queued_.load(std::memory_order_relaxed); }

private:
    void Pump();
    void SetFailed(const std::wstring& why);
    bool SendMsg(net::MsgType type, std::span<const uint8_t> a, std::span<const uint8_t> b = {});
    bool Receive(void* dst, size_t bytes, uint32_t timeoutMs);  // pump thread / before it runs
    bool ReceiveMessage(net::Header& h, std::vector<uint8_t>& payload, uint32_t timeoutMs);

    uintptr_t socket_ = ~uintptr_t{0};  // SOCKET
    std::wstring address_;
    net::Open open_{};
    std::function<const uint8_t*(long)> fill_;
    std::thread pump_;
    std::atomic<bool> quit_{false}, started_{false}, failed_{false};
    std::atomic<uint64_t> target_{0};
    std::atomic<uint64_t> underruns_{0}, xruns_{0}, queued_{0};
    std::mutex sendMutex_;
    mutable std::mutex errorMutex_;
    std::wstring error_;
    std::function<void(const std::wstring&)> onFail_;
};

}  // namespace dgmod::bridge
