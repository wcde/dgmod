#include "plugins/PluginHost.h"

#include "common/BridgeConfig.h"
#include "common/Registry.h"

#include <dwmapi.h>
#include <ole2.h>

#include <algorithm>
#include <format>
#include <fstream>
#include <iterator>

namespace dgmod::plugins {

namespace {

constexpr wchar_t kEditorClass[] = L"dgmodPluginEditor";
constexpr UINT_PTR kTimerId = 1;
// Posted with every task: a plug-in's modal loop (a menu, a file dialog) still dispatches it, so the engine never
// waits for the user to close a dialog.
constexpr UINT kRunTasks = WM_APP + 16;
constexpr UINT kTimerMs = 30;
constexpr uint32_t kStatusTicks = 8;     // ~250 ms
constexpr uint32_t kAutosaveTicks = 100; // ~3 s
constexpr DWORD kEditorStyle = WS_OVERLAPPED | WS_CAPTION | WS_SYSMENU | WS_MINIMIZEBOX | WS_CLIPCHILDREN;
constexpr DWORD kResizableStyle = WS_THICKFRAME | WS_MAXIMIZEBOX;

void Log(const std::wstring& text) { AppendBridgeLog(L"plugins: " + text); }

std::wstring MarkerPath() {
    const std::wstring dir = PluginDataDirectory();
    return dir.empty() ? std::wstring() : dir + L"\\loading";
}

std::vector<uint8_t> ReadFileBytes(const std::wstring& path) {
    std::ifstream f(path, std::ios::binary);
    if (!f) return {};
    return std::vector<uint8_t>(std::istreambuf_iterator<char>(f), {});
}

bool WriteFileAtomic(const std::wstring& path, const std::vector<uint8_t>& data) {
    const std::wstring tmp = path + L".tmp";
    {
        std::ofstream f(tmp, std::ios::binary | std::ios::trunc);
        if (!f) return false;
        f.write(reinterpret_cast<const char*>(data.data()), static_cast<std::streamsize>(data.size()));
        if (!f) return false;
    }
    return ::MoveFileExW(tmp.c_str(), path.c_str(), MOVEFILE_REPLACE_EXISTING) != 0;
}

double QpcToUs() {
    static const double v = [] {
        LARGE_INTEGER f;
        ::QueryPerformanceFrequency(&f);
        return 1e6 / double(f.QuadPart);
    }();
    return v;
}

}  // namespace

PluginHost::~PluginHost() { Stop(); }

void PluginHost::Start(uint32_t timeoutMs) {
    if (thread_.joinable()) return;
    quit_.Reset(::CreateEventW(nullptr, TRUE, FALSE, nullptr));
    tasksEvent_.Reset(::CreateEventW(nullptr, FALSE, FALSE, nullptr));
    UniqueHandle started(::CreateEventW(nullptr, TRUE, FALSE, nullptr));
    HANDLE s = started.Get();
    thread_ = std::thread([this, s] { ThreadMain(s); });
    ::WaitForSingleObject(s, timeoutMs);
}

void PluginHost::Stop() {
    if (!thread_.joinable()) return;
    ::SetEvent(quit_.Get());
    thread_.join();
}

void PluginHost::Post(std::function<void()> fn) {
    {
        std::lock_guard lock(tasksMutex_);
        tasks_.push_back(std::move(fn));
    }
    ::SetEvent(tasksEvent_.Get());
    if (window_) ::PostMessageW(window_, kRunTasks, 0, 0);
}

void PluginHost::RunTasks() {
    std::vector<std::function<void()>> run;
    {
        std::lock_guard lock(tasksMutex_);
        run.swap(tasks_);
    }
    for (auto& f : run) f();
}

void PluginHost::PrepareSession(double rate, uint32_t maxFrames, uint32_t channels, uint32_t timeoutMs) {
    chain_.ConfigureSession(channels, rate, maxFrames);
    if (!thread_.joinable()) return;
    auto done = std::make_shared<UniqueHandle>(::CreateEventW(nullptr, TRUE, FALSE, nullptr));
    const ProcessFormat f{rate, maxFrames, std::max<uint32_t>(channels, 1)};
    Post([this, f, done] {
        session_ = f;
        hasSession_ = true;
        for (auto& inst : instances_)
            if (inst->plugin && !inst->plugin->Crashed() && (!inst->plugin->Active() || inst->plugin->ActiveFormat() != f))
                Activate(*inst, true);
        RebuildChain(true);
        PublishStatus();
        ::SetEvent(done->Get());
    });
    if (::WaitForSingleObject(done->Get(), timeoutMs) != WAIT_OBJECT_0)
        Log(L"the plug-ins did not get ready in time; they join the session once they are");
}

// ---- Host thread -----------------------------------------------------------------------------------------------------

void PluginHost::ThreadMain(HANDLE started) {
    ::SetThreadDescription(::GetCurrentThread(), L"dgmod plugin host");
    const HRESULT ole = ::OleInitialize(nullptr);  // editors may use drag and drop, the clipboard, COM dialogs
    const HINSTANCE instance = ::GetModuleHandleW(nullptr);
    WNDCLASSW hc{};
    hc.lpfnWndProc = HostProc;
    hc.hInstance = instance;
    hc.lpszClassName = kPluginHostWindowClass;
    ::RegisterClassW(&hc);
    hc.lpszClassName = L"dgmodPluginHostTest";
    ::RegisterClassW(&hc);
    WNDCLASSW ec{};
    ec.lpfnWndProc = EditorProc;
    ec.hInstance = instance;
    ec.hIcon = ::LoadIconW(instance, MAKEINTRESOURCEW(1));
    ec.hCursor = ::LoadCursorW(nullptr, IDC_ARROW);
    ec.hbrBackground = static_cast<HBRUSH>(::GetStockObject(BLACK_BRUSH));
    ec.lpszClassName = kEditorClass;
    ::RegisterClassW(&ec);
    window_ = ::CreateWindowExW(0, publicWindow_ ? kPluginHostWindowClass : L"dgmodPluginHostTest", L"dgmod plugin host", 0, 0, 0,
                                0, 0, HWND_MESSAGE, nullptr, instance, this);
    ::SetTimer(window_, kTimerId, kTimerMs, nullptr);

    auto key = reg::Create(HKEY_CURRENT_USER, key_, KEY_NOTIFY | KEY_QUERY_VALUE);
    UniqueHandle changed(::CreateEventW(nullptr, FALSE, FALSE, nullptr));
    bool armed = false;
    ULONGLONG lastCheck = ::GetTickCount64();

    CheckLoadingMarker();
    Sync();
    ::SetEvent(started);

    for (;;) {
        if (key && !armed)
            armed = ::RegNotifyChangeKeyValue(key->Get(), FALSE, REG_NOTIFY_CHANGE_LAST_SET | REG_NOTIFY_THREAD_AGNOSTIC,
                                              changed.Get(), TRUE) == ERROR_SUCCESS;
        HANDLE handles[] = {quit_.Get(), tasksEvent_.Get(), changed.Get()};
        const DWORD w = ::MsgWaitForMultipleObjectsEx(3, handles, 2000, QS_ALLINPUT, MWMO_INPUTAVAILABLE);
        if (w == WAIT_OBJECT_0) break;
        if (w == WAIT_OBJECT_0 + 1) RunTasks();
        if (w == WAIT_OBJECT_0 + 2) {
            armed = false;
            Sync();
        }
        if (const ULONGLONG now = ::GetTickCount64(); now - lastCheck >= 2000) {
            lastCheck = now;
            // A key deleted and created again (by a tool, or the user in regedit) no longer notifies the handle held
            // here: open it afresh and compare what it holds now.
            key = reg::Create(HKEY_CURRENT_USER, key_, KEY_NOTIFY | KEY_QUERY_VALUE);
            armed = false;
            if (!(LoadPluginChain(key_.c_str()) == synced_)) Sync();
        }
        MSG msg;
        while (::PeekMessageW(&msg, nullptr, 0, 0, PM_REMOVE)) Dispatch(msg);
    }

    RunTasks();  // a PrepareSession waiting for us
    ::KillTimer(window_, kTimerId);
    for (auto& inst : instances_) {
        SaveState(*inst, true);
        CloseEditor(*inst);
    }
    {
        auto lock = chain_.Lock();
        chain_.Set({}, false, true);
    }
    for (auto& inst : instances_) Unload(*inst, false);
    instances_.clear();
    PublishStatus();
    ::DestroyWindow(window_);
    window_ = nullptr;
    if (SUCCEEDED(ole)) ::OleUninitialize();
}

void PluginHost::Dispatch(MSG& msg) {
    // A plug-in editor's window procedure runs in our thread: an exception in it disables that plug-in instead of
    // taking the bridge down.
    if (const uint32_t code = Guarded([&] {
            ::TranslateMessage(&msg);
            ::DispatchMessageW(&msg);
        })) {
        Instance* inst = msg.hwnd ? FromWindow(msg.hwnd) : nullptr;
        if (inst && inst->plugin) inst->plugin->ReportCrash(code, L"its editor");
        else Log(L"exception in a window procedure: " + ExceptionName(code));
    }
}

LRESULT CALLBACK PluginHost::HostProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
    if (msg == WM_NCCREATE) {
        ::SetWindowLongPtrW(hwnd, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(reinterpret_cast<CREATESTRUCTW*>(lp)->lpCreateParams));
        return TRUE;
    }
    auto* self = reinterpret_cast<PluginHost*>(::GetWindowLongPtrW(hwnd, GWLP_USERDATA));
    if (!self) return ::DefWindowProcW(hwnd, msg, wp, lp);
    switch (msg) {
        case WM_TIMER: self->OnTimer(); return 0;
        case kRunTasks: self->RunTasks(); return 0;
        case kPluginHostOpenEditor: self->OpenEditor(static_cast<uint32_t>(wp)); return 0;
        case kPluginHostCloseEditor:
            if (Instance* inst = self->Find(static_cast<uint32_t>(wp))) self->CloseEditor(*inst);
            return 0;
        case kPluginHostReload:
            if (Instance* inst = self->Find(static_cast<uint32_t>(wp)); inst && inst->state != PluginRunState::Quarantined) {
                Log(std::format(L"reloading \"{}\"", inst->entry.name));
                self->Unload(*inst, false);
                self->RebuildChain(true);
                self->Load(*inst);
                self->RebuildChain(true);
                self->PublishStatus();
            }
            return 0;
        default: return ::DefWindowProcW(hwnd, msg, wp, lp);
    }
}

