#pragma once

#include "common/AsioDrivers.h"
#include "common/BridgeConfig.h"
#include "common/BridgeStatus.h"
#include "common/PluginConfig.h"
#include "common/PolicyConfig.h"
#include "dsp/Kernel.h"
#include "dsp/Oversampler.h"
#include "ui/Ui.h"

#include <atomic>
#include <deque>
#include <functional>
#include <future>
#include <memory>
#include <optional>
#include <string>
#include <vector>

namespace dgmod::app {

enum class Page : int { Bridge, Filter, Tone, Plugins, Devices, Log, Count };

// Frequency response and layout of the filter shown on the Filter page.
struct FilterPreview {
    uint32_t inRate = 0, outRate = 0;
    dsp::FilterSpec spec{};
    dsp::KernelInfo info{};     // the filter plotted (stage 1 of the cascade, or the single stage)
    dsp::OversamplerPlan plan{};  // both stages as built
    double maxFreqHz = 0;
    std::vector<float> responseDb;  // evenly spaced over [0, maxFreqHz]
    // Impulse response around its main peak, normalized to the peak, evenly spaced from impulseStartMs to
    // impulseEndMs (time relative to the peak).
    std::vector<float> impulse;
    double impulseStartMs = 0, impulseEndMs = 0;
    double buildMs = 0;
};

class AppModel {
public:
    AppModel() = default;
    ~AppModel();
    AppModel(const AppModel&) = delete;
    AppModel& operator=(const AppModel&) = delete;

    Page page = Page::Bridge;
    double now = 0;
    bool demo = false;

    // the dgmod bridge (virtual cable -> oversampler -> DAC in exclusive mode).
    BridgeConfig bridge;                           // edited in place by the UI, saved after a short debounce
    std::wstring bridgeError;
    BridgeStatusData bridgeStatus{};
    bool bridgeMapped = false;
    bool bridgeRunning = false;
    bool bridgeForeign = false;  // a bridge process runs whose status this build cannot read (another version)
    bool bridgeAutostart = false;
    std::vector<RenderEndpoint> renderDevices;     // active playback devices (pickers, Devices page)
    std::wstring defaultDeviceId;                  // default playback device
    bool outputHasEqualizerApo = false;            // Equalizer APO is installed on the output device
    std::vector<DeviceFormatOption> bridgeFormats; // exclusive formats of the bridge output device
    std::wstring bridgeFormatsFor;
    std::vector<AsioDriverEntry> asioDrivers;      // installed ASIO drivers (native DSD)
    // Native DSD rates of an ASIO driver, as the bridge found them when it last opened it (DsdMaskBit; 0 = unknown).
    uint32_t asioDsdMask = 0;
    std::wstring asioMaskDriver;
    // ASIO driver native DSD would use now: the configured one, or the one matching the output device.
    [[nodiscard]] const AsioDriverEntry* NativeDsdDriver() const;
    // Level history of the bridge for the live graphs (oldest first), refreshed every frame by PollBridgeMeter.
    struct LevelSample {
        double t = 0;             // seconds on the QPC clock
        float in[2]{}, out[2]{};  // linear L/R peaks: source (filter input) and output
    };
    std::deque<LevelSample> bridgeMeter;
    double bridgeMeterNow = 0;  // QPC clock at the latest poll, seconds
    // Reads the level entries published since the previous call and keeps the last `keepSec` seconds.
    void PollBridgeMeter(double keepSec);

    // Plug-in chain of the bridge (Plugins page): edited in place, saved at once (the bridge applies it live).
    PluginChainConfig plugins;
    std::wstring pluginsError;
    // Catalogue of installed plug-ins (effect classes, sorted by name) and the files that could not be used.
    std::vector<PluginClassInfo> pluginCatalog;
    std::vector<PluginFileScan> pluginProblems;
    bool pluginCatalogLoaded = false;
    bool pluginScanning = false;
    size_t pluginScanDone = 0, pluginScanTotal = 0;
    int pluginFilter = 0;          // 0 all, 1 VST 3, 2 VST 2
    bool pluginShowProblems = false;
    [[nodiscard]] const BridgePluginStatus* PluginStatus(uint32_t id) const;
    void PluginsEdited();
    void AddPlugin(const PluginClassInfo& c);
    void RemovePlugin(uint32_t id);
    void MovePlugin(uint32_t id, int delta);
    void OpenPluginEditor(uint32_t id);
    void RetryPlugin(uint32_t id);  // quarantined: load again; failed or crashed: reload in the bridge
    void ScanPlugins(bool full);    // full = ignore the cache
    void BrowsePlugin();            // adds the effects of a plug-in file chosen in a dialog
    void RevealPlugin(uint32_t id) const;

    // Tail of the bridge log (Log page).
    std::wstring logTail;

    // Devices page: selected device (index into renderDevices) and the shared-mode formats it accepts.
    int current = -1;
    std::vector<DeviceFormatOption> formatOptions;
    std::wstring formatOptionsFor;
    int formatChoice = -1;

    std::wstring toast;
    double toastUntil = 0;

    // Filter preview (built in the background).
    std::optional<FilterPreview> preview;
    bool previewPending = false;

    // Per-page UI state.
    ui::ScrollState scroll[static_cast<int>(Page::Count)];
    ui::TableState devicesTable;

    std::function<void()> notify;  // wakes the UI thread (posted message)
    HWND hwnd = nullptr;

    void Init(bool demoMode);

    // Demo: rebuilds the filter preview after the demo filter settings were changed.

    void RebuildDemoPreview();
    // Polls the bridge, saves settings, completes background work. Returns true if the UI should redraw.
    bool Tick();
    void RefreshEndpoints();
    void Select(int index);
    [[nodiscard]] const RenderEndpoint* Current() const;
    void ApplyDeviceFormat();
    void ShowToast(std::wstring text, double seconds = 4.0);
    [[nodiscard]] uint32_t PreviewSourceRate() const;
    [[nodiscard]] uint32_t PreviewDeviceRate() const;
    void OpenLogFolder() const;

    void BridgeEdited();
    void StartBridge();
    void StopBridge();
    void SetAutostart(bool on);
    [[nodiscard]] std::wstring BridgeExePath() const;
    [[nodiscard]] const RenderEndpoint* FindRenderDevice(const std::wstring& id) const;

private:
    void PollBridge();
    void SuggestBridgeDevices();
    void ReadLogTail();
    void UpdatePreview();
    void LoadDemo();
    void PollPluginScan();
    void PostToPluginHost(UINT msg, uint32_t id);
    void SetCatalog(std::vector<PluginFileScan> scans);

    double lastEndpointRefresh_ = 0;
    double lastLogRead_ = -10;
    BridgeStatusMapping bridgeMapping_;
    double lastBridgeTry_ = -10;
    double bridgeEditedAt_ = -1;
    double lastBridgeIdCheck_ = -10;
    uint64_t meterCursor_ = 0;
    std::vector<BridgeMeterEntry> meterScratch_;

    struct ScanState {
        std::atomic<size_t> done{0}, total{0};
        std::atomic<bool> finished{false};
        std::vector<PluginFileScan> result;  // valid once finished
    };
    std::shared_ptr<ScanState> scan_;  // the scanning thread is detached: it only touches this state
    double lastPluginsCheck_ = -10;
    std::vector<PluginFileScan> scans_;       // the catalogue as scanned
    std::vector<PluginFileScan> extraScans_;  // files added through the Browse dialog

    std::future<FilterPreview> previewFuture_;
    uint32_t previewIn_ = 0, previewOut_ = 0;
    dsp::FilterSpec previewSpec_{};
};

}  // namespace dgmod::app
