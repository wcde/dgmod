#include "AlsaSink.h"

#include <algorithm>
#include <cerrno>
#include <iterator>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>

namespace dgmod::receiver {

namespace {

struct Candidate {
    snd_pcm_format_t alsa;
    const char* name;
    uint32_t sampleBytes;
    bool littleEndian;
};
constexpr Candidate kFormats[] = {
    {SND_PCM_FORMAT_DSD_U32_BE, "DSD_U32_BE", 4, false}, {SND_PCM_FORMAT_DSD_U32_LE, "DSD_U32_LE", 4, true},
    {SND_PCM_FORMAT_DSD_U16_BE, "DSD_U16_BE", 2, false}, {SND_PCM_FORMAT_DSD_U16_LE, "DSD_U16_LE", 2, true},
    {SND_PCM_FORMAT_DSD_U8, "DSD_U8", 1, false},
};
constexpr uint32_t kMultiples[] = {64, 128, 256, 512, 1024};

// ALSA frame rate of DSD at 44.1 kHz x `multiple` in a format of `sampleBytes` DSD bytes per frame and channel.
unsigned FrameRate(uint32_t multiple, uint32_t sampleBytes) { return 44100u * multiple / (8u * sampleBytes); }

struct Found {
    const Candidate* format = nullptr;
    uint32_t mask = 0, maxChannels = 0;
    std::string card;
};

// Opens `name` without blocking (a busy device fails at once) and finds its first DSD format and rates.
bool ProbeDevice(const std::string& name, Found& found, std::string& error) {
    snd_pcm_t* pcm = nullptr;
    if (const int e = snd_pcm_open(&pcm, name.c_str(), SND_PCM_STREAM_PLAYBACK, SND_PCM_NONBLOCK); e < 0) {
        error = name + ": " + snd_strerror(e);
        return false;
    }
    snd_pcm_info_t* info = nullptr;
    snd_pcm_info_alloca(&info);
    found.card = name;
    if (snd_pcm_info(pcm, info) == 0) {
        char* cardName = nullptr;
        if (snd_card_get_name(snd_pcm_info_get_card(info), &cardName) == 0 && cardName) {
            found.card = cardName;
            std::free(cardName);
        }
    }
    snd_pcm_hw_params_t* any = nullptr;
    snd_pcm_hw_params_t* hw = nullptr;
    snd_pcm_hw_params_alloca(&any);
    snd_pcm_hw_params_alloca(&hw);
    snd_pcm_hw_params_any(pcm, any);
    snd_pcm_hw_params_set_access(pcm, any, SND_PCM_ACCESS_RW_INTERLEAVED);
    for (const Candidate& c : kFormats) {
        if (snd_pcm_hw_params_test_format(pcm, any, c.alsa) != 0) continue;
        snd_pcm_hw_params_copy(hw, any);
        if (snd_pcm_hw_params_set_format(pcm, hw, c.alsa) != 0) continue;
        uint32_t mask = 0;
        for (size_t i = 0; i < std::size(kMultiples); ++i)
            if (snd_pcm_hw_params_test_rate(pcm, hw, FrameRate(kMultiples[i], c.sampleBytes), 0) == 0) mask |= 1u << i;
        if (!mask) continue;
        unsigned maxChannels = 0;
        snd_pcm_hw_params_get_channels_max(hw, &maxChannels);
        found.format = &c;
        found.mask = mask;
        found.maxChannels = maxChannels;
        break;
    }
    snd_pcm_close(pcm);
    if (!found.format) error = name + " (" + found.card + ") has no native DSD format";
    return found.format != nullptr;
}

}  // namespace

bool AlsaSink::Probe(net::Caps& caps, std::string& error) {
    Found found;
    std::string why;
    if (!wanted_.empty()) {
        if (!ProbeDevice(wanted_, found, error)) return false;
        device_ = wanted_;
    } else {
        int card = -1;
        while (snd_card_next(&card) == 0 && card >= 0) {
            const std::string name = "hw:" + std::to_string(card) + ",0";
            Found f;
            if (ProbeDevice(name, f, why)) {
                found = f;
                device_ = name;
                break;
            }
        }
        if (!found.format) {
            error = "no sound card accepts native DSD" + (why.empty() ? std::string() : " (last: " + why + ")");
            return false;
        }
    }
    alsaFormat_ = found.format->alsa;
    format_ = {found.format->name, found.format->sampleBytes, found.format->littleEndian};
    caps = {};
    caps.dsdMask = found.mask;
    caps.maxChannels = found.maxChannels;
    std::snprintf(caps.device, sizeof(caps.device), "%s", found.card.c_str());
    std::snprintf(caps.format, sizeof(caps.format), "%s", found.format->name);
    return true;
}

bool AlsaSink::Open(uint32_t dsdMultiple, uint32_t channels, double periodMs, double bufferMs, std::string& error) {
    Close();
    if (device_.empty() || alsaFormat_ == SND_PCM_FORMAT_UNKNOWN) {
        error = "the device was not probed";
        return false;
    }
    auto fail = [&](const char* what, int e) {
        error = device_ + ": " + what + ": " + snd_strerror(e);
        Close();
        return false;
    };
    int e = snd_pcm_open(&pcm_, device_.c_str(), SND_PCM_STREAM_PLAYBACK, 0);
    if (e < 0) {
        pcm_ = nullptr;
        return fail("open", e);
    }
    const unsigned rate = FrameRate(dsdMultiple, format_.sampleBytes);
    snd_pcm_hw_params_t* hw = nullptr;
    snd_pcm_hw_params_alloca(&hw);
    snd_pcm_hw_params_any(pcm_, hw);
    if ((e = snd_pcm_hw_params_set_access(pcm_, hw, SND_PCM_ACCESS_RW_INTERLEAVED)) < 0) return fail("interleaved access", e);
    if ((e = snd_pcm_hw_params_set_format(pcm_, hw, alsaFormat_)) < 0) return fail(format_.name.c_str(), e);
    if ((e = snd_pcm_hw_params_set_channels(pcm_, hw, channels)) < 0) return fail("channels", e);
    snd_pcm_hw_params_set_rate_resample(pcm_, hw, 0);
    if ((e = snd_pcm_hw_params_set_rate(pcm_, hw, rate, 0)) < 0) return fail("rate", e);
    snd_pcm_uframes_t period = static_cast<snd_pcm_uframes_t>(std::lround(rate * periodMs / 1000.0));
    snd_pcm_uframes_t buffer = static_cast<snd_pcm_uframes_t>(std::lround(rate * bufferMs / 1000.0));
    int dir = 0;
    if ((e = snd_pcm_hw_params_set_period_size_near(pcm_, hw, &period, &dir)) < 0) return fail("period size", e);
    buffer = std::max(buffer, 2 * period);
    if ((e = snd_pcm_hw_params_set_buffer_size_near(pcm_, hw, &buffer)) < 0) return fail("buffer size", e);
    if ((e = snd_pcm_hw_params(pcm_, hw)) < 0) return fail("hardware parameters", e);
    snd_pcm_hw_params_get_period_size(hw, &period, &dir);
    snd_pcm_hw_params_get_buffer_size(hw, &buffer);
    period_ = static_cast<uint32_t>(period);
    buffer_ = static_cast<uint32_t>(buffer);
    channels_ = channels;

    snd_pcm_sw_params_t* sw = nullptr;
    snd_pcm_sw_params_alloca(&sw);
    snd_pcm_sw_params_current(pcm_, sw);
    // The stream starts once the whole periods of silence written first fill the buffer.
    snd_pcm_sw_params_set_start_threshold(pcm_, sw, buffer / period * period);
    snd_pcm_sw_params_set_avail_min(pcm_, sw, period);
    if ((e = snd_pcm_sw_params(pcm_, sw)) < 0) return fail("software parameters", e);
    xruns_ = 0;
    return true;
}

void AlsaSink::Close() {
    if (!pcm_) return;
    snd_pcm_drop(pcm_);
    snd_pcm_close(pcm_);
    pcm_ = nullptr;
}

bool AlsaSink::Recover(int err) {
    if (err == -EPIPE) ++xruns_;
    return snd_pcm_recover(pcm_, err, 1) == 0;
}

long AlsaSink::WaitSpace(int timeoutMs) {
    if (!pcm_) return -1;
    if (snd_pcm_state(pcm_) == SND_PCM_STATE_RUNNING) {
        const int w = snd_pcm_wait(pcm_, timeoutMs);
        if (w < 0 && !Recover(w)) return -1;
    }
    const snd_pcm_sframes_t avail = snd_pcm_avail_update(pcm_);
    if (avail < 0) return Recover(static_cast<int>(avail)) ? 0 : -1;
    return static_cast<long>(avail);
}

long AlsaSink::Delay() {
    snd_pcm_sframes_t d = 0;
    if (!pcm_ || snd_pcm_delay(pcm_, &d) < 0) return 0;
    return static_cast<long>(d);
}

bool AlsaSink::Write(const uint8_t* data, uint32_t frames) {
    if (!pcm_) return false;
    const size_t frameBytes = size_t(format_.sampleBytes) * channels_;
    while (frames) {
        const snd_pcm_sframes_t n = snd_pcm_writei(pcm_, data, frames);
        if (n == -EAGAIN) {
            snd_pcm_wait(pcm_, 10);
            continue;
        }
        if (n < 0) {
            if (!Recover(static_cast<int>(n))) return false;
            continue;  // prepared again: the stream restarts once the buffer is full
        }
        data += size_t(n) * frameBytes;
        frames -= static_cast<uint32_t>(n);
    }
    return true;
}

std::vector<std::string> AlsaSink::Describe() {
    std::vector<std::string> lines;
    int card = -1;
    while (snd_card_next(&card) == 0 && card >= 0) {
        const std::string name = "hw:" + std::to_string(card) + ",0";
        Found f;
        std::string error;
        if (!ProbeDevice(name, f, error)) {
            lines.push_back(error);
            continue;
        }
        std::string rates;
        for (size_t i = 0; i < std::size(kMultiples); ++i)
            if (f.mask & (1u << i)) rates += " DSD" + std::to_string(kMultiples[i]);
        lines.push_back(name + " (" + f.card + "): " + f.format->name + ", " + std::to_string(f.maxChannels) + " channels," + rates);
    }
    return lines;
}

}  // namespace dgmod::receiver
