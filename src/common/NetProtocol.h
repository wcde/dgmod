#pragma once

// Wire protocol between dgmod-bridge (Windows, the sender) and dgmod-receiver (Linux, e.g. a Raspberry Pi with a USB
// DAC). Shared by both sides: plain C++, no platform headers.
//
// One TCP connection, messages framed as NetHeader + payload, all integers little-endian (both ends are). The sender
// produces the DSD bit stream; the receiver plays it through ALSA (DSD_U32_BE and friends) and is the clock master:
// it reports after every ALSA period how much is still queued, and the sender hands over a new buffer only while less
// than its target is in flight. The buffer pace therefore follows the DAC clock, like the buffer switches of an ASIO
// driver.
//
//   sender                         receiver
//   Hello            ->
//                    <-            Caps    (DSD rates and channels the device accepts)
//   Open             ->
//                    <-            Opened  (the device plays DSD silence from now on)
//   Idle             ->            (the silence pattern: repeated whenever no data is queued)
//   Data, Data, ...  ->
//                    <-            Status, Status, ... (every ALSA period)
//
// Payloads of Idle and Data are DSD bytes, 8 samples per byte with the earliest sample in the MSB. Data holds one buffer:
// `channels` blocks of Open::bytesPerBuffer bytes each (channel-planar); Idle holds one such block (all channels).

#include <cstdint>

namespace dgmod::net {

inline constexpr uint16_t kDefaultPort = 47300;
inline constexpr uint32_t kMagic = 0x414E4744;  // "DGNA"
inline constexpr uint32_t kVersion = 1;
inline constexpr uint32_t kMaxPayload = 4u << 20;

enum class MsgType : uint32_t { Hello = 1, Caps = 2, Open = 3, Opened = 4, Idle = 5, Data = 6, Status = 7 };

struct Header {
    uint32_t type = 0;  // MsgType
    uint32_t size = 0;  // payload bytes that follow
};

struct Hello {
    uint32_t magic = kMagic;
    uint32_t version = kVersion;
};

struct Caps {
    uint32_t magic = kMagic;
    uint32_t version = kVersion;
    uint32_t dsdMask = 0;      // DsdMaskBit of every DSD rate the device accepts (DSD64 = bit 0 ... DSD1024 = bit 4)
    uint32_t maxChannels = 0;  // the device's channels in DSD mode
    char device[64]{};         // UTF-8 name of the ALSA card
    char format[16]{};         // ALSA sample format in use ("DSD_U32_BE", ...)
};

struct Open {
    uint32_t dsdMultiple = 0;     // DSD rate / 44.1 kHz
    uint32_t channels = 0;
    uint32_t bytesPerBuffer = 0;  // per channel and Data message; a multiple of 8
};

struct Opened {
    uint32_t status = 0;          // 0 = playing DSD silence; otherwise `message` says why not
    uint32_t bufferBytes = 0;     // ALSA buffer per channel
    uint32_t periodBytes = 0;     // ALSA period per channel (the Status interval)
    uint32_t kept = 0;            // the device was already running in this format (no re-lock of the DAC)
    char message[128]{};          // UTF-8
};

struct Status {
    uint64_t received = 0;    // Data messages received since Opened
    uint64_t queuedBytes = 0; // per channel: bytes waiting in the receiver (its queue plus the ALSA buffer)
    uint64_t underruns = 0;   // periods the queue ran dry while data was expected (silence was inserted)
    uint64_t xruns = 0;       // ALSA under-runs (the receiver itself was late)
};

static_assert(sizeof(Header) == 8 && sizeof(Hello) == 8 && sizeof(Caps) == 96 && sizeof(Open) == 12 &&
              sizeof(Opened) == 144 && sizeof(Status) == 32);

}  // namespace dgmod::net
