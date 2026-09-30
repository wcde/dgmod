#pragma once

#include <algorithm>
#include <atomic>
#include <cstdint>
#include <cstring>
#include <vector>

namespace dgmod::bridge {

// Single-producer / single-consumer ring of interleaved float frames. Lock-free, real-time safe after Allocate().
class FrameFifo {
public:
    void Allocate(uint32_t channels, uint32_t capacityFrames) {
        channels_ = channels;
        capacity_ = capacityFrames;
        buffer_.assign(size_t(channels) * capacityFrames, 0.0f);
        write_.store(0);
        read_.store(0);
    }

    [[nodiscard]] uint32_t Channels() const { return channels_; }
    [[nodiscard]] uint32_t Capacity() const { return capacity_; }
    [[nodiscard]] uint32_t Available() const {
        return static_cast<uint32_t>(write_.load(std::memory_order_acquire) - read_.load(std::memory_order_acquire));
    }
    [[nodiscard]] uint32_t Space() const { return capacity_ - Available(); }
    [[nodiscard]] uint64_t TotalWritten() const { return write_.load(std::memory_order_acquire); }
    [[nodiscard]] uint64_t TotalRead() const { return read_.load(std::memory_order_acquire); }

    // Producer. Frames that do not fit are dropped; returns the number written.
    uint32_t Write(const float* frames, uint32_t n) { return Put(frames, n); }
    uint32_t WriteSilence(uint32_t n) { return Put(nullptr, n); }

    // Consumer. Returns the number of frames copied (and removed).
    uint32_t Read(float* dst, uint32_t n) {
        const uint64_t r = read_.load(std::memory_order_relaxed);
        n = std::min(n, static_cast<uint32_t>(write_.load(std::memory_order_acquire) - r));
        CopyOut(r, dst, n);
        read_.store(r + n, std::memory_order_release);
        return n;
    }
    uint32_t Skip(uint32_t n) {
        const uint64_t r = read_.load(std::memory_order_relaxed);
        n = std::min(n, static_cast<uint32_t>(write_.load(std::memory_order_acquire) - r));
        read_.store(r + n, std::memory_order_release);
        return n;
    }

private:
    uint32_t Put(const float* frames, uint32_t n) {
        const uint64_t w = write_.load(std::memory_order_relaxed);
        const uint32_t space = capacity_ - static_cast<uint32_t>(w - read_.load(std::memory_order_acquire));
        n = std::min(n, space);
        const uint32_t start = static_cast<uint32_t>(w % capacity_);
        const uint32_t first = std::min(n, capacity_ - start);
        const size_t ch = channels_;
        if (frames) {
            std::memcpy(buffer_.data() + start * ch, frames, first * ch * sizeof(float));
            std::memcpy(buffer_.data(), frames + first * ch, (n - first) * ch * sizeof(float));
        } else {
            std::memset(buffer_.data() + start * ch, 0, first * ch * sizeof(float));
            std::memset(buffer_.data(), 0, (n - first) * ch * sizeof(float));
        }
        write_.store(w + n, std::memory_order_release);
        return n;
    }
    void CopyOut(uint64_t r, float* dst, uint32_t n) const {
        const uint32_t start = static_cast<uint32_t>(r % capacity_);
        const uint32_t first = std::min(n, capacity_ - start);
        const size_t ch = channels_;
        std::memcpy(dst, buffer_.data() + start * ch, first * ch * sizeof(float));
        std::memcpy(dst + first * ch, buffer_.data(), (n - first) * ch * sizeof(float));
    }

    std::vector<float> buffer_;
    uint32_t channels_ = 0;
    uint32_t capacity_ = 0;
    std::atomic<uint64_t> write_{0};
    std::atomic<uint64_t> read_{0};
};

}  // namespace dgmod::bridge
