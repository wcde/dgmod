// dgmod-bridge: background process (no window) that runs the oversampling bridge for the current user session.
//
//   dgmod-bridge.exe          run (single instance per session)
//   dgmod-bridge.exe --stop   ask a running instance to stop (restores the default playback device)
//   dgmod-bridge.exe --asio-probe
//                             list the ASIO drivers and their native DSD capabilities
//   dgmod-bridge.exe --asio-test <driver> [dsd multiple]
//                             open that driver in native DSD mode, play 3 s of modulated DSD silence the way the bridge
//                             renders (1 s of the driver's static silence first) and check that its buffer switches
//                             match the declared buffer size and none is missed (the bridge must be stopped);
//                             --tone plays 6 s of 1 / 1.5 kHz at -20 dBFS instead; --asio-probe --verbose lists
//                             channels and clocks
//   dgmod-bridge.exe --scan-plugin <path>
//                             list the effect classes of a VST 3 / VST 2 plug-in file (run by the GUI for every file)
//   dgmod-bridge.exe --plugin-test <path> [--class <id>] [--editor <seconds>] [--capture <png>] [--desktop <n>]
//                             load one plug-in as the bridge does, process a test signal, check its state round trip
//                             and optionally show its editor

#include "bridge/AsioOutput.h"
#include "bridge/Bridge.h"
#include "bridge/PluginTool.h"
#include "common/AsioDrivers.h"
#include "dsp/DeltaSigma.h"
#include "common/BridgeConfig.h"
#include "common/BridgeStatus.h"
#include "common/Registry.h"
#include "plugins/PluginHost.h"

#include <shellapi.h>

#include <cmath>
#include <format>
#include <string>
#include <thread>
#include <vector>

using namespace dgmod;

namespace {

UniqueHandle g_stop;

LRESULT CALLBACK WndProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
    switch (msg) {
        case WM_QUERYENDSESSION: return TRUE;
        case WM_ENDSESSION:
            if (wp && g_stop) ::SetEvent(g_stop.Get());
            return 0;
        default: return ::DefWindowProcW(hwnd, msg, wp, lp);
    }
}

std::vector<std::wstring> Args() {
    int argc = 0;
    wchar_t** argv = ::CommandLineToArgvW(::GetCommandLineW(), &argc);
    std::vector<std::wstring> out;
    for (int i = 1; argv && i < argc; ++i) out.emplace_back(argv[i]);
    ::LocalFree(argv);
    return out;
}

bool HasArg(std::wstring_view name) {
    for (const auto& a : Args())
        if (a == name) return true;
    return false;
}

// Console output of the diagnostic commands (a GUI-subsystem process: stdout is a redirected pipe/file, or the
// console of the parent).
void Print(const std::wstring& line) {
    HANDLE out = ::GetStdHandle(STD_OUTPUT_HANDLE);
    if (!out || out == INVALID_HANDLE_VALUE) {
        ::AttachConsole(ATTACH_PARENT_PROCESS);
        out = ::GetStdHandle(STD_OUTPUT_HANDLE);
    }
    if (!out || out == INVALID_HANDLE_VALUE) return;
    const std::string text = Narrow(line + L"\n");
    DWORD written = 0;
    ::WriteFile(out, text.data(), static_cast<DWORD>(text.size()), &written, nullptr);
}

std::wstring RatesText(uint32_t mask) {
    std::wstring s;
    for (const uint32_t m : kNativeDsdMultiples)
        if (mask & DsdMaskBit(m)) s = std::format(L" DSD{}", m) + s;
    return s.empty() ? L" none" : s;
}

int AsioProbe() {
    const auto drivers = ListAsioDrivers();
    if (drivers.empty()) Print(L"No ASIO drivers installed.");
    for (const auto& d : drivers) {
        auto c = bridge::AsioDsdOutput::Probe(d.clsid);
        if (!c) {
            Print(std::format(L"{}: {}", d.name, c.error().Message()));
            continue;
        }
        Print(std::format(L"{} (\"{}\"): {} in / {} out, native DSD: {}", d.name, c->driverName, c->inputs, c->outputs,
                          c->dsd ? L"yes" : L"no"));
        if (c->dsd)
            Print(std::format(L"    rates:{}; at the highest: buffer min {} max {} preferred {} granularity {}, sample type {}",
                              RatesText(c->supportedMask), c->minSize, c->maxSize, c->preferredSize, c->granularity,
                              c->sampleType));
        if (HasArg(L"--verbose")) Print(L"    " + c->details);
    }
    return 0;
}

