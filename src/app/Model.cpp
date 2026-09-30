#include "app/Model.h"

#include "common/Registry.h"

#include <shellapi.h>
#include <shobjidl.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cwctype>
#include <format>
#include <fstream>
#include <thread>

namespace dgmod::app {

namespace {

constexpr double kSettingsDebounceSec = 0.35;
constexpr double kEndpointRefreshSec = 5.0;
constexpr uint32_t kFallbackSourceRate = 48000;

// Equalizer APO in any effect slot of the endpoint (its FxProperties hold the CLSID of its APO).
bool HasEqualizerApo(const std::wstring& deviceId) {
    constexpr std::wstring_view kEqualizerApoClsid = L"{EC1CC9CE-FAED-4822-828A-82A81A6F018F}";
    const size_t dot = deviceId.find_last_of(L'.');
    if (dot == std::wstring::npos) return false;
    auto key = reg::Open(HKEY_LOCAL_MACHINE,
                         L"SOFTWARE\\Microsoft\\Windows\\CurrentVersion\\MMDevices\\Audio\\Render\\" +
                             deviceId.substr(dot + 1) + L"\\FxProperties",
                         KEY_QUERY_VALUE);
    if (!key) return false;
    for (const auto& name : reg::EnumValueNames(key->Get())) {
        std::wstring v = reg::ReadString(key->Get(), name.c_str()).value_or(L"");
        for (auto& c : v) c = static_cast<wchar_t>(std::towupper(c));
        if (v.find(kEqualizerApoClsid) != std::wstring::npos) return true;
    }
    return false;
}

FilterPreview BuildPreview(uint32_t in, uint32_t out, dsp::FilterSpec spec) {
    const auto t0 = std::chrono::steady_clock::now();
    FilterPreview p;
    p.inRate = in;
    p.outRate = out;
    p.spec = spec;
    const auto kernel = dsp::Oversampler::FilterKernel(in, out, spec);
    if (!kernel) return p;
    p.info = kernel->Info();
    p.plan = dsp::Oversampler::Plan(in, out, spec);
    p.plan.filter = p.info;  // as built (an equiripple design may be longer than planned)
    // Show everything up to the lower rate: pass band, transition and the first image/alias band.
    p.maxFreqHz = std::min(in, out);
    constexpr int kPoints = 480;
    std::vector<double> freqs(kPoints);
    for (int i = 0; i < kPoints; ++i) freqs[size_t(i)] = p.maxFreqHz * i / (kPoints - 1);
    const auto db = kernel->MagnitudeResponseDb(freqs);
    p.responseDb.reserve(db.size());
    for (const double v : db) p.responseDb.push_back(static_cast<float>(v));
    // Impulse response from 24 input samples before the peak to 40 after it: shows pre-ringing (linear phase), its
    // absence (minimum phase) and how long the ringing lasts (shorter when apodizing).
    constexpr int kImpulsePoints = 640;
    constexpr double kBefore = 24.0, kAfter = 40.0;
    const double peak = kernel->PeakTime();
    const double scale = kernel->Prototype(peak);
    p.impulse.reserve(kImpulsePoints);
    for (int i = 0; i < kImpulsePoints; ++i) {
        const double t = -kBefore + (kBefore + kAfter) * i / (kImpulsePoints - 1);
        p.impulse.push_back(static_cast<float>(scale != 0.0 ? kernel->Prototype(peak + t) / scale : 0.0));
    }
    p.impulseStartMs = -kBefore * 1000.0 / in;
    p.impulseEndMs = kAfter * 1000.0 / in;
    p.buildMs = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count();
    return p;
}

}  // namespace

AppModel::~AppModel() {
    if (previewFuture_.valid()) previewFuture_.wait();
}

void AppModel::Init(bool demoMode) {
    demo = demoMode;
    if (demo) {
        LoadDemo();
        return;
    }
    bridge = LoadBridgeConfig();
    plugins = LoadPluginChain();
    RefreshEndpoints();
    bridgeAutostart = BridgeAutostartEnabled();
    SuggestBridgeDevices();
    PollBridge();
}

void AppModel::SuggestBridgeDevices() {
    // First run: the DAC is the current default device, the source is an enabled virtual cable.
    auto isVirtual = [](const std::wstring& name) {
        for (const wchar_t* key : {L"Voicemeeter", L"VB-Audio", L"CABLE", L"Virtual"})
            if (name.find(key) != std::wstring::npos) return true;
        return false;
    };
    if (bridge.outputId.empty()) {
        const std::wstring def = DefaultRenderEndpointId();
        if (const RenderEndpoint* d = FindRenderDevice(def); d && !isVirtual(d->name)) bridge.outputId = def;
    }
    if (bridge.sourceId.empty())
        for (const auto& d : renderDevices)
            if (isVirtual(d.name) && d.id != bridge.outputId) {
                bridge.sourceId = d.id;
                break;
            }
}

const RenderEndpoint* AppModel::FindRenderDevice(const std::wstring& id) const {
    for (const auto& d : renderDevices)
        if (d.id == id) return &d;
    return nullptr;
}

std::wstring AppModel::BridgeExePath() const {
    wchar_t path[MAX_PATH]{};
    ::GetModuleFileNameW(nullptr, path, MAX_PATH);
    std::wstring p = path;
    return p.substr(0, p.find_last_of(L'\\') + 1) + kBridgeExeName;
}

void AppModel::BridgeEdited() {
    bridge.Validate();
    bridgeEditedAt_ = now;
}

void AppModel::StartBridge() {
    if (demo) {
        ShowToast(L"Bridge started (demo)");
        return;
    }
    bridge.Validate();
    SaveBridgeConfig(bridge);
    bridgeEditedAt_ = -1;
    const std::wstring exe = BridgeExePath();
    const auto launch = [](const std::wstring& path) -> DWORD {
        std::wstring cmd = L"\"" + path + L"\"";
        STARTUPINFOW si{sizeof(si)};
        PROCESS_INFORMATION pi{};
        if (!::CreateProcessW(path.c_str(), cmd.data(), nullptr, nullptr, FALSE, DETACHED_PROCESS, nullptr, nullptr, &si,
                              &pi))
            return ::GetLastError();
        ::CloseHandle(pi.hThread);
        ::CloseHandle(pi.hProcess);
        return ERROR_SUCCESS;
    };
    // A bridge this build cannot talk to (another version) still holds the single-instance mutex, so a new process
    // would exit at once: stop it and launch this build as soon as it is gone.
    if (UniqueHandle(::OpenMutexW(SYNCHRONIZE, FALSE, kBridgeInstanceMutexName))) {
        StopBridge();
        std::thread([exe, launch] {
            for (int i = 0; i < 100; ++i) {
                if (!UniqueHandle(::OpenMutexW(SYNCHRONIZE, FALSE, kBridgeInstanceMutexName))) {
                    launch(exe);
                    return;
                }
                ::Sleep(100);
            }
        }).detach();
        ShowToast(L"Restarting the bridge");
        return;
    }
    if (const DWORD err = launch(exe); err == ERROR_SUCCESS)
        ShowToast(L"Bridge started");
    else
        ShowToast(L"Cannot start " + exe + L": " + HResultText(HRESULT_FROM_WIN32(err)), 8.0);
}

void AppModel::StopBridge() {
    if (demo) {
        ShowToast(L"Bridge stopped (demo)");
        return;
    }
    UniqueHandle e(::OpenEventW(EVENT_MODIFY_STATE, FALSE, kBridgeStopEventName));
    if (e) ::SetEvent(e.Get());
    ShowToast(L"Stopping the bridge");
}

void AppModel::SetAutostart(bool on) {
    if (demo) {
        bridgeAutostart = on;
        return;
    }
    if (HRESULT hr = SetBridgeAutostart(on, BridgeExePath()); FAILED(hr)) ShowToast(L"Autostart: " + HResultText(hr), 8.0);
    bridgeAutostart = BridgeAutostartEnabled();
}

void AppModel::PollBridge() {
    if (demo) return;
    if (!bridgeMapping_ && now - lastBridgeTry_ > 1.0) {
        lastBridgeTry_ = now;
        if (auto m = BridgeStatusMapping::OpenForReader()) bridgeMapping_ = std::move(*m);
    }
    bridgeMapped = static_cast<bool>(bridgeMapping_);
    BridgeStatusData d{};
    if (bridgeMapping_ && bridgeMapping_.Read(d)) bridgeStatus = d;
    const uint64_t t = FileTimeNow();
    bridgeRunning = bridgeMapped && bridgeStatus.heartbeat && t >= bridgeStatus.heartbeat &&
                    t - bridgeStatus.heartbeat < 15'000'000 && bridgeStatus.state != BridgeState::Stopped;
    const bool fresh = bridgeMapped && bridgeStatus.heartbeat && t >= bridgeStatus.heartbeat &&
                       t - bridgeStatus.heartbeat < 15'000'000;
    bridgeForeign = !fresh && UniqueHandle(::OpenMutexW(SYNCHRONIZE, FALSE, kBridgeInstanceMutexName));
}

void AppModel::PollBridgeMeter(double keepSec) {
    static const double qpcToSec = [] {
        LARGE_INTEGER f;
        ::QueryPerformanceFrequency(&f);
        return 1.0 / double(f.QuadPart);
    }();
    LARGE_INTEGER q;
    ::QueryPerformanceCounter(&q);
    bridgeMeterNow = double(q.QuadPart) * qpcToSec;
    if (demo) {
        // Something that looks like music: a 120 BPM pulse with decaying hits over a slowly breathing floor.
        const double step = 0.005;
        double t = bridgeMeter.empty() ? bridgeMeterNow - keepSec : bridgeMeter.back().t + step;
        for (t = std::max(t, bridgeMeterNow - keepSec); t <= bridgeMeterNow; t += step) {
            const auto noise = [](double x) {
                const double s = std::sin(x * 12.9898) * 43758.5453;
                return float(s - std::floor(s));
            };
            const double beat = std::fmod(t, 0.5) / 0.5;
            const float base = 0.10f + 0.06f * float(std::sin(t * 0.7));
            const float hit = 0.55f * float(std::exp(-7.0 * beat)) * (std::fmod(t, 2.0) < 0.5 ? 1.25f : 1.f);
            LevelSample s;
            s.t = t;
            for (int c = 0; c < 2; ++c) {
                const float v = (base + hit) * (0.75f + 0.25f * noise(t * 97.0 + c * 13.0)) * (c ? 0.86f : 1.f);
                s.in[c] = std::min(v, 1.f);
                s.out[c] = std::min(v * 1.06f, 0.98f);
            }
            bridgeMeter.push_back(s);
        }
    } else if (bridgeMapping_) {
        meterScratch_.clear();
        bridgeMapping_.ReadMeter(meterCursor_, meterScratch_);
        for (const auto& e : meterScratch_) {
            LevelSample s;
            s.t = double(e.qpc) * qpcToSec;
            std::copy_n(e.in, 2, s.in);
            std::copy_n(e.out, 2, s.out);
            bridgeMeter.push_back(s);
        }
    }
    while (!bridgeMeter.empty() && bridgeMeter.front().t < bridgeMeterNow - keepSec) bridgeMeter.pop_front();
}

void AppModel::RefreshEndpoints() {
    if (demo) return;
    const std::wstring keep = Current() ? Current()->id : L"";
    renderDevices = ActiveRenderEndpoints();
    defaultDeviceId = DefaultRenderEndpointId();
    asioDrivers = ListAsioDrivers();
    outputHasEqualizerApo = !bridge.outputId.empty() && HasEqualizerApo(bridge.outputId);
    lastEndpointRefresh_ = now;
    current = -1;
    for (const std::wstring& id : {keep, defaultDeviceId})
        for (size_t i = 0; current < 0 && i < renderDevices.size(); ++i)
            if (!id.empty() && renderDevices[i].id == id) current = static_cast<int>(i);
    if (current < 0 && !renderDevices.empty()) current = 0;
    if (!Current() || Current()->id != keep) {
        formatOptions.clear();
        formatOptionsFor.clear();
        formatChoice = -1;
    }
}

void AppModel::Select(int index) {
    if (index < 0 || index >= static_cast<int>(renderDevices.size()) || index == current) return;
    current = index;
    formatOptions.clear();
    formatOptionsFor.clear();
    formatChoice = -1;
}

const RenderEndpoint* AppModel::Current() const {
    return current >= 0 && current < static_cast<int>(renderDevices.size()) ? &renderDevices[size_t(current)] : nullptr;
}

uint32_t AppModel::PreviewSourceRate() const {
    if (bridgeRunning && bridgeStatus.inRate) return bridgeStatus.inRate;
    if (const RenderEndpoint* s = FindRenderDevice(bridge.sourceId); s && s->mixFormat.sampleRate) return s->mixFormat.sampleRate;
    return kFallbackSourceRate;
}

uint32_t AppModel::PreviewDeviceRate() const {
    if (bridgeRunning && bridgeStatus.outRate && !bridgeStatus.bypass && (bridgeStatus.dop != 0) == bridge.Dop() &&
        (bridgeStatus.nativeDsd != 0) == bridge.NativeDsd())
        return bridgeStatus.outRate;
    if (bridge.NativeDsd()) {
        if (bridge.dsdMultiple) return DopFrameRate(bridge.dsdMultiple);
        if (const AsioDriverEntry* d = NativeDsdDriver(); d && asioDsdMask && d->name == asioMaskDriver)
            for (const uint32_t m : kNativeDsdMultiples)
                if (m <= kNativeDsdAutoMax && (asioDsdMask & DsdMaskBit(m))) return DopFrameRate(m);
        return DopFrameRate(256);
    }
    if (bridge.Dop()) {
        if (bridge.dsdMultiple) return DopFrameRate(bridge.dsdMultiple);
        for (const uint32_t m : kDsdMultiples)
            for (const auto& f : bridgeFormats)
                if (f.sampleRate == DopFrameRate(m) && f.validBits >= 24) return f.sampleRate;
        return DopFrameRate(128);
    }
    if (bridge.outputRate) return bridge.outputRate;
    uint32_t best = 0;
    for (const auto& f : bridgeFormats) best = std::max(best, f.sampleRate);
    return best ? best : 384000;
}

const AsioDriverEntry* AppModel::NativeDsdDriver() const {
    const RenderEndpoint* out = FindRenderDevice(bridge.outputId);
    return FindAsioDriver(asioDrivers, bridge.asioDriver, out ? out->name : bridge.outputName);
}

void AppModel::UpdatePreview() {
    if (previewFuture_.valid()) {
        if (previewFuture_.wait_for(std::chrono::seconds(0)) != std::future_status::ready) return;
        preview = previewFuture_.get();
        previewPending = false;
    }
    const uint32_t in = PreviewSourceRate(), out = PreviewDeviceRate();
    const dsp::FilterSpec spec = bridge.Spec();
    if (in == previewIn_ && out == previewOut_ && spec == previewSpec_ && preview) return;
    if (in == previewIn_ && out == previewOut_ && spec == previewSpec_ && previewPending) return;
    previewIn_ = in;
    previewOut_ = out;
    previewSpec_ = spec;
    previewPending = true;
    auto wake = notify;
    previewFuture_ = std::async(std::launch::async, [in, out, spec, wake] {
        FilterPreview p = BuildPreview(in, out, spec);
        if (wake) wake();
        return p;
    });
}

void AppModel::ReadLogTail() {
    lastLogRead_ = now;
    std::ifstream f(BridgeLogPath(), std::ios::binary);
    if (!f) {
        logTail.clear();
        return;
    }
    f.seekg(0, std::ios::end);
    const std::streamoff size = f.tellg();
    constexpr std::streamoff kTail = 48 * 1024;
    f.seekg(std::max<std::streamoff>(0, size - kTail));
    std::string text((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());
    if (size > kTail) text.erase(0, text.find('\n') + 1);
    logTail = Widen(text);
}

bool AppModel::Tick() {
    bool changed = false;

    if (bridgeEditedAt_ >= 0 && now - bridgeEditedAt_ > kSettingsDebounceSec) {
        bridgeEditedAt_ = -1;
        if (!demo) {
            const HRESULT hr = SaveBridgeConfig(bridge);
            bridgeError = SUCCEEDED(hr) ? L"" : L"Bridge settings could not be saved: " + HResultText(hr);
        }
        changed = true;
    }
    {
        const bool wasRunning = bridgeRunning;
        const auto before = bridgeStatus.heartbeat;
        PollBridge();
        changed |= wasRunning != bridgeRunning || before != bridgeStatus.heartbeat;
        // Native DSD rates of the driver the bridge opened (only the bridge can open it while it runs).
        if (bridgeRunning && bridgeStatus.asioDriver[0] && bridgeStatus.asioDsdMask &&
            (bridgeStatus.asioDsdMask != asioDsdMask || asioMaskDriver != bridgeStatus.asioDriver)) {
            asioDsdMask = bridgeStatus.asioDsdMask;
            asioMaskDriver = bridgeStatus.asioDriver;
            changed = true;
        }
    }
    // The bridge re-binds a re-plugged device (new endpoint ID) by name: adopt its choice unless an edit is pending.
    if (!demo && bridgeEditedAt_ < 0 && now - lastBridgeIdCheck_ > 2.0) {
        lastBridgeIdCheck_ = now;
        const BridgeConfig stored = LoadBridgeConfig();
        if (!stored.outputId.empty() && (stored.sourceId != bridge.sourceId || stored.outputId != bridge.outputId)) {
            bridge.sourceId = stored.sourceId;
            bridge.outputId = stored.outputId;
            bridgeFormatsFor.clear();
            changed = true;
        }
    }
    if (page == Page::Bridge && !demo && !bridge.outputId.empty() && bridgeFormatsFor != bridge.outputId) {
        outputHasEqualizerApo = HasEqualizerApo(bridge.outputId);
        // While the bridge holds the DAC in exclusive mode only the bridge can probe it: take its list.
        if (bridgeRunning && bridgeStatus.formatCount && bridge.outputId == bridgeStatus.outputId) {
            bridgeFormatsFor = bridge.outputId;
            bridgeFormats.clear();
            for (uint32_t i = 0; i < std::min<uint32_t>(bridgeStatus.formatCount, kBridgeMaxFormats); ++i) {
                const BridgeFormat& f = bridgeStatus.formats[i];
                bridgeFormats.push_back({f.rate, f.bits, f.validBits, DeviceFormatLabel(f.rate, f.bits, f.validBits), {}});
            }
            changed = true;
        } else if (!bridgeRunning) {
            bridgeFormatsFor = bridge.outputId;
            bridgeFormats = SupportedDeviceFormats(bridge.outputId);
            changed = true;
        }
    }

    if (!demo && now - lastEndpointRefresh_ > kEndpointRefreshSec && page == Page::Devices) {
        RefreshEndpoints();
        changed = true;
    }
    if (page == Page::Plugins && !demo) {
        if (!pluginCatalogLoaded) {
            pluginCatalogLoaded = true;
            auto cached = LoadPluginCache();
            if (cached.empty()) ScanPlugins(false);
            else SetCatalog(std::move(cached));
            changed = true;
        }
        // The bridge quarantines a plug-in that crashed it: show the stored chain.
        if (now - lastPluginsCheck_ > 2.0) {
            lastPluginsCheck_ = now;
            if (PluginChainConfig stored = LoadPluginChain(); !(stored == plugins)) {
                plugins = std::move(stored);
                changed = true;
            }
        }
    }
    if (pluginScanning) {
        PollPluginScan();
        changed = true;
    }
    if (page == Page::Log && now - lastLogRead_ > 2.0 && !demo) {
        ReadLogTail();
        changed = true;
    }
    if (page == Page::Filter || page == Page::Bridge) {
        const bool had = preview.has_value();
        UpdatePreview();
        changed |= had != preview.has_value() || previewPending;
    }
    if (page == Page::Devices && !demo) {
        if (const RenderEndpoint* ep = Current(); ep && formatOptionsFor != ep->id) {
            formatOptionsFor = ep->id;
            formatOptions = SupportedDeviceFormats(ep->id);
            formatChoice = -1;
            for (size_t i = 0; i < formatOptions.size(); ++i)
                if (formatOptions[i].sampleRate == ep->deviceFormat.sampleRate &&
                    formatOptions[i].bits == ep->deviceFormat.bitsPerSample &&
                    formatOptions[i].validBits == ep->deviceFormat.validBits)
                    formatChoice = static_cast<int>(i);
            changed = true;
        }
    }
    return changed;
}

void AppModel::ApplyDeviceFormat() {
    const RenderEndpoint* ep = Current();
    if (!ep || formatChoice < 0 || formatChoice >= static_cast<int>(formatOptions.size())) return;
    if (demo) {
        ShowToast(L"Device format changed (demo)");
        return;
    }
    const auto& opt = formatOptions[size_t(formatChoice)];
    if (auto r = SetDeviceFormat(ep->id, opt.format); r) ShowToast(L"Device format set to " + opt.label);
    else ShowToast(r.error().Message(), 8.0);
    formatOptionsFor.clear();
    RefreshEndpoints();
}

// ---- Plug-ins ------------------------------------------------------------------------------------------------------

const BridgePluginStatus* AppModel::PluginStatus(uint32_t id) const {
    if (!bridgeRunning) return nullptr;
    for (uint32_t i = 0; i < std::min(bridgeStatus.pluginCount, kBridgeMaxPlugins); ++i)
        if (bridgeStatus.plugins[i].id == id) return &bridgeStatus.plugins[i];
    return nullptr;
}

void AppModel::PluginsEdited() {
    if (demo) return;
    const HRESULT hr = SavePluginChain(plugins);
    pluginsError = SUCCEEDED(hr) ? L"" : L"The plug-in chain could not be saved: " + HResultText(hr);
    lastPluginsCheck_ = now;
}

void AppModel::AddPlugin(const PluginClassInfo& c) {
    if (plugins.plugins.size() >= kMaxPlugins) {
        ShowToast(std::format(L"The chain holds at most {} plug-ins", kMaxPlugins));
        return;
    }
    PluginEntry e;
    e.id = plugins.NextId();
    e.format = c.format;
    e.path = c.path;
    e.classId = c.classId;
    e.name = c.name;
    e.vendor = c.vendor;
    plugins.plugins.push_back(std::move(e));
    PluginsEdited();
    ShowToast(bridgeRunning ? L"Added " + c.name : L"Added " + c.name + L": it loads when the bridge runs");
}

void AppModel::RemovePlugin(uint32_t id) {
    const auto it = std::find_if(plugins.plugins.begin(), plugins.plugins.end(), [&](const PluginEntry& e) { return e.id == id; });
    if (it == plugins.plugins.end()) return;
    const std::wstring name = it->name;
    plugins.plugins.erase(it);
    PluginsEdited();
    if (!demo) {
        // The bridge deletes the settings file when it unloads the plug-in; if it does not run, nobody else will.
        if (!bridgeRunning)
            if (const std::wstring path = PluginStatePath(id); !path.empty()) ::DeleteFileW(path.c_str());
    }
    ShowToast(L"Removed " + name);
}

void AppModel::MovePlugin(uint32_t id, int delta) {
    auto& v = plugins.plugins;
    const auto it = std::find_if(v.begin(), v.end(), [&](const PluginEntry& e) { return e.id == id; });
    if (it == v.end()) return;
    const ptrdiff_t from = it - v.begin(), to = from + delta;
    if (to < 0 || to >= static_cast<ptrdiff_t>(v.size())) return;
    std::swap(v[size_t(from)], v[size_t(to)]);
    PluginsEdited();
}

void AppModel::PostToPluginHost(UINT msg, uint32_t id) {
    if (demo) {
        ShowToast(L"Plug-in editor (demo)");
        return;
    }
    HWND host = ::FindWindowExW(HWND_MESSAGE, nullptr, kPluginHostWindowClass, nullptr);
    if (!host) {
        ShowToast(L"Start the bridge first: it hosts the plug-ins and their editors");
        return;
    }
    DWORD pid = 0;
    ::GetWindowThreadProcessId(host, &pid);
    ::AllowSetForegroundWindow(pid);  // the editor window may come to the front
    ::PostMessageW(host, msg, id, 0);
}

void AppModel::OpenPluginEditor(uint32_t id) { PostToPluginHost(kPluginHostOpenEditor, id); }

void AppModel::RetryPlugin(uint32_t id) {
    for (auto& e : plugins.plugins)
        if (e.id == id && e.quarantined) {
            e.quarantined = false;
            PluginsEdited();
            return;
        }
    PostToPluginHost(kPluginHostReload, id);
}

void AppModel::RevealPlugin(uint32_t id) const {
    const PluginEntry* e = plugins.Find(id);
    if (!e) return;
    const std::wstring args = L"/select,\"" + e->path + L"\"";
    ::ShellExecuteW(hwnd, nullptr, L"explorer.exe", args.c_str(), nullptr, SW_SHOWNORMAL);
}

void AppModel::SetCatalog(std::vector<PluginFileScan> scans) {
    scans_ = scans;
    for (const auto& x : extraScans_)
        if (std::none_of(scans.begin(), scans.end(), [&](const PluginFileScan& s) { return s.path == x.path; })) scans.push_back(x);
    pluginCatalog.clear();
    pluginProblems.clear();
    for (auto& s : scans) {
        if (s.notPlugin) continue;
        if (!s.error.empty() && s.classes.empty()) {
            pluginProblems.push_back(s);
            continue;
        }
        for (auto& c : s.classes)
            if (!c.instrument) pluginCatalog.push_back(c);
    }
    auto lower = [](std::wstring t) {
        for (auto& ch : t) ch = static_cast<wchar_t>(std::towlower(ch));
        return t;
    };
    std::sort(pluginCatalog.begin(), pluginCatalog.end(), [&](const PluginClassInfo& a, const PluginClassInfo& b) {
        const auto la = lower(a.name), lb = lower(b.name);
        return la != lb ? la < lb : a.format < b.format;
    });
    std::sort(pluginProblems.begin(), pluginProblems.end(), [](const auto& a, const auto& b) { return a.path < b.path; });
}

void AppModel::ScanPlugins(bool full) {
    if (demo || pluginScanning) return;
    pluginScanning = true;
    pluginScanDone = pluginScanTotal = 0;
    auto state = std::make_shared<ScanState>();
    scan_ = state;
    const std::wstring exe = BridgeExePath();
    std::thread([state, exe, full] {
        ::CoInitializeEx(nullptr, COINIT_MULTITHREADED);
        state->result = ScanAllPlugins(exe, !full, [&](size_t done, size_t total) {
            state->done = done;
            state->total = total;
        });
        state->finished.store(true, std::memory_order_release);
        ::CoUninitialize();
    }).detach();
}

void AppModel::PollPluginScan() {
    if (!scan_) {
        pluginScanning = false;
        return;
    }
    pluginScanDone = scan_->done.load();
    pluginScanTotal = scan_->total.load();
    if (!scan_->finished.load(std::memory_order_acquire)) return;
    SetCatalog(std::move(scan_->result));
    scan_.reset();
    pluginScanning = false;
    ShowToast(std::format(L"{} effects found", pluginCatalog.size()));
}

void AppModel::BrowsePlugin() {
    if (demo) return;
    ComPtr<IFileOpenDialog> dlg;
    if (FAILED(::CoCreateInstance(CLSID_FileOpenDialog, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&dlg)))) return;
    const COMDLG_FILTERSPEC types[] = {{L"Plug-ins (*.vst3, *.dll)", L"*.vst3;*.dll"}, {L"All files", L"*.*"}};
    dlg->SetFileTypes(2, types);
    dlg->SetTitle(L"Add a VST 3 or VST 2 plug-in");
    FILEOPENDIALOGOPTIONS opts = 0;
    dlg->GetOptions(&opts);
    dlg->SetOptions(opts | FOS_FORCEFILESYSTEM | FOS_FILEMUSTEXIST);
    if (dlg->Show(hwnd) != S_OK) return;
    ComPtr<IShellItem> item;
    wchar_t* path = nullptr;
    if (FAILED(dlg->GetResult(&item)) || FAILED(item->GetDisplayName(SIGDN_FILESYSPATH, &path))) return;
    std::wstring file = path;
    ::CoTaskMemFree(path);
    // A VST 3 bundle's inner binary was picked: use the bundle.
    if (const size_t at = file.find(L".vst3\\Contents\\"); at != std::wstring::npos) file = file.substr(0, at + 5);
    const PluginFileScan s = ScanPluginFile(BridgeExePath(), file, PluginFormatOf(file));
    std::vector<const PluginClassInfo*> effects;
    for (const auto& c : s.classes)
        if (!c.instrument) effects.push_back(&c);
    if (effects.empty()) {
        ShowToast(s.notPlugin ? L"This file is not a plug-in"
                  : !s.error.empty() ? s.error
                                      : std::wstring(L"This plug-in contains no effect (instruments need MIDI)"),
                  8.0);
        return;
    }
    if (effects.size() == 1) {
        AddPlugin(*effects[0]);
        return;
    }
    // Several effects (a shell): list them at the top of the catalogue.
    extraScans_.push_back(s);
    SetCatalog(std::vector<PluginFileScan>(scans_));
    ShowToast(std::format(L"{} effects in this file: choose one below", effects.size()), 6.0);
}

void AppModel::ShowToast(std::wstring text, double seconds) {
    toast = std::move(text);
    toastUntil = now + seconds;
}

void AppModel::OpenLogFolder() const {
    const std::wstring dir = DataDirectory();
    if (!dir.empty()) ::ShellExecuteW(hwnd, L"open", dir.c_str(), nullptr, nullptr, SW_SHOWNORMAL);
}

void AppModel::LoadDemo() {
    auto fmt = [](uint32_t rate, uint32_t bits, uint32_t valid, bool isFloat) {
        StreamFormat f;
        f.tag = isFloat ? 3 : 1;
        f.isFloat = isFloat;
        f.isPcm = !isFloat;
        f.channels = 2;
        f.sampleRate = rate;
        f.bitsPerSample = bits;
        f.validBits = valid;
        return f;
    };
    logTail = L"2026-09-26 12:00:01.120 session: Voicemeeter AUX Input (VB-Audio) 48000 Hz -> Speakers (2- FiiO K9) "
              L"384000 Hz, 32/24 bit, period 10.0 ms\r\n"
              L"2026-09-26 12:00:01.180 playing\r\n";
    formatOptions = {{44100, 24, 24, L"24 bit, 44100 Hz", {}}, {48000, 24, 24, L"24 bit, 48000 Hz", {}},
                     {96000, 24, 24, L"24 bit, 96000 Hz", {}}};
    formatChoice = 1;
    formatOptionsFor = L"cable";
    renderDevices = {{L"dac", L"Speakers (2- FiiO K9)", 1, fmt(384000, 32, 32, true), fmt(384000, 32, 24, false)},
                     {L"cable", L"Voicemeeter AUX Input (VB-Audio)", 1, fmt(48000, 32, 32, true), fmt(48000, 24, 24, false)},
                     {L"tv", L"LG TV (NVIDIA High Definition Audio)", 1, fmt(48000, 32, 32, true), fmt(48000, 16, 16, false)}};
    defaultDeviceId = L"cable";
    current = 1;
    asioDrivers = {{L"FiiO ASIO Driver", {}}, {L"Voicemeeter Virtual ASIO", {}}};
    asioDsdMask = DsdMaskBit(64) | DsdMaskBit(128) | DsdMaskBit(256) | DsdMaskBit(512);
    asioMaskDriver = L"FiiO ASIO Driver";
    bridge.sourceId = L"cable";
    bridge.outputId = L"dac";
    bridgeFormats = {{384000, 32, 24, L"24 bit (32-bit container), 384000 Hz", {}}, {192000, 32, 24, L"24 bit (32-bit container), 192000 Hz", {}}};
    bridgeFormatsFor = L"dac";
    bridgeAutostart = true;
    bridgeRunning = bridgeMapped = true;
    bridgeStatus.state = BridgeState::Playing;
    CopyText(bridgeStatus.sourceName, L"Voicemeeter AUX Input (VB-Audio)");
    CopyText(bridgeStatus.outputName, L"Speakers (2- FiiO K9)");
    bridgeStatus.inRate = 48000;
    bridgeStatus.outRate = 384000;
    bridgeStatus.channels = 2;
    bridgeStatus.outBits = 32;
    bridgeStatus.outValidBits = 24;
    bridgeStatus.periodFrames = 3840;
    bridgeStatus.periodMs = 10.0;
    bridgeStatus.taps = 480;
    bridgeStatus.tablePhases = 4096;
    bridgeStatus.filterLatencyMs = 5.0;
    bridgeStatus.fifoMs = 30.1;
    bridgeStatus.targetMs = 30.0;
    bridgeStatus.driftPpm = 37.4;
    bridgeStatus.totalLatencyMs = 55.1;
    bridgeStatus.framesOut = 384000ull * 3600;
    bridgeStatus.cpuAvgUs = 412.0;
    bridgeStatus.cpuMaxUs = 690.0;
    bridgeStatus.peakIn[0] = 0.62f;
    bridgeStatus.peakIn[1] = 0.58f;
    bridgeStatus.peakOut[0] = 0.63f;
    bridgeStatus.peakOut[1] = 0.59f;
    // Plug-ins: a running chain with a bypassed and a failed entry.
    auto demoPlugin = [&](uint32_t id, PluginFormat f, const wchar_t* name, const wchar_t* vendor, bool enabled) {
        PluginEntry e;
        e.id = id;
        e.format = f;
        e.path = std::format(L"C:\\Program Files\\Common Files\\VST3\\{}.vst3", name);
        e.name = name;
        e.vendor = vendor;
        e.enabled = enabled;
        plugins.plugins.push_back(e);
    };
    demoPlugin(1, PluginFormat::Vst3, L"Ghz CanOpener Studio 3", L"Goodhertz", true);
    demoPlugin(2, PluginFormat::Vst2, L"BC Liny EQ 5 (Stereo)", L"Blue Cat Audio", false);
    demoPlugin(3, PluginFormat::Vst3, L"Realphones", L"dSONIQ", true);
    auto demoStatus = [&](uint32_t i, uint32_t id, PluginRunState st, uint32_t flags, uint32_t latency, float cpu, const wchar_t* msg) {
        BridgePluginStatus& s = bridgeStatus.plugins[i];
        s.id = id;
        s.state = st;
        s.flags = flags;
        s.latencyFrames = latency;
        s.inputs = s.outputs = 2;
        s.cpuUs = cpu;
        s.cpuMaxUs = cpu * 1.6f;
        CopyText(s.message, msg);
    };
    demoStatus(0, 1, PluginRunState::Ready, kPluginFlagEditor | kPluginFlagEnabled | kPluginFlagProcessing | kPluginFlagEditorOpen, 0, 6.2f, L"");
    demoStatus(1, 2, PluginRunState::Ready, kPluginFlagEditor | kPluginFlagProcessing, 128, 93.0f, L"");
    demoStatus(2, 3, PluginRunState::Failed, 0, 0, 0.f, L"The effect could not be activated at 48000 Hz");
    bridgeStatus.pluginCount = 3;
    bridgeStatus.pluginsOn = 1;
    bridgeStatus.pluginLatencyMs = 0;
    bridgeStatus.pluginCpuUs = 99.2f;
    pluginCatalogLoaded = true;
    for (const auto& [name, vendor, f, cat] : {std::tuple{L"BinauralDecoder", L"IEM", PluginFormat::Vst3, L"Fx|Spatial"},
                                               std::tuple{L"bs2R", L"bs2b.sourceforge.net", PluginFormat::Vst2, L"Effect"},
                                               std::tuple{L"Ghz CanOpener Studio 3", L"Goodhertz", PluginFormat::Vst3, L"Fx|Spatial"},
                                               std::tuple{L"Realphones", L"dSONIQ", PluginFormat::Vst3, L"Fx"}}) {
        PluginClassInfo c;
        c.format = f;
        c.name = name;
        c.vendor = vendor;
        c.category = cat;
        for (const auto& e : plugins.plugins)
            if (e.name == c.name) c.path = e.path;
        pluginCatalog.push_back(c);
    }
    PluginFileScan bad;
    bad.path = L"C:\\Program Files\\Steinberg\\VSTPlugins\\BS2BR VST Plugin\\BS2BR.dll";
    bad.error = L"32-bit plug-in: dgmod loads 64-bit plug-ins only";
    pluginProblems.push_back(bad);
    RebuildDemoPreview();
}

void AppModel::RebuildDemoPreview() {
    preview = BuildPreview(48000, 384000, bridge.Spec());
    previewIn_ = 48000;
    previewOut_ = 384000;
    previewSpec_ = bridge.Spec();
}

}  // namespace dgmod::app