PluginHost::Instance* PluginHost::Find(uint32_t id) {
    for (auto& i : instances_)
        if (i->entry.id == id) return i.get();
    return nullptr;
}

PluginHost::Instance* PluginHost::FromWindow(HWND hwnd) {
    const HWND root = ::GetAncestor(hwnd, GA_ROOT);
    for (auto& i : instances_)
        if (i->editor && (i->editor == hwnd || i->editor == root)) return i.get();
    return nullptr;
}

void PluginHost::CheckLoadingMarker() {
    const std::wstring marker = MarkerPath();
    if (marker.empty()) return;
    const auto bytes = ReadFileBytes(marker);
    if (bytes.empty() && ::GetFileAttributesW(marker.c_str()) == INVALID_FILE_ATTRIBUTES) return;
    const uint32_t id = static_cast<uint32_t>(std::strtoul(std::string(bytes.begin(), bytes.end()).c_str(), nullptr, 10));
    ::DeleteFileW(marker.c_str());
    PluginChainConfig cfg = LoadPluginChain(key_.c_str());
    for (auto& e : cfg.plugins)
        if (e.id == id && !e.quarantined) {
            e.quarantined = true;
            SavePluginChain(cfg, key_.c_str());
            Log(std::format(L"\"{}\" ({}) crashed the bridge while loading: quarantined", e.name, e.path));
        }
}

