#include "plugins/PluginChain.h"

#include <algorithm>

namespace dgmod::plugins {

namespace {

uint64_t Qpc() {
    LARGE_INTEGER v;
    ::QueryPerformanceCounter(&v);
    return static_cast<uint64_t>(v.QuadPart);
}

}  // namespace

void PluginChain::ConfigureSession(uint32_t channels, double rate, uint32_t maxFrames) {
    channels_ = std::max<uint32_t>(channels, 1);
    rate_ = rate;
    maxFrames_ = maxFrames;
    const auto fade = static_cast<uint32_t>(bridge::kDeclickMs * rate / 1000.0 + 0.5);
    splice_.Configure(channels_, std::max<uint32_t>(fade, 1), std::clamp<uint32_t>(static_cast<uint32_t>(rate / 50), 256, 4096));
    cur_.assign(size_t(channels_) * maxFrames, 0.0f);
    next_.assign(size_t(channels_) * maxFrames, 0.0f);
    curPtr_.resize(channels_);
    nextPtr_.resize(channels_);
    curConst_.resize(channels_);
    for (uint32_t c = 0; c < channels_; ++c) {
        curPtr_[c] = cur_.data() + size_t(c) * maxFrames;
        nextPtr_[c] = next_.data() + size_t(c) * maxFrames;
    }
    seen_ = 0;
    active_ = passed_ = false;
}

void PluginChain::Set(std::vector<ChainSlot> slots, bool on, bool changed) {
    slots_ = std::move(slots);
    on_ = on;
    if (changed) ++generation_;
}

bool PluginChain::Usable(const PluginInstance* p, uint32_t frames) const {
    if (!p || !p->Active() || p->Crashed()) return false;
    const ProcessFormat& f = p->ActiveFormat();
    return f.rate == rate_ && f.channels == channels_ && f.maxFrames >= frames;
}

void PluginChain::Process(float* x, uint32_t frames) {
    if (!channels_ || !frames || frames > maxFrames_) return;
    std::unique_lock lock(mutex_, std::try_to_lock);
    bool jump = false;
    if (!lock.owns_lock()) {
        // The host is changing the chain: pass this period by.
        skipped_.fetch_add(1, std::memory_order_relaxed);
        if (active_) jump = true;
        active_ = false;
        passed_ = true;
        if (jump) splice_.Splice();
        splice_.Process(x, frames);
        return;
    }
    bool any = false;
    if (on_)
        for (const ChainSlot& s : slots_) any |= Usable(s.plugin, frames);
    if (generation_ != seen_ || passed_) {
        jump = active_ || any;
        seen_ = generation_;
    }
    passed_ = false;
    bool active = false;
    if (any) {
        const uint32_t ch = channels_;
        for (uint32_t i = 0; i < frames; ++i)
            for (uint32_t c = 0; c < ch; ++c) curPtr_[c][i] = x[size_t(i) * ch + c];
        for (const ChainSlot& s : slots_) {
            PluginInstance* p = s.plugin;
            if (!Usable(p, frames)) continue;
            for (uint32_t c = 0; c < ch; ++c) curConst_[c] = curPtr_[c];
            const uint64_t t0 = Qpc();
            p->Process(curConst_.data(), nextPtr_.data(), ch, frames);
            const uint64_t dt = Qpc() - t0;
            p->cpuTicks.fetch_add(dt, std::memory_order_relaxed);
            p->cpuCalls.fetch_add(1, std::memory_order_relaxed);
            if (dt > p->cpuMaxTicks.load(std::memory_order_relaxed)) p->cpuMaxTicks.store(dt, std::memory_order_relaxed);
            if (p->Crashed()) {  // its output is garbage: the chain continues without it
                jump = true;
                continue;
            }
            uint32_t bad = 0;
            for (uint32_t c = 0; c < ch; ++c) bad += bridge::SanitizeSamples(nextPtr_[c], frames, bridge::kInputMax);
            if (bad) badSamples_.fetch_add(bad, std::memory_order_relaxed);
            if (s.enabled) {
                std::swap(curPtr_, nextPtr_);
                active = true;
            }
        }
        if (active)
            for (uint32_t i = 0; i < frames; ++i)
                for (uint32_t c = 0; c < ch; ++c) x[size_t(i) * ch + c] = curPtr_[c][i];
    }
    if (active != active_) jump = true;
    active_ = active;
    lock.unlock();
    if (jump) splice_.Splice();
    splice_.Process(x, frames);
}

}  // namespace dgmod::plugins
