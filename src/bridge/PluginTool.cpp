// Plug-in commands of dgmod-bridge.exe:
//
//   --scan-plugin <path>   lists the effect classes of a plug-in file (the GUI runs this per file: a plug-in that
//                          crashes while loading only takes this process down)
//   --plugin-test <path> [--class <id>] [--editor <seconds>] [--capture <file.png>] [--desktop <n>]
//                          loads one plug-in the way the bridge does, processes 2 s of test signal at 48 kHz, checks
//                          its state round trip and optionally opens its editor (on virtual desktop n, captured to PNG)
//   --plugin-host-test <path>...
//                          runs the bridge's plug-in host on a test chain (HKCU\Software\dgmod\PluginsTest): loads,
//                          activates and processes the plug-ins in real time while the chain is edited (bypass, reorder,
//                          removal), then checks the saved settings

#include "bridge/PluginTool.h"

#include "common/PluginConfig.h"
#include "common/Registry.h"
#include "plugins/Plugin.h"
#include "plugins/PluginHost.h"

#include <shobjidl.h>
#include <wincodec.h>
#include <xmmintrin.h>

#include <algorithm>
#include <cmath>
#include <format>
#include <atomic>
#include <string>
#include <thread>
#include <vector>

namespace dgmod::bridge {

namespace {

std::wstring ArgAfter(const std::vector<std::wstring>& args, std::wstring_view name) {
    for (size_t i = 0; i + 1 < args.size(); ++i)
        if (args[i] == name) return args[i + 1];
    return {};
}

bool HasFlag(const std::vector<std::wstring>& args, std::wstring_view name) {
    return std::find(args.begin(), args.end(), name) != args.end();
}

struct NullCallbacks final : plugins::PluginCallbacks {
    HWND editor = nullptr;
    int resizes = 0, changes = 0, restarts = 0;
    void EditorResizeRequest(plugins::PluginInstance&, int w, int h) override {
        ++resizes;
        if (!editor) return;
        RECT r{0, 0, w, h};
        ::AdjustWindowRectExForDpi(&r, static_cast<DWORD>(::GetWindowLongPtrW(editor, GWL_STYLE)), FALSE, 0, ::GetDpiForWindow(editor));
        ::SetWindowPos(editor, nullptr, 0, 0, r.right - r.left, r.bottom - r.top, SWP_NOMOVE | SWP_NOZORDER | SWP_NOACTIVATE);
    }
    void StateChanged(plugins::PluginInstance&) override { ++changes; }
    void RestartRequested(plugins::PluginInstance&) override { ++restarts; }
};

bool MoveToDesktop(HWND w, int desktop) {
    auto key = std::wstring(L"Software\\Microsoft\\Windows\\CurrentVersion\\Explorer\\VirtualDesktops");
    HKEY k = nullptr;
    if (::RegOpenKeyExW(HKEY_CURRENT_USER, key.c_str(), 0, KEY_QUERY_VALUE, &k) != ERROR_SUCCESS) return false;
    BYTE ids[16 * 32]{};
    DWORD size = sizeof(ids);
    const LSTATUS st = ::RegQueryValueExW(k, L"VirtualDesktopIDs", nullptr, nullptr, ids, &size);
    ::RegCloseKey(k);
    if (st != ERROR_SUCCESS || size < DWORD(desktop) * 16 || desktop < 1) return false;
    GUID id;
    std::memcpy(&id, ids + (desktop - 1) * 16, 16);
    constexpr CLSID kVirtualDesktopManager = {0xAA509086, 0x5CA9, 0x4C25, {0x8F, 0x95, 0x58, 0x9D, 0x3C, 0x07, 0xB4, 0x8A}};
    ComPtr<IVirtualDesktopManager> m;
    if (FAILED(::CoCreateInstance(kVirtualDesktopManager, nullptr, CLSCTX_ALL, IID_PPV_ARGS(&m)))) return false;
    return SUCCEEDED(m->MoveWindowToDesktop(w, id));
}

bool CapturePng(HWND w, const std::wstring& path) {
    RECT r{};
    ::GetWindowRect(w, &r);
    const int width = r.right - r.left, height = r.bottom - r.top;
    if (width <= 0 || height <= 0) return false;
    HDC screen = ::GetDC(nullptr);
    HDC dc = ::CreateCompatibleDC(screen);
    BITMAPINFO bi{};
    bi.bmiHeader = {sizeof(BITMAPINFOHEADER), width, -height, 1, 32, BI_RGB};
    void* bits = nullptr;
    HBITMAP bmp = ::CreateDIBSection(screen, &bi, DIB_RGB_COLORS, &bits, nullptr, 0);
    HGDIOBJ old = ::SelectObject(dc, bmp);
    const BOOL printed = ::PrintWindow(w, dc, PW_RENDERFULLCONTENT);
    ::SelectObject(dc, old);
    bool ok = false;
    ComPtr<IWICImagingFactory> f;
    if (printed && SUCCEEDED(::CoCreateInstance(CLSID_WICImagingFactory, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&f)))) {
        ComPtr<IWICStream> stream;
        ComPtr<IWICBitmapEncoder> enc;
        ComPtr<IWICBitmapFrameEncode> frame;
        if (SUCCEEDED(f->CreateStream(&stream)) && SUCCEEDED(stream->InitializeFromFilename(path.c_str(), GENERIC_WRITE)) &&
            SUCCEEDED(f->CreateEncoder(GUID_ContainerFormatPng, nullptr, &enc)) &&
            SUCCEEDED(enc->Initialize(stream.Get(), WICBitmapEncoderNoCache)) && SUCCEEDED(enc->CreateNewFrame(&frame, nullptr)) &&
            SUCCEEDED(frame->Initialize(nullptr)) && SUCCEEDED(frame->SetSize(width, height))) {
            // The encoder picks its own pixel format: hand it a bitmap source it converts from.
            ComPtr<IWICBitmap> source;
            WICPixelFormatGUID fmt = GUID_WICPixelFormat24bppBGR;
            frame->SetPixelFormat(&fmt);
            ok = SUCCEEDED(f->CreateBitmapFromMemory(width, height, GUID_WICPixelFormat32bppBGR, width * 4, width * height * 4,
                                                     static_cast<BYTE*>(bits), &source)) &&
                 SUCCEEDED(frame->WriteSource(source.Get(), nullptr)) && SUCCEEDED(frame->Commit()) && SUCCEEDED(enc->Commit());
        }
    }
    ::DeleteObject(bmp);
    ::DeleteDC(dc);
    ::ReleaseDC(nullptr, screen);
    return ok;
}

}  // namespace