void PluginHost::Sync() {
    const PluginChainConfig cfg = LoadPluginChain(key_.c_str());
    synced_ = cfg;
    bool changed = cfg.enabled != on_;
    on_ = cfg.enabled;
    // Instances that are gone (or now point at another plug-in) leave the chain first, then are unloaded.
    std::vector<std::unique_ptr<Instance>> keep, drop;
    for (auto& inst : instances_) {
        const PluginEntry* e = cfg.Find(inst->entry.id);
        const bool same = e && e->path == inst->entry.path && e->classId == inst->entry.classId && e->format == inst->entry.format &&
                          !(e->quarantined && !inst->entry.quarantined);
        (same ? keep : drop).push_back(std::move(inst));
    }
    instances_ = std::move(keep);
    if (!drop.empty()) {
        RebuildChain(true);
        for (auto& inst : drop) {
            const bool removed = !cfg.Find(inst->entry.id);
            Log(std::format(L"removed \"{}\"", inst->entry.name));
            CloseEditor(*inst);
            Unload(*inst, removed);
        }
        changed = true;
    }
    // The configured order, loading what is new.
    std::vector<uint32_t> order;
    for (const auto& inst : instances_) order.push_back(inst->entry.id);
    std::vector<std::unique_ptr<Instance>> next;
    for (const PluginEntry& e : cfg.plugins) {
        auto it = std::find_if(instances_.begin(), instances_.end(), [&](const auto& i) { return i && i->entry.id == e.id; });
        if (it != instances_.end()) {
            Instance& inst = **it;
            changed |= inst.entry.enabled != e.enabled;
            const bool retry = inst.state == PluginRunState::Quarantined && !e.quarantined;
            inst.entry = e;
            next.push_back(std::move(*it));
            if (retry) Load(*next.back());
            continue;
        }
        auto inst = std::make_unique<Instance>();
        inst->entry = e;
        if (e.quarantined) {
            inst->state = PluginRunState::Quarantined;
            inst->message = L"Crashed the bridge while loading";
        } else {
            Load(*inst);
        }
        next.push_back(std::move(inst));
        changed = true;
    }
    changed |= next.size() != order.size();
    for (size_t i = 0; i < next.size() && !changed; ++i) changed = order[i] != next[i]->entry.id;
    instances_ = std::move(next);
    RebuildChain(changed);
    PublishStatus();
}

