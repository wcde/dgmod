#pragma once

// In-band monitor of the native DSD stream: decodes the exact bytes handed to the ASIO driver (AsioDsdOutput::ReadTap)
// to the audio band (CIC4 /128 in integers, then an 8 kHz low-pass) and reports pulses that rise out of silence: a
// click put into the stream itself, as opposed to one the DAC or its driver adds.

#include <array>
#include <cmath>
#include <cstdint>
#include <vector>

namespace dgmod::bridge {

class InbandMonitor {
public:
    struct Event {
        double peakDb = 0;     // decoded peak of the buffer, dBFS of DSD full scale
        double beforeDb = 0;   // highest decoded peak of the kQuietBuffers buffers before it
        double offsetMs = 0;   // where in the buffer
        uint32_t channel = 0;
    };
    static constexpr double kEventDb = -100.0;  // a pulse at least this loud...
    static constexpr double kQuietDb = -115.0;  // ...after this much silence
    static constexpr uint32_t kQuietBuffers = 40;

    void Reset(uint32_t channels, uint32_t bytesPerBuffer, uint32_t dsdRate, bool lsbFirst) {
        channels_ = channels;
        bytes_ = bytesPerBuffer;
        dsdRate_ = dsdRate;
        lsbFirst_ = lsbFirst;
        ch_.assign(channels, {});
        const double fs = dsdRate / 128.0, w = 2 * 3.14159265358979323846 * 8000.0 / fs, c = std::cos(w), s = std::sin(w);
        const double al = s / (2 * 0.7071067811865476), a0 = 1 + al;
        for (auto& st : ch_)
            for (auto& f : st.lp) f = {(1 - c) / 2 / a0, (1 - c) / a0, (1 - c) / 2 / a0, -2 * c / a0, (1 - al) / a0};
        warmup_ = 8;
    }
    [[nodiscard]] uint32_t Channels() const { return channels_; }
    // Buffers were lost before the next one: decoding across the gap would show a cut stream, so the decoder settles
    // again before anything is reported.
    void Gap() {
        warmup_ = 8;
        for (auto& st : ch_) st.filled = 0;
    }
    // Quietest and loudest decoded buffer peak since the last call (dBFS); false if none was decoded.
    bool TakeRange(double& quietest, double& loudest) {
        const bool any = loudest_ > -1000.0;
        quietest = quietest_;
        loudest = loudest_;
        quietest_ = 1000.0;
        loudest_ = -1000.0;
        return any;
    }

    // One buffer as handed to the driver (channels x bytesPerBuffer). Returns true and fills `e` for a pulse out of
    // silence.
    bool Process(const uint8_t* data, Event& e) {
        bool hit = false;
        for (uint32_t c = 0; c < channels_; ++c) {
            State& st = ch_[c];
            const uint8_t* p = data + size_t(c) * bytes_;
            double peak = 0, at = 0;
            uint32_t sample = 0;
            for (uint32_t i = 0; i < bytes_; ++i) {
                const uint8_t b = p[i];
                for (int k = 0; k < 8; ++k) {
                    const int bit = lsbFirst_ ? (b >> k) & 1 : (b >> (7 - k)) & 1;
                    uint64_t x = bit ? 1u : ~uint64_t{0};
                    for (auto& v : st.integ) x = v += x;
                    if (++st.pos == 128) {
                        st.pos = 0;
                        uint64_t y = st.integ[3];
                        for (auto& v : st.comb) {
                            const uint64_t t = y - v;
                            v = y;
                            y = t;
                        }
                        double d = double(static_cast<int64_t>(y)) / (128.0 * 128.0 * 128.0 * 128.0);
                        for (auto& f : st.lp) d = f.Run(d);
                        if (std::abs(d) > peak) {
                            peak = std::abs(d);
                            at = sample;
                        }
                        ++sample;
                    }
                }
            }
            const double db = 20.0 * std::log10(peak + 1e-30);
            double before = -400.0;
            for (double h : st.history) before = std::max(before, h);
            if (!warmup_ && st.filled >= kQuietBuffers && db >= kEventDb && before < kQuietDb && !hit) {
                e = {db, before, at * 128.0 * 1000.0 / dsdRate_, c};
                hit = true;
            }
            if (!warmup_) {
                loudest_ = std::max(loudest_, db);
                quietest_ = std::min(quietest_, db);
            }
            st.history[st.next] = db;
            st.next = (st.next + 1) % kQuietBuffers;
            st.filled = std::min(st.filled + 1, kQuietBuffers);
        }
        if (warmup_) --warmup_;
        return hit;
    }

private:
    struct Biquad {
        double b0 = 0, b1 = 0, b2 = 0, a1 = 0, a2 = 0, z1 = 0, z2 = 0;
        double Run(double x) {
            const double y = b0 * x + z1;
            z1 = b1 * x - a1 * y + z2;
            z2 = b2 * x - a2 * y;
            return y;
        }
    };
    struct State {
        uint64_t integ[4]{}, comb[4]{};
        uint32_t pos = 0;
        Biquad lp[4];
        std::array<double, kQuietBuffers> history{};
        uint32_t next = 0, filled = 0;
    };
    uint32_t channels_ = 0, bytes_ = 0, dsdRate_ = 1;
    bool lsbFirst_ = false;
    std::vector<State> ch_;
    uint32_t warmup_ = 0;
    double quietest_ = 1000.0, loudest_ = -1000.0;
};

}  // namespace dgmod::bridge