int AsioTest(const std::vector<std::wstring>& args) {
    size_t at = 0;
    while (at < args.size() && args[at] != L"--asio-test") ++at;
    const std::wstring name = at + 1 < args.size() ? args[at + 1] : L"";
    const uint32_t multiple = at + 2 < args.size() ? static_cast<uint32_t>(_wtoi(args[at + 2].c_str())) : 0;
    const auto drivers = ListAsioDrivers();
    const AsioDriverEntry* d = FindAsioDriver(drivers, name, L"");
    if (!d) {
        Print(std::format(L"ASIO driver \"{}\" not found (see --asio-probe).", name));
        return 1;
    }
    bridge::AsioDsdOutput out;
    bridge::AsioDsdOutput::Request req;
    req.clsid = d->clsid;
    req.name = d->name;
    req.dsdMultiple = multiple;
    const bool tone = HasArg(L"--tone");
    uint32_t mask = 0;
    auto o = out.Open(req, mask);
    if (!o) {
        Print(L"open failed: " + o.error().Message() + L"; rates:" + RatesText(mask));
        return 1;
    }
    const double expectedMs = o->bufferSamples * 1000.0 / o->dsdRate;
    Print(std::format(L"DSD{} ({} Hz), {} of {} outputs, {} samples/buffer = {:.3f} ms expected, {}, output latency {:.2f} ms, "
                      L"outputReady {}",
                      o->dsdMultiple, o->dsdRate, o->channels, o->deviceChannels, o->bufferSamples, expectedMs,
                      o->lsbFirst ? L"LSB first" : L"MSB first", o->outputLatencyMs, o->outputReady ? L"yes" : L"no"));
    if (auto r = out.Start(); !r) {
        Print(L"start failed: " + r.error().Message());
        return 1;
    }
    ::Sleep(1000);
    // Render like the bridge: modulated silence into the half of every switch.
    dsp::DsdEncoder encoder;
    encoder.Configure(o->channels, o->dsdRate / 16, 22050.0, o->frames);
    std::vector<float> zeros(size_t(o->frames) * o->channels, 0.0f);
    std::vector<uint16_t> words(zeros.size());
    // --tone: 1 kHz at -20 dBFS (left) and 1.5 kHz (right) instead of silence, at the bridge's 50 % modulation level.
    const double toneRate = o->dsdRate / 16.0;
    uint64_t toneFrame = 0;
    auto fill = [&] {
        if (!tone) return;
        for (uint32_t i = 0; i < o->frames; ++i, ++toneFrame)
            for (uint32_t c = 0; c < o->channels; ++c)
                zeros[size_t(i) * o->channels + c] =
                    0.05f * static_cast<float>(std::sin(6.283185307179586 * (c ? 1500.0 : 1000.0) * double(toneFrame) / toneRate));
    };
    UniqueHandle quit(::CreateEventW(nullptr, TRUE, FALSE, nullptr));
    uint64_t rendered = 0;
    std::thread render([&] {
        for (;;) {
            while (out.Space() > 0) {
                fill();
                encoder.Process(zeros.data(), o->frames, words.data());
                if (!out.Push(words.data(), words.data(), o->channels, out.Epoch())) break;
                ++rendered;
            }
            HANDLE h[] = {quit.Get(), out.SwitchEvent()};
            if (::WaitForMultipleObjects(2, h, FALSE, 2000) != WAIT_OBJECT_0 + 1) break;
        }
    });
    ::Sleep(tone ? 6000 : 3000);
    const uint64_t late = out.LateSwitches();  // before the queue runs out on purpose
    ::SetEvent(quit.Get());
    render.join();
    ::Sleep(200);  // switches after the queue ran out: the callback fades out and writes silence itself
    const double measuredMs = out.MeasuredSwitchSec() * 1e3;
    const uint64_t switches = out.Switches(), overloads = out.Overloads();
    out.Close();
    const double ratio = expectedMs > 0 ? measuredMs / expectedMs : 0;
    const bool timing = measuredMs > 0 && ratio <= 1.5 && ratio >= 0.67;
    Print(std::format(L"{} buffer switches, every {:.3f} ms (ratio {:.3f}): {}", switches, measuredMs, ratio,
                      measuredMs <= 0 ? L"TOO FEW SWITCHES"
                      : !timing       ? L"MISMATCH (buffer size is not in DSD samples)"
                                      : L"OK"));
    Print(std::format(L"{} periods rendered, {} missed, {} driver overload(s), modulator resets {}: {}", rendered, late,
                      overloads, encoder.Resets(), late == 0 && rendered > 0 ? L"OK" : L"MISSED BUFFERS"));
    return timing && late == 0 && rendered > 0 ? 0 : 2;
}

}  // namespace

