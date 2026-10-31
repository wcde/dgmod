#pragma once

// ALSA playback of raw DSD (Linux): the first DSD format the device accepts out of DSD_U32_BE, DSD_U32_LE, DSD_U16_BE,
// DSD_U16_LE and DSD_U8, on a hw: device (no plug layer: the bits reach the USB driver untouched).

#include "Receiver.h"

#include <alsa/asoundlib.h>

#include <string>
#include <vector>

namespace dgmod::receiver {

class AlsaSink final : public Sink {
public:
    // `device`: an ALSA device name ("hw:1,0", "hw:CARD=K13,DEV=0"); empty = the first card with native DSD.
    explicit AlsaSink(std::string device) : wanted_(std::move(device)) {}
    ~AlsaSink() override { Close(); }

    bool Probe(net::Caps& caps, std::string& error) override;
    bool Open(uint32_t dsdMultiple, uint32_t channels, double periodMs, double bufferMs, std::string& error) override;
    void Close() override;
    [[nodiscard]] const DeviceFormat& Format() const override { return format_; }
    [[nodiscard]] uint32_t PeriodFrames() const override { return period_; }
    [[nodiscard]] uint32_t BufferFrames() const override { return buffer_; }
    long WaitSpace(int timeoutMs) override;
    long Delay() override;
    bool Write(const uint8_t* data, uint32_t frames) override;
    [[nodiscard]] uint64_t Xruns() const override { return xruns_; }

    // Every card's hw:N,0 and what it accepts (for --list).
    static std::vector<std::string> Describe();

private:
    bool Recover(int err);

    std::string wanted_, device_;
    snd_pcm_format_t alsaFormat_ = SND_PCM_FORMAT_UNKNOWN;
    DeviceFormat format_;
    snd_pcm_t* pcm_ = nullptr;
    uint32_t period_ = 0, buffer_ = 0, channels_ = 0;
    uint64_t xruns_ = 0;
};

}  // namespace dgmod::receiver
