#pragma once

// The bridge's plug-in host: a thread of its own (the plug-ins' UI thread) that loads the chain configured in
// HKCU\Software\dgmod\Plugins, follows its changes, activates the plug-ins for each session's format, opens their
// editor windows on request of the GUI (messages to its message-only window), saves their state and hands the render
// thread a PluginChain to run.
//
// Crash handling: exceptions inside plug-in code (processing, editor, host calls) are caught and the plug-in is
// disabled; a plug-in that takes the whole process down while loading is quarantined at the next start (a marker file
// names the plug-in being loaded).

#include "common/BridgeStatus.h"
#include "common/PluginConfig.h"
#include "plugins/PluginChain.h"

#include <functional>
#include <memory>
#include <mutex>
#include <thread>
#include <vector>

namespace dgmod::plugins {

class PluginHost final : public PluginCallbacks {
public:
    // `registryKey` (under HKCU) holds the chain; another key for tests. `window` = false: no message-only window
    // (a test host the GUI must not find).
    explicit PluginHost(std::wstring registryKey = kRegPlugins, bool window = true)
        : key_(std::move(registryKey)), publicWindow_(window) {}
    ~PluginHost() override;
    PluginHost(const PluginHost&) = delete;
    PluginHost& operator=(const PluginHost&) = delete;

    // Starts the host thread and waits until the configured chain is loaded (at most `timeoutMs`).
    void Start(uint32_t timeoutMs = 30000);
    // Saves every plug-in's state, closes the editors and unloads the plug-ins. The render thread must be stopped.
    void Stop();

    [[nodiscard]] PluginChain& Chain() { return chain_; }
    // Engine thread, before the render thread starts: activates the plug-ins for the session format and configures
    // the chain (waits at most `timeoutMs` for the host thread).
    void PrepareSession(double rate, uint32_t maxFrames, uint32_t channels, uint32_t timeoutMs = 20000);
    // Copies the chain's status into `d` (any thread).
    void Snapshot(BridgeStatusData& d) const;

    // PluginCallbacks
    void EditorResizeRequest(PluginInstance& p, int width, int height) override;
    void StateChanged(PluginInstance& p) override;
    void RestartRequested(PluginInstance& p) override;

private:
    struct Instance {
        PluginEntry entry;
        std::unique_ptr<PluginInstance> plugin;
        PluginRunState state = PluginRunState::Loading;
        std::wstring message;
        HWND editor = nullptr;
        bool editorScaled = false;   // per-monitor DPI aware editor window
        bool editorResizing = false; // the window is being resized for the plug-in
        bool crashHandled = false;
        RECT editorRect{};           // last window position
        bool editorPlaced = false;
        std::atomic<bool> dirty{false}, restart{false};
        std::vector<uint8_t> saved;  // state as last written to its file
        float cpuUs = 0, cpuMaxUs = 0;
    };

    void ThreadMain(HANDLE started);
    void RunTasks();
    void Post(std::function<void()> fn);
    static LRESULT CALLBACK HostProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp);
    static LRESULT CALLBACK EditorProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp);
    void Dispatch(MSG& msg);

    void Sync();
    void Load(Instance& inst);
    void Unload(Instance& inst, bool removed);
    bool Activate(Instance& inst, bool inChain);
    void RebuildChain(bool changed);
    void CheckLoadingMarker();
    void OnTimer();
    void CheckCrash(Instance& inst);
    void SaveState(Instance& inst, bool force);
    void PublishStatus();
    Instance* Find(uint32_t id);
    Instance* FromWindow(HWND hwnd);

    void OpenEditor(uint32_t id);
    bool CreateEditor(Instance& inst, bool scaled);
    void CloseEditor(Instance& inst);
    void SizeEditor(Instance& inst, int width, int height);

    std::wstring key_;
    bool publicWindow_ = true;
    PluginChain chain_;
    std::thread thread_;
    UniqueHandle quit_, tasksEvent_;
    std::mutex tasksMutex_;
    std::vector<std::function<void()>> tasks_;
    std::atomic<HWND> window_{nullptr};

    // Host thread.
    std::vector<std::unique_ptr<Instance>> instances_;  // chain order
    bool on_ = true;
    PluginChainConfig synced_;  // the configuration applied last
    bool hasSession_ = false;
    ProcessFormat session_{};
    uint32_t ticks_ = 0;
    uint64_t lastBad_ = 0;

    // Status (host thread writes, any thread reads).
    mutable std::mutex statusMutex_;
    std::vector<BridgePluginStatus> status_;
    bool statusOn_ = false;
    double statusLatencyFrames_ = 0, statusRate_ = 0;
    float statusCpuUs_ = 0;
};

}  // namespace dgmod::plugins
