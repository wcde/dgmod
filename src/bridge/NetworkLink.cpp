#include "bridge/NetworkLink.h"

#include <winsock2.h>
#include <ws2tcpip.h>
#include <avrt.h>

#include <algorithm>
#include <cstring>
#include <format>

namespace dgmod::bridge {

namespace {

constexpr uint32_t kStatusTimeoutMs = 1500;  // the receiver reports every ALSA period: this long silent = gone

bool WinsockReady() {
    static const bool ready = [] {
        WSADATA data{};
        return ::WSAStartup(MAKEWORD(2, 2), &data) == 0;
    }();
    return ready;
}

std::wstring SocketError(const wchar_t* what, int code = ::WSAGetLastError()) {
    return std::format(L"{} ({})", what, HResultText(HRESULT_FROM_WIN32(static_cast<DWORD>(code))));
}

template <class T>
std::span<const uint8_t> Bytes(const T& v) {
    return {reinterpret_cast<const uint8_t*>(&v), sizeof(T)};
}

std::wstring Text(const char* s, size_t n) { return Widen(std::string(s, strnlen(s, n))); }

}  // namespace

bool ParseNetworkAddress(const std::wstring& address, std::wstring& host, uint16_t& port) {
    host.clear();
    port = net::kDefaultPort;
    std::wstring rest;
    if (address.starts_with(L'[')) {
        const size_t close = address.find(L']');
        if (close == std::wstring::npos) return false;
        host = address.substr(1, close - 1);
        rest = address.substr(close + 1);
    } else if (std::count(address.begin(), address.end(), L':') == 1) {
        const size_t colon = address.find(L':');
        host = address.substr(0, colon);
        rest = address.substr(colon);
    } else {
        host = address;  // a name, an IPv4 address or a bare IPv6 address
    }
    if (!rest.empty()) {
        if (rest[0] != L':' || rest.size() < 2 || rest.size() > 6) return false;
        uint32_t p = 0;
        for (size_t i = 1; i < rest.size(); ++i) {
            if (rest[i] < L'0' || rest[i] > L'9') return false;
            p = p * 10 + static_cast<uint32_t>(rest[i] - L'0');
        }
        if (p == 0 || p > 65535) return false;
        port = static_cast<uint16_t>(p);
    }
    return !host.empty();
}

NetworkLink::~NetworkLink() { Close(); }

Result<net::Caps> NetworkLink::Connect(const std::wstring& address, uint32_t timeoutMs,
                                      std::function<void(const std::wstring&)> onFail) {
    Close();
    address_ = address;
    onFail_ = std::move(onFail);
    failed_ = started_ = quit_ = false;
    underruns_ = xruns_ = queued_ = 0;
    {
        std::lock_guard lock(errorMutex_);
        error_.clear();
    }
    std::wstring host;
    uint16_t port = 0;
    if (!ParseNetworkAddress(address, host, port))
        return dgmod::Fail(E_INVALIDARG, std::format(L"\"{}\" is not a network address (host or host:port)", address));
    if (!WinsockReady()) return dgmod::Fail(E_FAIL, L"Winsock is not available");

    addrinfoW hints{};
    hints.ai_family = AF_UNSPEC;
    hints.ai_socktype = SOCK_STREAM;
    hints.ai_protocol = IPPROTO_TCP;
    addrinfoW* found = nullptr;
    const std::wstring service = std::to_wstring(port);
    if (const int e = ::GetAddrInfoW(host.c_str(), service.c_str(), &hints, &found); e != 0)
        return dgmod::Fail(E_FAIL, SocketError(std::format(L"The receiver \"{}\" was not found", host).c_str(), e));
    std::wstring lastError = L"no address";
    for (addrinfoW* a = found; a && socket_ == INVALID_SOCKET; a = a->ai_next) {
        SOCKET s = ::socket(a->ai_family, a->ai_socktype, a->ai_protocol);
        if (s == INVALID_SOCKET) continue;
        u_long nonBlocking = 1;
        ::ioctlsocket(s, FIONBIO, &nonBlocking);
        bool connected = ::connect(s, a->ai_addr, static_cast<int>(a->ai_addrlen)) == 0;
        if (!connected && ::WSAGetLastError() == WSAEWOULDBLOCK) {
            fd_set writable, failed;
            FD_ZERO(&writable);
            FD_ZERO(&failed);
            FD_SET(s, &writable);
            FD_SET(s, &failed);
            timeval tv{static_cast<long>(timeoutMs / 1000), static_cast<long>(timeoutMs % 1000 * 1000)};
            if (::select(0, nullptr, &writable, &failed, &tv) > 0 && FD_ISSET(s, &writable)) {
                connected = true;
            } else {
                int code = WSAETIMEDOUT, len = sizeof(code);
                if (FD_ISSET(s, &failed)) ::getsockopt(s, SOL_SOCKET, SO_ERROR, reinterpret_cast<char*>(&code), &len);
                lastError = SocketError(L"connection failed", code);
            }
        } else if (!connected) {
            lastError = SocketError(L"connection failed");
        }
        if (!connected) {
            ::closesocket(s);
            continue;
        }
        nonBlocking = 0;
        ::ioctlsocket(s, FIONBIO, &nonBlocking);
        BOOL noDelay = TRUE;
        ::setsockopt(s, IPPROTO_TCP, TCP_NODELAY, reinterpret_cast<const char*>(&noDelay), sizeof(noDelay));
        socket_ = s;
    }
    ::FreeAddrInfoW(found);
    if (socket_ == INVALID_SOCKET)
        return dgmod::Fail(E_FAIL, std::format(L"The receiver at {} did not answer: {} (is dgmod-receiver running?)", address,
                                               lastError));

    if (!SendMsg(net::MsgType::Hello, Bytes(net::Hello{}))) return dgmod::Fail(E_FAIL, Error());
    net::Header h;
    std::vector<uint8_t> payload;
    if (!ReceiveMessage(h, payload, timeoutMs)) return dgmod::Fail(E_FAIL, Error());
    net::Caps caps;
    if (h.type != static_cast<uint32_t>(net::MsgType::Caps) || payload.size() < sizeof(caps)) {
        Close();
        return dgmod::Fail(E_FAIL, std::format(L"{} is not a dgmod receiver", address));
    }
    std::memcpy(&caps, payload.data(), sizeof(caps));
    if (caps.magic != net::kMagic || caps.version != net::kVersion) {
        Close();
        return dgmod::Fail(E_FAIL, std::format(L"The receiver at {} speaks protocol version {} (this bridge: {}): update it",
                                               address, caps.version, net::kVersion));
    }
    return caps;
}

Result<net::Opened> NetworkLink::Open(const net::Open& request, std::function<const uint8_t*(long)> fill) {
    if (socket_ == INVALID_SOCKET) return dgmod::Fail(E_UNEXPECTED, L"Network: not connected");
    open_ = request;
    fill_ = std::move(fill);
    if (!SendMsg(net::MsgType::Open, Bytes(request))) return dgmod::Fail(E_FAIL, Error());
    net::Header h;
    std::vector<uint8_t> payload;
    // Opening the DAC can take a while (the USB device switches its alternate setting).
    if (!ReceiveMessage(h, payload, 5000)) return dgmod::Fail(E_FAIL, Error());
    net::Opened o;
    if (h.type != static_cast<uint32_t>(net::MsgType::Opened) || payload.size() < sizeof(o))
        return dgmod::Fail(E_FAIL, L"The receiver answered Open with an unexpected message");
    std::memcpy(&o, payload.data(), sizeof(o));
    if (o.status != 0)
        return dgmod::Fail(E_FAIL, std::format(L"The receiver could not open its DAC: {}", Text(o.message, sizeof(o.message))));
    pump_ = std::thread([this] { Pump(); });
    return o;
}

bool NetworkLink::SendIdle(std::span<const uint8_t> idle) { return SendMsg(net::MsgType::Idle, idle); }

void NetworkLink::Close() {
    quit_ = true;
    if (socket_ != INVALID_SOCKET) ::shutdown(socket_, SD_BOTH);
    if (pump_.joinable()) pump_.join();
    if (socket_ != INVALID_SOCKET) ::closesocket(socket_);
    socket_ = INVALID_SOCKET;
    started_ = false;
}

std::wstring NetworkLink::Error() const {
    std::lock_guard lock(errorMutex_);
    return error_;
}

void NetworkLink::SetFailed(const std::wstring& why) {
    if (quit_.load(std::memory_order_relaxed)) return;  // closing on purpose
    {
        std::lock_guard lock(errorMutex_);
        if (error_.empty()) error_ = why;
    }
    if (failed_.exchange(true, std::memory_order_acq_rel)) return;
    if (onFail_) onFail_(why);
}

bool NetworkLink::SendMsg(net::MsgType type, std::span<const uint8_t> a, std::span<const uint8_t> b) {
    const net::Header h{static_cast<uint32_t>(type), static_cast<uint32_t>(a.size() + b.size())};
    std::lock_guard lock(sendMutex_);
    if (socket_ == INVALID_SOCKET) return false;
    WSABUF bufs[3] = {{sizeof(h), reinterpret_cast<char*>(const_cast<net::Header*>(&h))},
                      {static_cast<ULONG>(a.size()), reinterpret_cast<char*>(const_cast<uint8_t*>(a.data()))},
                      {static_cast<ULONG>(b.size()), reinterpret_cast<char*>(const_cast<uint8_t*>(b.data()))}};
    DWORD count = b.empty() ? 2 : 3;
    WSABUF* next = bufs;
    while (count) {  // a blocking WSASend sends everything unless the connection fails; loop for safety
        DWORD sent = 0;
        if (::WSASend(socket_, next, count, &sent, 0, nullptr, nullptr) != 0) {
            SetFailed(SocketError(std::format(L"The connection to the receiver at {} was lost", address_).c_str()));
            return false;
        }
        while (count && sent >= next->len) {
            sent -= next->len;
            ++next;
            --count;
        }
        if (count) {
            next->buf += sent;
            next->len -= sent;
        }
    }
    return true;
}

bool NetworkLink::Receive(void* dst, size_t bytes, uint32_t timeoutMs) {
    auto* p = static_cast<char*>(dst);
    while (bytes) {
        fd_set readable;
        FD_ZERO(&readable);
        FD_SET(socket_, &readable);
        timeval tv{static_cast<long>(timeoutMs / 1000), static_cast<long>(timeoutMs % 1000 * 1000)};
        const int ready = ::select(0, &readable, nullptr, nullptr, &tv);
        if (ready == 0) {
            SetFailed(std::format(L"The receiver at {} stopped responding", address_));
            return false;
        }
        const int got = ready > 0 ? ::recv(socket_, p, static_cast<int>(std::min<size_t>(bytes, 1 << 20)), 0) : -1;
        if (got <= 0) {
            SetFailed(got == 0 ? std::format(L"The receiver at {} closed the connection", address_)
                               : SocketError(std::format(L"The connection to the receiver at {} was lost", address_).c_str()));
            return false;
        }
        p += got;
        bytes -= static_cast<size_t>(got);
    }
    return true;
}

bool NetworkLink::ReceiveMessage(net::Header& h, std::vector<uint8_t>& payload, uint32_t timeoutMs) {
    if (!Receive(&h, sizeof(h), timeoutMs)) return false;
    if (h.size > net::kMaxPayload) {
        SetFailed(std::format(L"The receiver at {} sent a malformed message", address_));
        return false;
    }
    payload.resize(h.size);
    return h.size == 0 || Receive(payload.data(), h.size, timeoutMs);
}

void NetworkLink::Pump() {
    ::SetThreadDescription(::GetCurrentThread(), L"dgmod network");
    DWORD taskIndex = 0;
    HANDLE mmcss = ::AvSetMmThreadCharacteristicsW(L"Pro Audio", &taskIndex);
    if (mmcss) ::AvSetMmThreadPriority(mmcss, AVRT_PRIORITY_HIGH);
    uint64_t sent = 0;
    long half = 0;
    net::Header h;
    std::vector<uint8_t> payload;
    const size_t bufferBytes = size_t(open_.bytesPerBuffer) * open_.channels;
    while (!quit_.load(std::memory_order_relaxed)) {
        if (!ReceiveMessage(h, payload, kStatusTimeoutMs)) break;
        if (h.type != static_cast<uint32_t>(net::MsgType::Status) || payload.size() < sizeof(net::Status)) continue;
        net::Status st;
        std::memcpy(&st, payload.data(), sizeof(st));
        underruns_.store(st.underruns, std::memory_order_relaxed);
        xruns_.store(st.xruns, std::memory_order_relaxed);
        queued_.store(st.queuedBytes, std::memory_order_relaxed);
        if (!started_.load(std::memory_order_acquire)) continue;
        // In flight: sent but not received when the report left, plus what was still queued there. Reports that waited
        // in the socket overstate it (the receiver played on meanwhile), so a late pump never overfills the receiver.
        uint64_t inFlight = (sent - std::min(sent, st.received)) * open_.bytesPerBuffer + st.queuedBytes;
        const uint64_t target = target_.load(std::memory_order_relaxed);
        while (inFlight + open_.bytesPerBuffer <= target && !quit_.load(std::memory_order_relaxed)) {
            const uint8_t* data = fill_(half);
            if (!SendMsg(net::MsgType::Data, {data, bufferBytes})) break;
            ++sent;
            half ^= 1;
            inFlight += open_.bytesPerBuffer;
        }
    }
    if (mmcss) ::AvRevertMmThreadCharacteristics(mmcss);
}

}  // namespace dgmod::bridge