int ScanPluginCommand(const std::wstring& path, const std::function<void(const std::wstring&)>& print) {
    ::SetErrorMode(SEM_FAILCRITICALERRORS | SEM_NOGPFAULTERRORBOX | SEM_NOOPENFILEERRORBOX);
    ::OleInitialize(nullptr);
    const PluginFileScan s = plugins::ScanPluginInProcess(path);
    print(FormatScanLines(s));
    // Leave without running the plug-in's unload code (a clean scan result is already out).
    ::TerminateProcess(::GetCurrentProcess(), s.classes.empty() ? 2 : 0);
    return 0;
}

int PluginTestCommand(const std::vector<std::wstring>& args, const std::function<void(const std::wstring&)>& print) {
    ::OleInitialize(nullptr);
    size_t at = 0;
    while (at < args.size() && args[at] != L"--plugin-test") ++at;
    if (at + 1 >= args.size()) {
        print(L"usage: --plugin-test <path> [--class <id>] [--editor <seconds>] [--capture <file.png>] [--desktop <n>]");
        return 1;
    }
    PluginEntry e;
    e.path = args[at + 1];
    e.format = PluginFormatOf(e.path);
    e.classId = ArgAfter(args, L"--class");
    NullCallbacks cb;
    auto loaded = plugins::LoadPlugin(e, &cb);
    if (!loaded) {
        print(L"load failed: " + loaded.error().Message());
        return 1;
    }
    plugins::PluginInstance& p = **loaded;
    print(std::format(L"{} by {} ({}), editor: {}", p.Name(), p.Vendor(), e.format == PluginFormat::Vst3 ? L"VST 3" : L"VST 2",
                      p.HasEditor() ? L"yes" : L"no"));
    constexpr double kRate = 48000;
    constexpr uint32_t kBlock = 480, kChannels = 2;
    if (auto r = p.Activate({kRate, kBlock, kChannels}); !r) {
        print(L"activate failed: " + r.error().Message());
        return 1;
    }
    print(std::format(L"active: {} in / {} out, latency {} frames", p.Inputs(), p.Outputs(), p.LatencyFrames()));
    std::vector<float> in(size_t(kChannels) * kBlock), out(in.size());
    const float* inPtr[kChannels] = {in.data(), in.data() + kBlock};
    float* outPtr[kChannels] = {out.data(), out.data() + kBlock};
    double inEnergy = 0, outEnergy = 0, peak = 0;
    uint64_t bad = 0, frame = 0;
    LARGE_INTEGER f, t0, t1;
    ::QueryPerformanceFrequency(&f);
    double worstUs = 0, totalUs = 0;
    const int blocks = static_cast<int>(2 * kRate / kBlock);
    uint32_t seed = 1;
    for (int b = 0; b < blocks; ++b) {
        for (uint32_t i = 0; i < kBlock; ++i, ++frame) {
            seed = seed * 1664525u + 1013904223u;
            const float noise = (float(seed >> 8) / float(1u << 24) - 0.5f) * 0.02f;
            in[i] = 0.25f * float(std::sin(2 * 3.14159265358979 * 1000.0 * double(frame) / kRate)) + noise;
            in[kBlock + i] = 0.25f * float(std::sin(2 * 3.14159265358979 * 440.0 * double(frame) / kRate)) + noise;
        }
        ::QueryPerformanceCounter(&t0);
        p.Process(inPtr, outPtr, kChannels, kBlock);
        ::QueryPerformanceCounter(&t1);
        const double us = double(t1.QuadPart - t0.QuadPart) * 1e6 / double(f.QuadPart);
        totalUs += us;
        worstUs = std::max(worstUs, us);
        if (p.Crashed()) break;
        for (size_t k = 0; k < in.size(); ++k) {
            if (!std::isfinite(out[k])) {
                ++bad;
                continue;
            }
            inEnergy += double(in[k]) * in[k];
            outEnergy += double(out[k]) * out[k];
            peak = std::max(peak, double(std::abs(out[k])));
        }
    }
    if (p.Crashed()) {
        print(L"CRASHED while " + std::wstring(p.CrashWhere() ? p.CrashWhere() : L"?") + L": " + plugins::ExceptionName(p.CrashCode()));
        return 3;
    }
    print(std::format(L"processed {} blocks of {}: gain {:+.2f} dB, peak {:.3f}, invalid samples {}, {:.1f} us/block avg, {:.1f} "
                      L"max ({:.1f} % of real time)",
                      blocks, kBlock, 10.0 * std::log10((outEnergy + 1e-30) / (inEnergy + 1e-30)), peak, bad, totalUs / blocks,
                      worstUs, 100.0 * totalUs / blocks / (kBlock / kRate * 1e6)));
    const auto state = p.SaveState();
    bool roundTrip = !state.empty();
    if (roundTrip) {
        auto r = p.LoadState(state);
        roundTrip = r.has_value();
        if (!r) print(L"state load failed: " + r.error().Message());
    }
    const auto again = p.SaveState();
    print(std::format(L"state: {} bytes, reload {}, identical after reload: {}", state.size(), roundTrip ? L"ok" : L"FAILED",
                      again == state ? L"yes" : L"no"));

    const std::wstring editorSec = ArgAfter(args, L"--editor");
    if (!editorSec.empty() && p.HasEditor()) {
        const bool scaled = !HasFlag(args, L"--windows-scaling");
        const DPI_HOSTING_BEHAVIOR prevHosting =
            ::SetThreadDpiHostingBehavior(scaled ? DPI_HOSTING_BEHAVIOR_DEFAULT : DPI_HOSTING_BEHAVIOR_MIXED);
        const DPI_AWARENESS_CONTEXT prev = ::SetThreadDpiAwarenessContext(
            scaled ? DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2 : DPI_AWARENESS_CONTEXT_UNAWARE_GDISCALED);
        WNDCLASSW wc{};
        wc.lpfnWndProc = ::DefWindowProcW;
        wc.hInstance = ::GetModuleHandleW(nullptr);
        wc.hbrBackground = static_cast<HBRUSH>(::GetStockObject(BLACK_BRUSH));
        wc.lpszClassName = L"dgmodPluginTest";
        ::RegisterClassW(&wc);
        HWND w = ::CreateWindowExW(0, wc.lpszClassName, (p.Name() + L" — dgmod test").c_str(),
                                   WS_OVERLAPPED | WS_CAPTION | WS_SYSMENU | WS_CLIPCHILDREN, 100, 100, 400, 300, nullptr, nullptr,
                                   wc.hInstance, nullptr);
        const int desktop = _wtoi(ArgAfter(args, L"--desktop").c_str());
        if (desktop > 0) print(MoveToDesktop(w, desktop) ? std::format(L"editor window on desktop {}", desktop) : L"desktop move failed");
        cb.editor = w;
        const float scale = scaled ? float(::GetDpiForWindow(w)) / 96.0f : 1.0f;
        auto size = p.OpenEditor(w, scale);
        if (!size) {
            print(L"editor failed: " + size.error().Message());
        } else {
            cb.EditorResizeRequest(p, size->cx, size->cy);
            ::ShowWindow(w, SW_SHOWNOACTIVATE);
            print(std::format(L"editor {}x{} (scale {:.2f}, scales itself: {}, resizable: {})", size->cx, size->cy, scale,
                              p.EditorScales() ? L"yes" : L"no", p.EditorResizable() ? L"yes" : L"no"));
            const ULONGLONG end = ::GetTickCount64() + ULONGLONG(_wtof(editorSec.c_str()) * 1000.0);
            while (::GetTickCount64() < end && !p.Crashed()) {
                MSG msg;
                while (::PeekMessageW(&msg, nullptr, 0, 0, PM_REMOVE)) {
                    ::TranslateMessage(&msg);
                    ::DispatchMessageW(&msg);
                }
                p.Idle();
                // Keep the processor running, as the bridge does, so meters in the editor move.
                p.Process(inPtr, outPtr, kChannels, kBlock);
                ::Sleep(10);
            }
            const std::wstring capture = ArgAfter(args, L"--capture");
            if (!capture.empty()) print(CapturePng(w, capture) ? L"captured " + capture : std::wstring(L"capture failed"));
            p.CloseEditor();
            print(p.Crashed() ? L"editor CRASHED: " + plugins::ExceptionName(p.CrashCode()) : std::wstring(L"editor closed"));
        }
        ::DestroyWindow(w);
        ::SetThreadDpiAwarenessContext(prev);
        ::SetThreadDpiHostingBehavior(prevHosting);
    }
    print(std::format(L"callbacks: {} resize, {} state change, {} restart", cb.resizes, cb.changes, cb.restarts));
    p.Deactivate();
    loaded->reset();
    print(L"unloaded");
    return 0;
}

