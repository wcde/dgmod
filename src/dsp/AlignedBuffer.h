#pragma once

#include <cstddef>
#include <cstring>
#include <new>
#include <type_traits>
#include <utility>

namespace dgmod::dsp {

// Fixed-size, 64-byte aligned, zero-initialized buffer of trivially copyable elements.
template <class T>
class AlignedBuffer {
    static_assert(std::is_trivially_copyable_v<T>);

public:
    static constexpr std::align_val_t kAlignment{64};

    AlignedBuffer() noexcept = default;
    explicit AlignedBuffer(size_t count) { Allocate(count); }
    ~AlignedBuffer() { Free(); }
    AlignedBuffer(const AlignedBuffer&) = delete;
    AlignedBuffer& operator=(const AlignedBuffer&) = delete;
    AlignedBuffer(AlignedBuffer&& o) noexcept : data_(std::exchange(o.data_, nullptr)), size_(std::exchange(o.size_, 0)) {}
    AlignedBuffer& operator=(AlignedBuffer&& o) noexcept {
        if (this != &o) {
            Free();
            data_ = std::exchange(o.data_, nullptr);
            size_ = std::exchange(o.size_, 0);
        }
        return *this;
    }

    void Allocate(size_t count) {
        Free();
        if (count == 0) return;
        data_ = static_cast<T*>(::operator new(count * sizeof(T), kAlignment));
        size_ = count;
        std::memset(static_cast<void*>(data_), 0, count * sizeof(T));
    }
    void Free() noexcept {
        if (data_) ::operator delete(data_, kAlignment);
        data_ = nullptr;
        size_ = 0;
    }
    void Zero() noexcept {
        if (data_) std::memset(static_cast<void*>(data_), 0, size_ * sizeof(T));
    }

    [[nodiscard]] T* data() noexcept { return data_; }
    [[nodiscard]] const T* data() const noexcept { return data_; }
    [[nodiscard]] size_t size() const noexcept { return size_; }
    [[nodiscard]] size_t bytes() const noexcept { return size_ * sizeof(T); }
    T& operator[](size_t i) noexcept { return data_[i]; }
    const T& operator[](size_t i) const noexcept { return data_[i]; }

private:
    T* data_ = nullptr;
    size_t size_ = 0;
};

}  // namespace dgmod::dsp
