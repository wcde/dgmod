#pragma once

// The real-time side of the plug-in chain: runs the active plug-ins in order on the render thread.
//
// The host thread changes the chain (slots, master switch, re-activation) only while holding Lock(); the render
// thread only try-locks it: if the host holds the lock, that period passes the plug-ins by instead of waiting. Every
// change of what the chain outputs (a plug-in added, removed, bypassed or re-activated, a period passed by, a plug-in
// that crashed) is turned into a short transition by the bridge's splicer (predicted continuation crossfaded into the
// new output) instead of a click.

#include "bridge/Declicker.h"
#include "plugins/Plugin.h"

#include <mutex>
#include <vector>

namespace dgmod::plugins {

struct ChainSlot {
    PluginInstance* plugin = nullptr;
    bool enabled = true;  // false = bypassed: still processed (kept warm), its output discarded
};

class PluginChain {
public:
    // Engine thread, before the render thread starts (allocates).
    void ConfigureSession(uint32_t channels, double rate, uint32_t maxFrames);
    // Render thread: `x` holds `frames` interleaved frames (at most the session's maxFrames), processed in place.
    void Process(float* x, uint32_t frames);

    // ---- Host thread
    [[nodiscard]] std::unique_lock<std::mutex> Lock() { return std::unique_lock(mutex_); }
    // With the lock held: the new slots and master switch; `changed` marks a transition.
    void Set(std::vector<ChainSlot> slots, bool on, bool changed);
    void MarkChanged() { ++generation_; }  // with the lock held
    [[nodiscard]] uint64_t BadSamples() const { return badSamples_.load(std::memory_order_relaxed); }
    // Frames passed by because the host held the lock.
    [[nodiscard]] uint64_t SkippedPeriods() const { return skipped_.load(std::memory_order_relaxed); }

private:
    [[nodiscard]] bool Usable(const PluginInstance* p, uint32_t frames) const;

    std::mutex mutex_;
    std::vector<ChainSlot> slots_;  // guarded by mutex_
    bool on_ = false;               // guarded by mutex_
    uint64_t generation_ = 1;       // guarded by mutex_

    // Render thread / session.
    uint32_t channels_ = 0, maxFrames_ = 0;
    double rate_ = 0;
    uint64_t seen_ = 0;
    bool active_ = false;   // plug-ins changed the output in the previous period
    bool passed_ = false;   // the previous period was passed by (lock held by the host)
    bridge::Splicer splice_;
    std::vector<float> cur_, next_;
    std::vector<float*> curPtr_, nextPtr_;
    std::vector<const float*> curConst_;
    std::atomic<uint64_t> badSamples_{0}, skipped_{0};
};

}  // namespace dgmod::plugins