void PluginHost::Load(Instance& inst) {
    const std::wstring marker = MarkerPath();
    if (!marker.empty()) {
        const std::string id = std::to_string(inst.entry.id);
        std::ofstream(marker, std::ios::binary | std::ios::trunc).write(id.data(), static_cast<std::streamsize>(id.size()));
    }
    auto loaded = LoadPlugin(inst.entry, this);
    if (!marker.empty()) ::DeleteFileW(marker.c_str());
    inst.crashHandled = false;
    if (!loaded) {
        inst.state = PluginRunState::Failed;
        inst.message = loaded.error().Message();
        Log(std::format(L"\"{}\" ({}) failed to load: {}", inst.entry.name, inst.entry.path, inst.message));
        return;
    }
    inst.plugin = std::move(*loaded);
    inst.plugin->hostData = &inst;
    inst.message.clear();
    const std::wstring statePath = PluginStatePath(inst.entry.id);
    if (auto bytes = ReadFileBytes(statePath); !bytes.empty()) {
        if (auto r = inst.plugin->LoadState(bytes); !r) {
            inst.message = L"Its settings could not be restored: " + r.error().Message();
            Log(std::format(L"\"{}\": {}", inst.entry.name, inst.message));
        } else {
            inst.saved = std::move(bytes);
        }
    }
    inst.state = PluginRunState::Idle;
    Log(std::format(L"loaded \"{}\" by {} ({}, {})", inst.plugin->Name(), inst.plugin->Vendor().empty() ? L"?" : inst.plugin->Vendor(),
                    inst.entry.format == PluginFormat::Vst3 ? L"VST 3" : L"VST 2", inst.entry.path));
    if (inst.plugin->Crashed()) {
        CheckCrash(inst);
        return;
    }
    if (hasSession_) Activate(inst, false);
}

void PluginHost::Unload(Instance& inst, bool removed) {
    CloseEditor(inst);
    if (inst.plugin) {
        // Out of the render thread's chain first (the lock waits for a period being processed), then unloaded.
        std::unique_ptr<PluginInstance> p = std::move(inst.plugin);
        RebuildChain(true);
        if (p->Crashed()) (void)p.release();  // its code may be corrupt: never called again
        else p.reset();
    }
    inst.editor = nullptr;
    inst.state = PluginRunState::Loading;
    if (removed) {
        const std::wstring path = PluginStatePath(inst.entry.id);
        if (!path.empty()) ::DeleteFileW(path.c_str());
    }
}

