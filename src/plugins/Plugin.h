#pragma once

// A hosted effect plug-in (VST 3 or VST 2), as the bridge's plug-in host and its real-time chain see it.
//
// Threads: everything except Process() runs on the host thread (the plug-in's UI thread: loading, activation, state,
// editor). Process() runs on the render thread, only while the instance is active, and never concurrently with
// Activate/Deactivate/LoadState (the chain lock keeps them apart). A structured exception inside the plug-in (access
// violation, ...) is caught: the instance is marked crashed and never called again (its code and memory are left
// alone, a crashed module is not unloaded).

#include "common/PluginConfig.h"
#include "common/Win.h"

#include <atomic>
#include <cstdint>
#include <memory>
#include <span>
#include <string>
#include <vector>

namespace dgmod::plugins {

// Runs fn(ctx) inside a structured exception handler. Returns 0, or the code of the exception it caught.
uint32_t GuardedCall(void (*fn)(void*), void* ctx);

template <class F>
uint32_t Guarded(F&& f) {
    using Fn = std::remove_reference_t<F>;
    return GuardedCall([](void* c) { (*static_cast<Fn*>(c))(); }, &f);
}

std::wstring ExceptionName(uint32_t code);

class PluginInstance;

// Notifications from an instance to its host.
struct PluginCallbacks {
    virtual ~PluginCallbacks() = default;
    // The editor asks for a new size (client pixels). Host thread.
    virtual void EditorResizeRequest(PluginInstance& p, int width, int height) = 0;
    // A parameter or the state changed (autosave). Any thread.
    virtual void StateChanged(PluginInstance& p) = 0;
    // Latency or I/O configuration changed: the instance wants to be re-activated. Any thread.
    virtual void RestartRequested(PluginInstance& p) = 0;
};

struct ProcessFormat {
    double rate = 0;
    uint32_t maxFrames = 0;
    uint32_t channels = 0;
    friend bool operator==(const ProcessFormat&, const ProcessFormat&) = default;
};

class PluginInstance {
public:
    virtual ~PluginInstance() = default;
    PluginInstance(const PluginInstance&) = delete;
    PluginInstance& operator=(const PluginInstance&) = delete;

    [[nodiscard]] const std::wstring& Name() const { return name_; }
    [[nodiscard]] const std::wstring& Vendor() const { return vendor_; }
    [[nodiscard]] PluginFormat Format() const { return format_; }

    // ---- Host thread
    // Activates the instance for a stream format (re-activates it if it was active). Real-time processing may start.
    virtual Result<void> Activate(const ProcessFormat& f) = 0;
    virtual void Deactivate() = 0;
    [[nodiscard]] bool Active() const { return active_; }
    [[nodiscard]] const ProcessFormat& ActiveFormat() const { return processFormat_; }
    [[nodiscard]] uint32_t LatencyFrames() const { return latency_; }
    // Channels of the plug-in's main input and output (while active).
    [[nodiscard]] uint32_t Inputs() const { return inputs_; }
    [[nodiscard]] uint32_t Outputs() const { return outputs_; }

    virtual std::vector<uint8_t> SaveState() = 0;
    virtual Result<void> LoadState(std::span<const uint8_t> data) = 0;

    [[nodiscard]] virtual bool HasEditor() const = 0;
    // Opens the editor inside `parent` (a top-level window) and returns its size in client pixels. `scale` is the
    // content scale for a DPI-aware window (1 = 96 dpi).
    virtual Result<SIZE> OpenEditor(HWND parent, float scale) = 0;
    virtual void CloseEditor() = 0;
    [[nodiscard]] virtual bool EditorOpen() const = 0;
    [[nodiscard]] virtual bool EditorResizable() const { return false; }
    // The editor follows the window's DPI (VST 3 content scaling); false = it draws at 96 dpi.
    [[nodiscard]] virtual bool EditorScales() const { return false; }
    // Nearest size the editor accepts (resizable editors).
    virtual SIZE ConstrainEditorSize(SIZE s) { return s; }
    // The window was resized by the user / its DPI changed.
    virtual void SetEditorSize(SIZE) {}
    virtual void SetEditorScale(float) {}
    // Periodic work (~30 Hz): editor idle, parameter echo from the processor to the controller.
    virtual void Idle() = 0;

