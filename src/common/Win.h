#pragma once

// Minimal Win32/COM helpers shared by the bridge and the app.
// Deliberately free of ATL/WRL/WIL: plain C++23 on top of the Windows SDK headers.

#include <windows.h>
#include <unknwn.h>

#include <cstdint>
#include <expected>
#include <string>
#include <string_view>
#include <utility>

namespace dgmod {

// Owning COM pointer with WRL-like ergonomics: operator& releases and yields T** (usable with IID_PPV_ARGS).
template <class T>
class ComPtr {
public:
    ComPtr() noexcept = default;
    ComPtr(std::nullptr_t) noexcept {}
    ComPtr(T* p) noexcept : p_(p) { if (p_) p_->AddRef(); }
    ComPtr(const ComPtr& o) noexcept : p_(o.p_) { if (p_) p_->AddRef(); }
    ComPtr(ComPtr&& o) noexcept : p_(std::exchange(o.p_, nullptr)) {}
    template <class U>
    ComPtr(const ComPtr<U>& o) noexcept : p_(o.Get()) { if (p_) p_->AddRef(); }
    ~ComPtr() { Reset(); }

    ComPtr& operator=(const ComPtr& o) noexcept {
        if (p_ != o.p_) { ComPtr tmp(o); Swap(tmp); }
        return *this;
    }
    ComPtr& operator=(ComPtr&& o) noexcept {
        if (this != &o) { Reset(); p_ = std::exchange(o.p_, nullptr); }
        return *this;
    }
    ComPtr& operator=(T* p) noexcept {
        if (p_ != p) { ComPtr tmp(p); Swap(tmp); }
        return *this;
    }
    ComPtr& operator=(std::nullptr_t) noexcept { Reset(); return *this; }

    [[nodiscard]] T* Get() const noexcept { return p_; }
    [[nodiscard]] T* operator->() const noexcept { return p_; }
    [[nodiscard]] T& operator*() const noexcept { return *p_; }
    explicit operator bool() const noexcept { return p_ != nullptr; }

    // Releases the current pointer and returns the address of the (now null) storage.
    [[nodiscard]] T** operator&() noexcept { Reset(); return &p_; }
    [[nodiscard]] T** ReleaseAndGetAddressOf() noexcept { Reset(); return &p_; }
    [[nodiscard]] T** GetAddressOf() noexcept { return &p_; }
    [[nodiscard]] T* const* GetAddressOf() const noexcept { return &p_; }

    void Reset() noexcept {
        if (T* p = std::exchange(p_, nullptr)) p->Release();
    }
    void Attach(T* p) noexcept { Reset(); p_ = p; }
    [[nodiscard]] T* Detach() noexcept { return std::exchange(p_, nullptr); }
    void Swap(ComPtr& o) noexcept { std::swap(p_, o.p_); }

    template <class U>
    HRESULT As(U** out) const noexcept {
        if (!out) return E_POINTER;
        *out = nullptr;
        return p_ ? p_->QueryInterface(__uuidof(U), reinterpret_cast<void**>(out)) : E_POINTER;
    }
    template <class U>
    HRESULT As(ComPtr<U>& out) const noexcept { return As(out.ReleaseAndGetAddressOf()); }

    HRESULT CopyTo(T** out) const noexcept {
        if (!out) return E_POINTER;
        *out = p_;
        if (p_) p_->AddRef();
        return S_OK;
    }

    friend bool operator==(const ComPtr& a, const ComPtr& b) noexcept { return a.p_ == b.p_; }
    friend bool operator==(const ComPtr& a, std::nullptr_t) noexcept { return a.p_ == nullptr; }

private:
    T* p_ = nullptr;
};

// Error carried by std::expected: HRESULT plus a short context string.
struct Error {
    HRESULT hr = E_FAIL;
    std::wstring what;