int PluginHostTestCommand(const std::vector<std::wstring>& args, const std::function<void(const std::wstring&)>& print) {
    constexpr wchar_t kKey[] = L"Software\\dgmod\\PluginsTest";
    size_t at = 0;
    while (at < args.size() && args[at] != L"--plugin-host-test") ++at;
    PluginChainConfig cfg;
    for (size_t i = at + 1; i < args.size() && !args[i].starts_with(L"--"); ++i) {
        PluginEntry e;
        e.id = 9001 + static_cast<uint32_t>(cfg.plugins.size());
        e.path = args[i];
        e.format = PluginFormatOf(e.path);
        e.name = e.path.substr(e.path.find_last_of(L'\\') + 1);
        cfg.plugins.push_back(e);
    }
    if (cfg.plugins.empty()) {
        print(L"usage: --plugin-host-test <path>...");
        return 1;
    }
    SavePluginChain(cfg, kKey);
    constexpr double kRate = 48000;
    constexpr uint32_t kBlock = 480, kChannels = 2;
    plugins::PluginHost host(kKey, false);
    auto report = [&](const wchar_t* when) {
        BridgeStatusData d{};
        host.Snapshot(d);
        print(std::format(L"[{}] on={} count={} latency={:.2f} ms cpu={:.1f} us bad={} passed-by={}", when, d.pluginsOn, d.pluginCount,
                          d.pluginLatencyMs, d.pluginCpuUs, d.pluginBadSamples, d.pluginSkips));
        for (uint32_t i = 0; i < d.pluginCount; ++i)
            print(std::format(L"    #{} {} state={} flags={:x} lat={} cpu={:.1f} {}", d.plugins[i].id, d.plugins[i].name,
                              static_cast<uint32_t>(d.plugins[i].state), d.plugins[i].flags, d.plugins[i].latencyFrames,
                              d.plugins[i].cpuUs, d.plugins[i].message));
    };
    const auto t0 = ::GetTickCount64();
    host.Start(60000);
    print(std::format(L"host started in {} ms", ::GetTickCount64() - t0));
    host.PrepareSession(kRate, kBlock, kChannels);
    report(L"prepared");

    // A render thread at real-time pace.
    std::atomic<bool> quit{false};
    std::atomic<uint64_t> blocks{0}, bad{0};
    std::atomic<float> maxStep{0};
    std::thread render([&] {
        _mm_setcsr(_mm_getcsr() | 0x8040);
        std::vector<float> x(size_t(kBlock) * kChannels);
        uint64_t n = 0;
        float prev = 0;
        LARGE_INTEGER f, start, now;
        ::QueryPerformanceFrequency(&f);
        ::QueryPerformanceCounter(&start);
        while (!quit) {
            for (uint32_t i = 0; i < kBlock; ++i, ++n) {
                const float v = 0.25f * float(std::sin(2 * 3.14159265358979 * 440.0 * double(n) / kRate));
                x[size_t(i) * 2] = v;
                x[size_t(i) * 2 + 1] = v;
            }
            host.Chain().Process(x.data(), kBlock);
            for (uint32_t i = 0; i < kBlock; ++i) {
                const float v = x[size_t(i) * 2];
                if (!std::isfinite(v)) ++bad;
                maxStep = std::max(maxStep.load(), std::abs(v - prev));
                prev = v;
            }
            ++blocks;
            const double due = double(blocks.load()) * kBlock / kRate;
            for (;;) {
                ::QueryPerformanceCounter(&now);
                const double t = double(now.QuadPart - start.QuadPart) / double(f.QuadPart);
                if (t >= due || quit) break;
                ::Sleep(1);
            }
        }
    });
    auto wait = [](int ms) { ::Sleep(static_cast<DWORD>(ms)); };
    wait(1500);
    report(L"playing");
    cfg.plugins[0].enabled = false;
    SavePluginChain(cfg, kKey);
    wait(800);
    report(L"first bypassed");
    if (cfg.plugins.size() > 1) {
        std::swap(cfg.plugins[0], cfg.plugins[1]);
        SavePluginChain(cfg, kKey);
        wait(800);
        report(L"reordered");
        const uint32_t removed = cfg.plugins.back().id;
        cfg.plugins.pop_back();
        SavePluginChain(cfg, kKey);
        wait(800);
        report(L"last removed");
        print(std::format(L"state file of the removed plug-in {}", ::GetFileAttributesW(PluginStatePath(removed).c_str()) ==
                                                                        INVALID_FILE_ATTRIBUTES ? L"deleted" : L"STILL THERE"));
    }
    cfg.enabled = false;
    SavePluginChain(cfg, kKey);
    wait(500);
    report(L"all off");
    quit = true;
    render.join();
    host.Stop();
    print(std::format(L"{} blocks rendered ({:.1f} s), invalid samples {}, largest step {:.4f} (the 440 Hz sine alone: {:.4f})",
                      blocks.load(), blocks.load() * kBlock / kRate, bad.load(), maxStep.load(),
                      0.25 * 2 * 3.14159265358979 * 440.0 / kRate));
    for (const auto& e : cfg.plugins) {
        const std::wstring path = PluginStatePath(e.id);
        const bool saved = ::GetFileAttributesW(path.c_str()) != INVALID_FILE_ATTRIBUTES;
        print(std::format(L"settings of #{} {}", e.id, saved ? L"saved" : L"not saved (unchanged)"));
        ::DeleteFileW(path.c_str());
    }
    ::RegDeleteKeyW(HKEY_CURRENT_USER, kKey);
    return 0;
}

}  // namespace dgmod::bridge