bool PluginHost::Activate(Instance& inst, bool inChain) {
    if (!inst.plugin || inst.plugin->Crashed()) return false;
    Result<void> r;
    {
        std::unique_lock<std::mutex> lock;
        if (inChain) lock = chain_.Lock();
        r = inst.plugin->Activate(session_);
        if (inChain) chain_.MarkChanged();
    }
    if (!r) {
        inst.state = inst.plugin->Crashed() ? PluginRunState::Crashed : PluginRunState::Failed;
        inst.message = r.error().Message();
        Log(std::format(L"\"{}\" could not start at {} Hz: {}", inst.entry.name, session_.rate, inst.message));
        CheckCrash(inst);
        return false;
    }
    inst.state = PluginRunState::Ready;
    const uint32_t lat = inst.plugin->LatencyFrames();
    Log(std::format(L"\"{}\" active at {} Hz, {} frames per block, {} in / {} out{}", inst.entry.name, session_.rate,
                    session_.maxFrames, inst.plugin->Inputs(), inst.plugin->Outputs(),
                    lat ? std::format(L", latency {} frames ({:.1f} ms)", lat, lat * 1000.0 / session_.rate) : std::wstring()));
    return true;
}

void PluginHost::RebuildChain(bool changed) {
    std::vector<ChainSlot> slots;
    for (auto& inst : instances_)
        if (inst->plugin && !inst->plugin->Crashed()) slots.push_back({inst->plugin.get(), inst->entry.enabled});
    auto lock = chain_.Lock();
    chain_.Set(std::move(slots), on_, changed);
}

void PluginHost::CheckCrash(Instance& inst) {
    if (!inst.plugin || !inst.plugin->Crashed() || inst.crashHandled) return;
    inst.crashHandled = true;
    inst.state = PluginRunState::Crashed;
    const wchar_t* where = inst.plugin->CrashWhere();
    inst.message = std::format(L"Crashed ({}) while {}: disabled", ExceptionName(inst.plugin->CrashCode()), where ? where : L"running");
    Log(std::format(L"\"{}\": {}", inst.entry.name, inst.message));
    if (inst.editor) {
        // Its window procedure is the plug-in's code: hide the window rather than destroy it.
        ::ShowWindow(inst.editor, SW_HIDE);
        inst.editor = nullptr;
    }
    RebuildChain(true);
}

void PluginHost::OnTimer() {
    ++ticks_;
    for (auto& p : instances_) {
        Instance& inst = *p;
        if (!inst.plugin) continue;
        if (!inst.plugin->Crashed()) {
            inst.plugin->Idle();
            if (inst.restart.exchange(false) && hasSession_ && inst.plugin->Active()) {
                const uint32_t before = inst.plugin->LatencyFrames();
                if (Activate(inst, true) && inst.plugin->LatencyFrames() != before)
                    Log(std::format(L"\"{}\" latency changed: {} -> {} frames", inst.entry.name, before, inst.plugin->LatencyFrames()));
            }
        }
        CheckCrash(inst);
        if (ticks_ % kAutosaveTicks == 0) SaveState(inst, false);
    }
    if (ticks_ % kStatusTicks == 0) PublishStatus();
}

void PluginHost::SaveState(Instance& inst, bool force) {
    if (!inst.plugin || inst.plugin->Crashed()) return;
    const bool dirty = inst.dirty.exchange(false);
    if (!force && !dirty && !inst.editor) return;
    auto bytes = inst.plugin->SaveState();
    if (bytes.empty() || bytes == inst.saved) return;
    const std::wstring path = PluginStatePath(inst.entry.id);
    if (path.empty()) return;
    if (WriteFileAtomic(path, bytes)) inst.saved = std::move(bytes);
    else Log(std::format(L"cannot save the settings of \"{}\" to {}", inst.entry.name, path));
}