int WINAPI wWinMain(HINSTANCE instance, HINSTANCE, PWSTR, int) {
    if (HasArg(L"--asio-probe")) return AsioProbe();
    if (HasArg(L"--asio-test")) return AsioTest(Args());
    if (HasArg(L"--scan-plugin")) {
        const auto args = Args();
        for (size_t i = 0; i + 1 < args.size(); ++i)
            if (args[i] == L"--scan-plugin") return bridge::ScanPluginCommand(args[i + 1], Print);
        return 1;
    }
    if (HasArg(L"--plugin-test")) return bridge::PluginTestCommand(Args(), Print);
    if (HasArg(L"--plugin-host-test")) return bridge::PluginHostTestCommand(Args(), Print);
    if (HasArg(L"--stop")) {
        UniqueHandle e(::OpenEventW(EVENT_MODIFY_STATE, FALSE, kBridgeStopEventName));
        if (e) ::SetEvent(e.Get());
        return e ? 0 : 1;
    }

    UniqueHandle mutex(::CreateMutexW(nullptr, TRUE, kBridgeInstanceMutexName));
    if (::GetLastError() == ERROR_ALREADY_EXISTS) return 0;

    g_stop.Reset(::CreateEventW(nullptr, TRUE, FALSE, kBridgeStopEventName));
    ::ResetEvent(g_stop.Get());
    UniqueHandle reload(::CreateEventW(nullptr, FALSE, FALSE, nullptr));

    auto status = BridgeStatusMapping::CreateForWriter();

    // Hidden top-level window: receives WM_ENDSESSION so the default device is restored at logoff/shutdown.
    WNDCLASSW wc{};
    wc.lpfnWndProc = WndProc;
    wc.hInstance = instance;
    wc.lpszClassName = L"dgmodBridgeWindow";
    ::RegisterClassW(&wc);
    HWND hwnd = ::CreateWindowExW(0, wc.lpszClassName, L"dgmod bridge", 0, 0, 0, 0, 0, nullptr, nullptr, instance, nullptr);

    // Settings watcher: any change of HKCU\Software\dgmod wakes the engine, which reloads if needed.
    std::thread watcher([&] {
        auto key = reg::Create(HKEY_CURRENT_USER, kRegBridge, KEY_NOTIFY | KEY_QUERY_VALUE);
        if (!key) return;
        UniqueHandle changed(::CreateEventW(nullptr, FALSE, FALSE, nullptr));
        for (;;) {
            if (::RegNotifyChangeKeyValue(key->Get(), TRUE, REG_NOTIFY_CHANGE_LAST_SET | REG_NOTIFY_THREAD_AGNOSTIC,
                                          changed.Get(), TRUE) != ERROR_SUCCESS)
                return;
            HANDLE handles[] = {g_stop.Get(), changed.Get()};
            if (::WaitForMultipleObjects(2, handles, FALSE, INFINITE) == WAIT_OBJECT_0) return;
            ::SetEvent(reload.Get());
        }
    });

    // Plug-in host: its own UI thread (editor windows), the chain runs on the render thread.
    plugins::PluginHost pluginHost;
    pluginHost.Start(0);  // the engine starts at once; the plug-ins join the session when they are loaded

    bridge::BridgeEngine engine;
    std::thread worker([&] { engine.Run(g_stop.Get(), reload.Get(), status ? &*status : nullptr, &pluginHost); });

    HANDLE handles[] = {worker.native_handle()};
    for (;;) {
        if (::MsgWaitForMultipleObjects(1, handles, FALSE, INFINITE, QS_ALLINPUT) == WAIT_OBJECT_0) break;
        MSG msg;
        while (::PeekMessageW(&msg, nullptr, 0, 0, PM_REMOVE)) {
            ::TranslateMessage(&msg);
            ::DispatchMessageW(&msg);
        }
    }
    worker.join();
    pluginHost.Stop();  // saves the plug-ins' settings
    ::SetEvent(g_stop.Get());
    watcher.join();
    if (hwnd) ::DestroyWindow(hwnd);
    return 0;
}
