#include "Receiver.h"

#include <algorithm>
#include <cstdarg>
#include <cstdio>
#include <cstring>
#include <ctime>

#ifdef _WIN32
#include <winsock2.h>
#include <ws2tcpip.h>
using Socket = SOCKET;
constexpr int kSendFlags = 0;
constexpr int kStatusFlags = 0;
static int CloseSocket(Socket s) { return ::closesocket(s); }
static int PollSockets(pollfd* fds, unsigned n, int ms) { return ::WSAPoll(fds, n, ms); }
#else
#include <arpa/inet.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <poll.h>
#include <sys/socket.h>
#include <unistd.h>
using Socket = int;
constexpr int kSendFlags = MSG_NOSIGNAL;
constexpr int kStatusFlags = MSG_NOSIGNAL | MSG_DONTWAIT;  // the audio thread never blocks on the network
static int CloseSocket(Socket s) { return ::close(s); }
static int PollSockets(pollfd* fds, unsigned n, int ms) { return ::poll(fds, n, ms); }
#endif

namespace dgmod::receiver {

namespace {

constexpr intptr_t kNone = -1;
Socket S(intptr_t s) { return static_cast<Socket>(s); }

constexpr uint32_t DsdMaskBit(uint32_t dsdMultiple) {
    uint32_t bit = 0;
    for (uint32_t m = dsdMultiple / 64; m > 1; m >>= 1) ++bit;
    return 1u << bit;
}

// Reads exactly `n` bytes, waiting up to `timeoutMs` for each part. False: closed, failed or timed out.
bool RecvAll(intptr_t s, void* dst, size_t n, int timeoutMs) {
    auto* p = static_cast<char*>(dst);
    while (n) {
        pollfd fd{};
        fd.fd = S(s);
        fd.events = POLLIN;
        if (PollSockets(&fd, 1, timeoutMs) <= 0) return false;
        const auto got = ::recv(S(s), p, static_cast<int>(std::min<size_t>(n, 1 << 20)), 0);
        if (got <= 0) return false;
        p += got;
        n -= static_cast<size_t>(got);
    }
    return true;
}

}  // namespace

// ---- Formats ----

void ToDevice(const uint8_t* planar, uint32_t bytesPerChannel, uint32_t channels, const DeviceFormat& f, uint8_t* dst) {
    const uint32_t b = f.sampleBytes, groups = bytesPerChannel / b;
    for (uint32_t c = 0; c < channels; ++c) {
        const uint8_t* src = planar + size_t(c) * bytesPerChannel;
        uint8_t* out = dst + size_t(c) * b;
        for (uint32_t g = 0; g < groups; ++g, src += b, out += size_t(channels) * b)
            for (uint32_t k = 0; k < b; ++k) out[f.littleEndian ? b - 1 - k : k] = src[k];
    }
}

void FromDevice(const uint8_t* device, uint32_t bytesPerChannel, uint32_t channels, const DeviceFormat& f, uint8_t* planar) {
    const uint32_t b = f.sampleBytes, groups = bytesPerChannel / b;
    for (uint32_t c = 0; c < channels; ++c) {
        uint8_t* dst = planar + size_t(c) * bytesPerChannel;
        const uint8_t* in = device + size_t(c) * b;
        for (uint32_t g = 0; g < groups; ++g, dst += b, in += size_t(channels) * b)
            for (uint32_t k = 0; k < b; ++k) dst[k] = in[f.littleEndian ? b - 1 - k : k];
    }
}

// ---- ByteRing ----

void ByteRing::Reset(size_t capacity) {
    data_.assign(capacity, 0);
    head_ = size_ = 0;
}

bool ByteRing::Push(const uint8_t* src, size_t n) {
    if (n > Free()) return false;
    size_t tail = (head_ + size_) % data_.size();
    const size_t first = std::min(n, data_.size() - tail);
    std::memcpy(data_.data() + tail, src, first);
    std::memcpy(data_.data(), src + first, n - first);
    size_ += n;
    return true;
}

size_t ByteRing::Pop(uint8_t* dst, size_t n) {
    n = std::min(n, size_);
    const size_t first = std::min(n, data_.size() - head_);
    std::memcpy(dst, data_.data() + head_, first);
    std::memcpy(dst + first, data_.data(), n - first);
    head_ = (head_ + n) % data_.size();
    size_ -= n;
    return n;
}

// ---- Receiver ----

Receiver::Receiver(Sink& sink, const Options& options) : sink_(sink), opt_(options) {
#ifdef _WIN32
    WSADATA data{};
    ::WSAStartup(MAKEWORD(2, 2), &data);
#endif
}

Receiver::~Receiver() {
    DropClient(nullptr);
    CloseDevice();
    if (listen_ != kNone) CloseSocket(S(listen_));
}

void Receiver::Log(const char* fmt, ...) {
    char text[512];
    va_list args;
    va_start(args, fmt);
    std::vsnprintf(text, sizeof(text), fmt, args);
    va_end(args);
    std::fprintf(stderr, "dgmod-receiver: %s\n", text);
    std::fflush(stderr);
}

bool Receiver::Listen(std::string& error) {
    // Dual-stack IPv6 (IPv4 too), else IPv4 only.
    Socket s = opt_.loopbackOnly ? static_cast<Socket>(kNone) : ::socket(AF_INET6, SOCK_STREAM, IPPROTO_TCP);
    bool v6 = s != static_cast<Socket>(kNone);
    if (!v6) s = ::socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    if (s == static_cast<Socket>(kNone)) {
        error = "socket() failed";
        return false;
    }
    int on = 1, off = 0;
    ::setsockopt(s, SOL_SOCKET, SO_REUSEADDR, reinterpret_cast<const char*>(&on), sizeof(on));
    sockaddr_storage addr{};
    socklen_t len = 0;
    if (v6) {
        ::setsockopt(s, IPPROTO_IPV6, IPV6_V6ONLY, reinterpret_cast<const char*>(&off), sizeof(off));
        auto* a = reinterpret_cast<sockaddr_in6*>(&addr);
        a->sin6_family = AF_INET6;
        a->sin6_addr = in6addr_any;
        a->sin6_port = htons(opt_.port);
        len = sizeof(sockaddr_in6);
    } else {
        auto* a = reinterpret_cast<sockaddr_in*>(&addr);
        a->sin_family = AF_INET;
        a->sin_addr.s_addr = htonl(opt_.loopbackOnly ? INADDR_LOOPBACK : INADDR_ANY);
        a->sin_port = htons(opt_.port);
        len = sizeof(sockaddr_in);
    }
    if (::bind(s, reinterpret_cast<sockaddr*>(&addr), len) != 0 || ::listen(s, 4) != 0) {
        error = "cannot listen on port " + std::to_string(opt_.port) + " (in use?)";
        CloseSocket(s);
        return false;
    }
    if (::getsockname(s, reinterpret_cast<sockaddr*>(&addr), &len) == 0)
        port_ = ntohs(v6 ? reinterpret_cast<sockaddr_in6*>(&addr)->sin6_port : reinterpret_cast<sockaddr_in*>(&addr)->sin_port);
    listen_ = static_cast<intptr_t>(s);
    return true;
}

void Receiver::Run() {
    Log("listening on port %u", static_cast<unsigned>(port_));
    while (!stop_.load()) {
        pollfd fds[2]{};
        fds[0].fd = S(listen_);
        fds[0].events = POLLIN;
        unsigned n = 1;
        if (client_ != kNone) {
            fds[1].fd = S(client_);
            fds[1].events = POLLIN;
            n = 2;
        }
        const int ready = PollSockets(fds, n, 200);
        if (ready < 0) continue;  // EINTR
        if (fds[0].revents & POLLIN) {
            sockaddr_storage from{};
            socklen_t len = sizeof(from);
            const Socket s = ::accept(S(listen_), reinterpret_cast<sockaddr*>(&from), &len);
            if (s != static_cast<Socket>(kNone)) {
                if (client_ != kNone) DropClient("a new sender connected");
                int on = 1;
                ::setsockopt(s, IPPROTO_TCP, TCP_NODELAY, reinterpret_cast<const char*>(&on), sizeof(on));
                char host[64] = "?";
                if (from.ss_family == AF_INET6)
                    ::inet_ntop(AF_INET6, &reinterpret_cast<sockaddr_in6*>(&from)->sin6_addr, host, sizeof(host));
                else
                    ::inet_ntop(AF_INET, &reinterpret_cast<sockaddr_in*>(&from)->sin_addr, host, sizeof(host));
                client_ = static_cast<intptr_t>(s);
                Log("sender connected from %s", host);
            }
        }
        if (n == 2 && client_ != kNone && (fds[1].revents & (POLLIN | POLLHUP | POLLERR))) {
            if (!Serve()) DropClient("the sender disconnected");
        }
        if (client_ == kNone && DeviceOpen() &&
            Clock::now() - clientGone_ > std::chrono::duration<double>(opt_.holdSec)) {
            Log("no sender for %.0f s: closing the device", opt_.holdSec);
            CloseDevice();
        }
    }
}

void Receiver::DropClient(const char* why) {
    if (client_ == kNone) return;
    {
        std::lock_guard lock(sendMutex_);
        statusTo_ = kNone;
    }
    CloseSocket(S(client_));
    client_ = kNone;
    {
        std::lock_guard lock(m_);
        streaming_ = false;  // silence from now on is not an under-run
    }
    clientGone_ = Clock::now();
    if (why) Log("%s", why);
}

bool Receiver::Serve() {
    net::Header h;
    if (!RecvAll(client_, &h, sizeof(h), 2000)) return false;
    if (h.size > net::kMaxPayload) {
        Log("malformed message (%u bytes)", h.size);
        return false;
    }
    payload_.resize(h.size);
    if (h.size && !RecvAll(client_, payload_.data(), h.size, 2000)) return false;
    switch (static_cast<net::MsgType>(h.type)) {
        case net::MsgType::Hello: {
            net::Hello hello{};
            std::memcpy(&hello, payload_.data(), std::min<size_t>(sizeof(hello), payload_.size()));
            if (hello.magic != net::kMagic) {
                Log("not a dgmod sender");
                return false;
            }
            HandleHello();  // a sender of another version learns ours from Caps and gives up
            return true;
        }
        case net::MsgType::Open: {
            if (payload_.size() < sizeof(net::Open)) return false;
            net::Open request;
            std::memcpy(&request, payload_.data(), sizeof(request));
            HandleOpen(request);
            return true;
        }
        case net::MsgType::Idle: {
            std::lock_guard lock(m_);
            if (!DeviceOpen() || payload_.size() != bytesPerBuffer_) return true;
            std::vector<uint8_t> planar(size_t(bytesPerBuffer_) * channels_);
            for (uint32_t c = 0; c < channels_; ++c)
                std::memcpy(planar.data() + size_t(c) * bytesPerBuffer_, payload_.data(), bytesPerBuffer_);
            ToDevice(planar.data(), bytesPerBuffer_, channels_, format_, idle_.data());
            idlePos_ = 0;
            return true;
        }
        case net::MsgType::Data: {
            if (!DeviceOpen() || payload_.size() != size_t(bytesPerBuffer_) * channels_) return true;
            converted_.resize(payload_.size());
            ToDevice(payload_.data(), bytesPerBuffer_, channels_, format_, converted_.data());
            {
                std::lock_guard lock(m_);
                if (!ring_.Push(converted_.data(), converted_.size())) ++overflows_;
                streaming_ = true;
            }
            ++received_;
            dataCv_.notify_one();
            return true;
        }
        default: return true;  // unknown messages are ignored (newer senders)
    }
}

void Receiver::HandleHello() {
    net::Caps caps;
    std::string error;
    if (DeviceOpen()) {
        caps = caps_;
    } else if (!sink_.Probe(caps, error)) {
        caps = {};
        std::snprintf(caps.device, sizeof(caps.device), "%s", error.c_str());
        Log("no DSD device: %s", error.c_str());
    }
    caps.magic = net::kMagic;
    caps.version = net::kVersion;
    if (DeviceOpen() || caps.dsdMask) caps_ = caps;
    SendMsg(net::MsgType::Caps, &caps, sizeof(caps), false);
}

void Receiver::HandleOpen(const net::Open& r) {
    net::Opened reply;
    auto refuse = [&](const std::string& why) {
        reply.status = 1;
        std::snprintf(reply.message, sizeof(reply.message), "%s", why.c_str());
        Log("open refused: %s", why.c_str());
        SendMsg(net::MsgType::Opened, &reply, sizeof(reply), false);
    };
    if (!r.channels || r.channels > 64 || !r.bytesPerBuffer || r.bytesPerBuffer % 8 || r.bytesPerBuffer > (1u << 20) ||
        !(caps_.dsdMask & DsdMaskBit(r.dsdMultiple)) || r.channels > caps_.maxChannels)
        return refuse("unsupported request (DSD" + std::to_string(r.dsdMultiple) + ", " + std::to_string(r.channels) +
                      " channels, " + std::to_string(r.bytesPerBuffer) + " bytes per buffer)");
    const bool kept = DeviceOpen() && r.dsdMultiple == dsdMultiple_ && r.channels == channels_;
    if (!kept) {
        CloseDevice();
        std::string error;
        if (!sink_.Open(r.dsdMultiple, r.channels, opt_.periodMs, opt_.bufferMs, error)) return refuse(error);
        format_ = sink_.Format();
        dsdMultiple_ = r.dsdMultiple;
        channels_ = r.channels;
    }
    const size_t frameBytes = size_t(format_.sampleBytes) * channels_;
    {
        std::lock_guard lock(m_);
        if (bytesPerBuffer_ != r.bytesPerBuffer || !kept) {
            bytesPerBuffer_ = r.bytesPerBuffer;
            idle_.assign(size_t(bytesPerBuffer_) * channels_, uint8_t{0x69});
            idlePos_ = 0;
        }
        const double bytesPerSec = 44100.0 * dsdMultiple_ / 8.0 * channels_;
        ring_.Reset(std::max<size_t>(size_t(bytesPerSec * opt_.queueSec) / frameBytes * frameBytes, 16 * idle_.size()));
        streaming_ = false;
    }
    received_ = 0;
    underruns_ = 0;
    overflows_ = 0;
    if (!kept) {
        audioQuit_ = false;
        audio_ = std::thread([this] { AudioLoop(); });
    }
    reply.bufferBytes = sink_.BufferFrames() * format_.sampleBytes;
    reply.periodBytes = sink_.PeriodFrames() * format_.sampleBytes;
    reply.kept = kept ? 1 : 0;
    Log("playing DSD%u, %u channels as %s (period %u, buffer %u frames)%s", dsdMultiple_, channels_, format_.name.c_str(),
        sink_.PeriodFrames(), sink_.BufferFrames(), kept ? ", device kept running" : "");
    // Reports go to this sender only after it has its answer.
    std::lock_guard lock(sendMutex_);
    const net::Header h{static_cast<uint32_t>(net::MsgType::Opened), sizeof(reply)};
    std::vector<uint8_t> msg(sizeof(h) + sizeof(reply));
    std::memcpy(msg.data(), &h, sizeof(h));
    std::memcpy(msg.data() + sizeof(h), &reply, sizeof(reply));
    if (::send(S(client_), reinterpret_cast<const char*>(msg.data()), static_cast<int>(msg.size()), kSendFlags) ==
        static_cast<int>(msg.size()))
        statusTo_ = client_;
}

void Receiver::CloseDevice() {
    if (!audio_.joinable()) return;
    audioQuit_ = true;
    dataCv_.notify_all();
    audio_.join();
    sink_.Close();
    std::lock_guard lock(m_);
    ring_.Clear();
    streaming_ = false;
}

void Receiver::FillIdle(uint8_t* dst, size_t bytes) {
    while (bytes) {
        const size_t n = std::min(bytes, idle_.size() - idlePos_);
        std::memcpy(dst, idle_.data() + idlePos_, n);
        dst += n;
        bytes -= n;
        idlePos_ = (idlePos_ + n) % idle_.size();
    }
}

void Receiver::AudioLoop() {
    if (opt_.onAudioThread) opt_.onAudioThread();
    const uint32_t period = sink_.PeriodFrames();
    const size_t frameBytes = size_t(format_.sampleBytes) * channels_;
    const size_t periodBytes = period * frameBytes;
    std::vector<uint8_t> buffer(periodBytes);
    // Start with a full buffer of silence (the device starts once its buffer is full).
    for (uint32_t done = 0; done + period <= sink_.BufferFrames(); done += period) {
        {
            std::lock_guard lock(m_);
            FillIdle(buffer.data(), periodBytes);
        }
        if (!sink_.Write(buffer.data(), period)) break;
    }
    while (!audioQuit_.load(std::memory_order_relaxed)) {
        const long space = sink_.WaitSpace(100);
        if (space < 0) {
            Log("the device failed");
            break;
        }
        if (space < static_cast<long>(period)) continue;
        net::Status st;
        {
            std::unique_lock lock(m_);
            if (ring_.Size() < periodBytes && streaming_ && sink_.Delay() > static_cast<long>(period) * 3 / 2) {
                // Not enough yet, but the device has time left: wait a little for the network.
                dataCv_.wait_for(lock, std::chrono::microseconds(500));
                continue;
            }
            const size_t got = ring_.Pop(buffer.data(), periodBytes);
            if (got < periodBytes) {
                FillIdle(buffer.data() + got, periodBytes - got);
                if (streaming_) {
                    ++underruns_;
                    if (opt_.verbose) Log("queue ran dry (%zu of %zu bytes)", got, periodBytes);
                }
            } else {
                idlePos_ = 0;  // silence after data starts at the pattern's beginning
            }
            st.queuedBytes = ring_.Size() / channels_;
        }
        if (!sink_.Write(buffer.data(), period)) {
            Log("the device failed");
            break;
        }
        st.queuedBytes += static_cast<uint64_t>(std::max(0L, sink_.Delay())) * format_.sampleBytes;
        st.received = received_.load();
        st.underruns = underruns_.load();
        st.xruns = sink_.Xruns();
        SendMsg(net::MsgType::Status, &st, sizeof(st), true);
    }
}

bool Receiver::SendMsg(net::MsgType type, const void* payload, uint32_t size, bool fromAudio) {
    const net::Header h{static_cast<uint32_t>(type), size};
    char msg[256];
    if (sizeof(h) + size > sizeof(msg)) return false;
    std::memcpy(msg, &h, sizeof(h));
    std::memcpy(msg + sizeof(h), payload, size);
    std::lock_guard lock(sendMutex_);
    const intptr_t to = fromAudio ? statusTo_ : client_;
    if (to == kNone) return false;
    const int n = static_cast<int>(sizeof(h) + size);
    const auto sent = ::send(S(to), msg, n, fromAudio ? kStatusFlags : kSendFlags);
    if (sent <= 0) return false;  // the audio thread skips a report rather than wait for a full socket
    // A report cut in two would break the framing: finish it (rare, only with a nearly full socket buffer).
    return sent == n || ::send(S(to), msg + sent, n - static_cast<int>(sent), kSendFlags) == n - static_cast<int>(sent);
}

}  // namespace dgmod::receiver