void PluginHost::PublishStatus() {
    std::vector<BridgePluginStatus> st;
    double latency = 0;
    float cpu = 0;
    const double us = QpcToUs();
    for (auto& p : instances_) {
        Instance& inst = *p;
        if (st.size() == kBridgeMaxPlugins) break;
        BridgePluginStatus s{};
        s.id = inst.entry.id;
        s.state = inst.state;
        CopyText(s.name, inst.plugin ? inst.plugin->Name() : inst.entry.name);
        CopyText(s.message, inst.message);
        if (inst.entry.enabled) s.flags |= kPluginFlagEnabled;
        if (inst.editor) s.flags |= kPluginFlagEditorOpen;
        if (PluginInstance* pl = inst.plugin.get(); pl && !pl->Crashed()) {
            if (pl->HasEditor()) s.flags |= kPluginFlagEditor;
            if (pl->Active()) {
                s.latencyFrames = pl->LatencyFrames();
                s.inputs = pl->Inputs();
                s.outputs = pl->Outputs();
                if (on_) {
                    s.flags |= kPluginFlagProcessing;
                    if (inst.entry.enabled) latency += s.latencyFrames;
                }
            }
            const uint64_t calls = pl->cpuCalls.exchange(0), ticks = pl->cpuTicks.exchange(0), peak = pl->cpuMaxTicks.exchange(0);
            if (calls) {
                inst.cpuUs = static_cast<float>(double(ticks) / double(calls) * us);
                inst.cpuMaxUs = static_cast<float>(double(peak) * us);
            } else if (!(s.flags & kPluginFlagProcessing)) {
                inst.cpuUs = inst.cpuMaxUs = 0;
            }
            s.cpuUs = inst.cpuUs;
            s.cpuMaxUs = inst.cpuMaxUs;
            if (s.flags & kPluginFlagProcessing) cpu += inst.cpuUs;
        }
        st.push_back(s);
    }
    if (const uint64_t bad = chain_.BadSamples(); bad != lastBad_) {
        Log(std::format(L"{} invalid samples from the plug-ins replaced", bad - lastBad_));
        lastBad_ = bad;
    }
    std::lock_guard lock(statusMutex_);
    status_ = std::move(st);
    statusOn_ = on_;
    statusLatencyFrames_ = latency;
    statusRate_ = hasSession_ ? session_.rate : 0;
    statusCpuUs_ = cpu;
}

void PluginHost::Snapshot(BridgeStatusData& d) const {
    std::lock_guard lock(statusMutex_);
    d.pluginsOn = statusOn_ ? 1u : 0u;
    d.pluginCount = static_cast<uint32_t>(std::min<size_t>(status_.size(), kBridgeMaxPlugins));
    for (uint32_t i = 0; i < d.pluginCount; ++i) d.plugins[i] = status_[i];
    d.pluginLatencyMs = statusRate_ > 0 ? statusLatencyFrames_ * 1000.0 / statusRate_ : 0.0;
    d.pluginCpuUs = statusCpuUs_;
    d.pluginBadSamples = chain_.BadSamples();
    d.pluginSkips = chain_.SkippedPeriods();
}

// ---- Callbacks from the plug-ins ---------------------------------------------------------------------------------------

void PluginHost::StateChanged(PluginInstance& p) {
    if (auto* inst = static_cast<Instance*>(p.hostData)) inst->dirty.store(true);
}

void PluginHost::RestartRequested(PluginInstance& p) {
    if (auto* inst = static_cast<Instance*>(p.hostData)) inst->restart.store(true);
}

void PluginHost::EditorResizeRequest(PluginInstance& p, int width, int height) {
    auto* inst = static_cast<Instance*>(p.hostData);
    if (!inst || !inst->editor || width <= 0 || height <= 0) return;
    inst->editorResizing = true;
    SizeEditor(*inst, width, height);
    inst->editorResizing = false;
}

// ---- Editor windows ----------------------------------------------------------------------------------------------------

