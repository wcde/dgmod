#pragma once

#include "bridge/FrameFifo.h"

#include <algorithm>
#include <cstdint>

namespace dgmod::bridge {

// Start/stop/re-sync decisions of the render side, taken once per render period before the resampler reads:
//  * starts playing once the FIFO reaches the target and drops whatever piled up beyond it (e.g. while a USB DAC
//    re-clocks after being opened at a new rate), so latency starts at the target;
//  * drops the excess when the level runs far above the target during playback (render stall), instead of
//    draining it slowly through the drift controller;
//  * falls back to buffering (silence) when the FIFO cannot supply a period.
class LevelGuard {
public:
    void Configure(uint32_t targetFrames, uint32_t resyncMarginFrames) {
        target_ = targetFrames;
        margin_ = resyncMarginFrames;
        playing_ = false;
    }

    struct Decision {
        bool play = false;          // read `need` frames and render them
        bool started = false;       // playback (re)started this period
        bool underrun = false;      // was playing but the FIFO ran dry
        bool resynced = false;      // excess dropped during playback
        uint32_t skipped = 0;       // frames dropped from the FIFO
    };

    Decision Before(FrameFifo& fifo, uint32_t need) {
        Decision d;
        uint32_t avail = fifo.Available();
        if (!playing_) {
            if (avail < target_) return d;
            d.skipped = fifo.Skip(avail - target_);
            playing_ = true;
            d.started = true;
            avail = target_;
        } else if (avail > target_ + margin_) {
            d.skipped = fifo.Skip(avail - target_);
            d.resynced = true;
            avail = target_;
        }
        if (avail < need) {
            playing_ = false;
            d.underrun = true;
            return d;
        }
        d.play = true;
        return d;
    }

    // Stops playback (the bridge is stopping): an underrun if it was playing, nothing otherwise.
    Decision Halt() {
        Decision d;
        d.underrun = playing_;
        playing_ = false;
        return d;
    }

    [[nodiscard]] bool Playing() const { return playing_; }

private:
    uint32_t target_ = 0;
    uint32_t margin_ = 0;
    bool playing_ = false;
};

}  // namespace dgmod::bridge