    [[nodiscard]] std::wstring Message() const;
};

template <class T>
using Result = std::expected<T, Error>;

[[nodiscard]] inline std::unexpected<Error> Fail(HRESULT hr, std::wstring what) {
    return std::unexpected(Error{hr, std::move(what)});
}

[[nodiscard]] inline std::unexpected<Error> FailWin32(std::wstring what, DWORD code = ::GetLastError()) {
    return std::unexpected(Error{HRESULT_FROM_WIN32(code), std::move(what)});
}

std::wstring HResultText(HRESULT hr);

// UTF-8 <-> UTF-16.
std::wstring Widen(std::string_view s);
std::string Narrow(std::wstring_view s);

// Generic RAII wrapper for handle-like values; Traits supplies Type, Invalid(), IsValid() and Close().
template <class Traits>
class UniqueResource {
public:
    using H = typename Traits::Type;

    UniqueResource() noexcept = default;
    explicit UniqueResource(H h) noexcept : h_(h) {}
    ~UniqueResource() { Reset(); }
    UniqueResource(const UniqueResource&) = delete;
    UniqueResource& operator=(const UniqueResource&) = delete;
    UniqueResource(UniqueResource&& o) noexcept : h_(std::exchange(o.h_, Traits::Invalid())) {}
    UniqueResource& operator=(UniqueResource&& o) noexcept {
        if (this != &o) { Reset(); h_ = std::exchange(o.h_, Traits::Invalid()); }
        return *this;
    }
    void Reset(H h = Traits::Invalid()) noexcept {
        if (Traits::IsValid(h_)) Traits::Close(h_);
        h_ = h;
    }
    [[nodiscard]] H Get() const noexcept { return h_; }
    [[nodiscard]] H* Put() noexcept { Reset(); return &h_; }
    [[nodiscard]] H Release() noexcept { return std::exchange(h_, Traits::Invalid()); }
    explicit operator bool() const noexcept { return Traits::IsValid(h_); }

private:
    H h_ = Traits::Invalid();
};

namespace detail {
template <class T, auto CloseFn>
struct NullTraits {
    using Type = T;
    static constexpr T Invalid() noexcept { return nullptr; }
    static bool IsValid(T h) noexcept { return h != nullptr; }
    static void Close(T h) noexcept { CloseFn(h); }
};
struct HandleTraits {
    using Type = HANDLE;
    static HANDLE Invalid() noexcept { return nullptr; }
    static bool IsValid(HANDLE h) noexcept { return h != nullptr && h != INVALID_HANDLE_VALUE; }
    static void Close(HANDLE h) noexcept { ::CloseHandle(h); }
};
inline void CloseKey(HKEY k) noexcept { ::RegCloseKey(k); }
inline void FreeCoTaskMem(void* p) noexcept { ::CoTaskMemFree(p); }
inline void UnmapView(void* p) noexcept { ::UnmapViewOfFile(p); }
inline void FreeLocal(HLOCAL p) noexcept { ::LocalFree(p); }
}  // namespace detail

// Accepts both nullptr and INVALID_HANDLE_VALUE as "empty".
using UniqueHandle = UniqueResource<detail::HandleTraits>;
using UniqueHKey = UniqueResource<detail::NullTraits<HKEY, detail::CloseKey>>;
using UniqueCoMem = UniqueResource<detail::NullTraits<void*, detail::FreeCoTaskMem>>;
using UniqueView = UniqueResource<detail::NullTraits<void*, detail::UnmapView>>;
using UniqueLocal = UniqueResource<detail::NullTraits<HLOCAL, detail::FreeLocal>>;

// CoInitializeEx scope.
class ComScope {
public:
    explicit ComScope(DWORD model = COINIT_MULTITHREADED) noexcept : hr_(::CoInitializeEx(nullptr, model)) {}
    ~ComScope() { if (SUCCEEDED(hr_)) ::CoUninitialize(); }
    ComScope(const ComScope&) = delete;
    ComScope& operator=(const ComScope&) = delete;
    [[nodiscard]] HRESULT Hr() const noexcept { return hr_; }

private:
    HRESULT hr_;
};

}  // namespace dgmod