void PluginHost::OpenEditor(uint32_t id) {
    Instance* inst = Find(id);
    if (!inst || !inst->plugin || inst->plugin->Crashed()) return;
    if (inst->editor) {
        if (::IsIconic(inst->editor)) ::ShowWindow(inst->editor, SW_RESTORE);
        ::SetForegroundWindow(inst->editor);
        return;
    }
    if (!inst->plugin->HasEditor()) {
        inst->message = L"This plug-in has no editor";
        PublishStatus();
        return;
    }
    // Editors draw at the monitor's DPI (VST 3 gets the content scale, VST 2 editors find it themselves), as in an
    // audio workstation. A VST 3 editor that cannot scale, or one the user chose so for, gets a window Windows scales.
    const bool scaled = !inst->entry.windowsScaling;
    if (!CreateEditor(*inst, scaled)) return;
    if (scaled && inst->entry.format == PluginFormat::Vst3 && !inst->plugin->EditorScales() &&
        ::GetDpiForWindow(inst->editor) != USER_DEFAULT_SCREEN_DPI) {
        CloseEditor(*inst);
        CreateEditor(*inst, false);
    }
    PublishStatus();
}

bool PluginHost::CreateEditor(Instance& inst, bool scaled) {
    // A Windows-scaled window may host child windows the plug-in creates with another DPI awareness.
    const DPI_HOSTING_BEHAVIOR previousHosting =
        ::SetThreadDpiHostingBehavior(scaled ? DPI_HOSTING_BEHAVIOR_DEFAULT : DPI_HOSTING_BEHAVIOR_MIXED);
    const DPI_AWARENESS_CONTEXT previous = ::SetThreadDpiAwarenessContext(
        scaled ? DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2 : DPI_AWARENESS_CONTEXT_UNAWARE_GDISCALED);
    const std::wstring title = std::format(L"{} — dgmod", inst.plugin->Name());
    HWND w = ::CreateWindowExW(0, kEditorClass, title.c_str(), kEditorStyle, CW_USEDEFAULT, CW_USEDEFAULT, 400, 300, nullptr,
                               nullptr, ::GetModuleHandleW(nullptr), this);
    bool ok = false;
    if (w) {
        const BOOL dark = TRUE;
        ::DwmSetWindowAttribute(w, 20 /* DWMWA_USE_IMMERSIVE_DARK_MODE */, &dark, sizeof(dark));
        inst.editor = w;
        inst.editorScaled = scaled;
        const float scale = scaled ? static_cast<float>(::GetDpiForWindow(w)) / USER_DEFAULT_SCREEN_DPI : 1.0f;
        auto size = inst.plugin->OpenEditor(w, scale);
        if (!size) {
            inst.message = size.error().Message();
            Log(std::format(L"\"{}\": {}", inst.entry.name, inst.message));
            inst.editor = nullptr;
            ::DestroyWindow(w);
            CheckCrash(inst);
        } else {
            if (inst.plugin->EditorResizable())
                ::SetWindowLongPtrW(w, GWL_STYLE, ::GetWindowLongPtrW(w, GWL_STYLE) | kResizableStyle);
            inst.editorResizing = true;
            SizeEditor(inst, size->cx, size->cy);
            inst.editorResizing = false;
            RECT r{};
            ::GetWindowRect(w, &r);
            int x = r.left, y = r.top;
            if (inst.editorPlaced) {
                x = inst.editorRect.left;
                y = inst.editorRect.top;
            } else {
                POINT cursor{};
                ::GetCursorPos(&cursor);
                MONITORINFO mi{sizeof(mi)};
                if (::GetMonitorInfoW(::MonitorFromPoint(cursor, MONITOR_DEFAULTTONEAREST), &mi)) {
                    x = (mi.rcWork.left + mi.rcWork.right - (r.right - r.left)) / 2;
                    y = (mi.rcWork.top + mi.rcWork.bottom - (r.bottom - r.top)) / 2;
                }
            }
            ::SetWindowPos(w, nullptr, x, y, 0, 0, SWP_NOSIZE | SWP_NOZORDER | SWP_NOACTIVATE);
            ::ShowWindow(w, SW_SHOWNORMAL);
            ::SetForegroundWindow(w);
            inst.message.clear();
            ok = true;
        }
    }
    ::SetThreadDpiAwarenessContext(previous);
    ::SetThreadDpiHostingBehavior(previousHosting);
    return ok;
}