    // ---- Render thread (active only). `in` and `out` hold `channels` planar buffers of at least `frames` frames.
    virtual void Process(const float* const* in, float* const* out, uint32_t channels, uint32_t frames) = 0;

    // ---- Any thread
    [[nodiscard]] bool Crashed() const { return crashCode_.load(std::memory_order_acquire) != 0; }
    [[nodiscard]] uint32_t CrashCode() const { return crashCode_.load(std::memory_order_acquire); }
    [[nodiscard]] const wchar_t* CrashWhere() const { return crashWhere_.load(std::memory_order_acquire); }
    // A crash the host caught in the plug-in's code (its editor window procedure).
    void ReportCrash(uint32_t code, const wchar_t* where) { Crash(code, where); }
    void* hostData = nullptr;  // the host's bookkeeping for this instance

    // Processing time (render thread adds, host reads and resets).
    std::atomic<uint64_t> cpuTicks{0}, cpuCalls{0}, cpuMaxTicks{0};

protected:
    PluginInstance() = default;
    // Records a crash (the first one wins). Returns false if `code` is 0 (no crash).
    bool Crash(uint32_t code, const wchar_t* where) {
        if (!code) return false;
        uint32_t expected = 0;
        if (crashCode_.compare_exchange_strong(expected, code)) crashWhere_.store(where, std::memory_order_release);
        active_ = false;
        return true;
    }

    std::wstring name_, vendor_;
    PluginFormat format_ = PluginFormat::Vst3;
    bool active_ = false;
    ProcessFormat processFormat_{};
    uint32_t latency_ = 0, inputs_ = 0, outputs_ = 0;
    PluginCallbacks* callbacks_ = nullptr;

private:
    std::atomic<uint32_t> crashCode_{0};
    std::atomic<const wchar_t*> crashWhere_{nullptr};
};

// Loads the plug-in class of a chain entry. `callbacks` must outlive the instance.
Result<std::unique_ptr<PluginInstance>> LoadPlugin(const PluginEntry& entry, PluginCallbacks* callbacks);
Result<std::unique_ptr<PluginInstance>> LoadVst3(const std::wstring& path, const std::wstring& classId, PluginCallbacks* cb);
Result<std::unique_ptr<PluginInstance>> LoadVst2(const std::wstring& path, const std::wstring& shellId, PluginCallbacks* cb);

// Lists the effect classes of a plug-in file in this process (the scanner process runs this).
PluginFileScan ScanPluginInProcess(const std::wstring& path);
void ScanVst3(const std::wstring& path, PluginFileScan& out);
void ScanVst2(const std::wstring& path, PluginFileScan& out);

// Maps the stream's channels to a plug-in bus of `busChannels` channels: a mono bus gets the mid signal, missing
// channels silence. And back: a mono output goes to every channel; channels beyond the bus stay dry.
void MapInput(const float* const* in, uint32_t channels, float* const* bus, uint32_t busChannels, uint32_t frames);
void MapOutput(const float* const* busOut, uint32_t busChannels, const float* const* in, float* const* out, uint32_t channels,
               uint32_t frames);

// Module handle of a plug-in binary with the plug-in's folder first on the DLL search path (its own dependencies).
HMODULE LoadPluginModule(const std::wstring& binary, std::wstring& error);
// The DLLs a binary imports that cannot be found (comma-separated), for a clear "not installed" message.
std::wstring MissingDependencies(const std::wstring& binary);

}  // namespace dgmod::plugins