void PluginHost::SizeEditor(Instance& inst, int width, int height) {
    HWND w = inst.editor;
    if (!w) return;
    const DWORD style = static_cast<DWORD>(::GetWindowLongPtrW(w, GWL_STYLE));
    RECT r{0, 0, width, height};
    ::AdjustWindowRectExForDpi(&r, style, FALSE, 0, ::GetDpiForWindow(w));
    ::SetWindowPos(w, nullptr, 0, 0, r.right - r.left, r.bottom - r.top, SWP_NOMOVE | SWP_NOZORDER | SWP_NOACTIVATE);
}

void PluginHost::CloseEditor(Instance& inst) {
    if (!inst.editor) return;
    HWND w = inst.editor;
    if (::GetWindowRect(w, &inst.editorRect) && !::IsIconic(w)) inst.editorPlaced = true;
    if (inst.plugin && !inst.plugin->Crashed()) {
        inst.plugin->CloseEditor();
        SaveState(inst, false);
    }
    inst.editor = nullptr;
    if (inst.plugin && inst.plugin->Crashed()) ::ShowWindow(w, SW_HIDE);
    else ::DestroyWindow(w);
    CheckCrash(inst);
    PublishStatus();
}

LRESULT CALLBACK PluginHost::EditorProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
    if (msg == WM_NCCREATE) {
        ::SetWindowLongPtrW(hwnd, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(reinterpret_cast<CREATESTRUCTW*>(lp)->lpCreateParams));
        return ::DefWindowProcW(hwnd, msg, wp, lp);
    }
    auto* self = reinterpret_cast<PluginHost*>(::GetWindowLongPtrW(hwnd, GWLP_USERDATA));
    Instance* inst = self ? self->FromWindow(hwnd) : nullptr;
    if (!inst || !inst->plugin || inst->plugin->Crashed()) return ::DefWindowProcW(hwnd, msg, wp, lp);
    switch (msg) {
        case WM_CLOSE: self->CloseEditor(*inst); return 0;
        case WM_SIZE:
            if (!inst->editorResizing && wp != SIZE_MINIMIZED && inst->plugin->EditorResizable())
                inst->plugin->SetEditorSize({LOWORD(lp), HIWORD(lp)});
            return 0;
        case WM_SIZING:
            if (inst->plugin->EditorResizable()) {
                auto* r = reinterpret_cast<RECT*>(lp);
                const DWORD style = static_cast<DWORD>(::GetWindowLongPtrW(hwnd, GWL_STYLE));
                RECT frame{0, 0, 0, 0};
                ::AdjustWindowRectExForDpi(&frame, style, FALSE, 0, ::GetDpiForWindow(hwnd));
                const SIZE want{(r->right - r->left) - (frame.right - frame.left), (r->bottom - r->top) - (frame.bottom - frame.top)};
                const SIZE got = inst->plugin->ConstrainEditorSize(want);
                const LONG w = got.cx + (frame.right - frame.left), h = got.cy + (frame.bottom - frame.top);
                if (wp == WMSZ_LEFT || wp == WMSZ_TOPLEFT || wp == WMSZ_BOTTOMLEFT) r->left = r->right - w;
                else r->right = r->left + w;
                if (wp == WMSZ_TOP || wp == WMSZ_TOPLEFT || wp == WMSZ_TOPRIGHT) r->top = r->bottom - h;
                else r->bottom = r->top + h;
                return TRUE;
            }
            break;
        case WM_DPICHANGED:
            if (inst->editorScaled) {
                const auto* r = reinterpret_cast<const RECT*>(lp);
                inst->editorResizing = true;
                ::SetWindowPos(hwnd, nullptr, r->left, r->top, r->right - r->left, r->bottom - r->top, SWP_NOZORDER | SWP_NOACTIVATE);
                inst->editorResizing = false;
                inst->plugin->SetEditorScale(static_cast<float>(HIWORD(wp)) / USER_DEFAULT_SCREEN_DPI);
                return 0;
            }
            break;
        default: break;
    }
    return ::DefWindowProcW(hwnd, msg, wp, lp);
}

}  // namespace dgmod::plugins
