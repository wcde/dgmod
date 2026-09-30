// dgmod: settings and monitoring UI of the dgmod bridge.
//
// Command line:
//   --status                           print the bridge status and the playback devices
//   --render-page <all|bridge|filter|tone|plugins|devices|log> [--out dir] [--demo] [--width 1280] [--height 840]
//                 [--output-mode pcm|dop|native]   (demo: the Bridge page in that output mode)
//                 [--response sharp|gaussian|slow|nos] [--design kaiser|equiripple] [--apodizing] [--phase linear|
//                 intermediate|minimum]            (demo: the Filter page with that filter)
//   --smoke <seconds> [--out dir] [--demo]  open the real window on another virtual desktop, cycle pages, capture them

#include "app/MainWindow.h"
#include "app/Pages.h"

#include <shellapi.h>
#include <shellscalingapi.h>

#include <algorithm>
#include <cmath>

#include <cstdio>
#include <format>
#include <string>
#include <thread>
#include <vector>

using namespace dgmod;
using namespace dgmod::app;

namespace {

struct Args {
    std::vector<std::wstring> items;

    [[nodiscard]] bool Has(std::wstring_view name) const {
        for (const auto& a : items)
            if (a == name) return true;
        return false;
    }
    [[nodiscard]] std::wstring Value(std::wstring_view name, std::wstring_view def = {}) const {
        for (size_t i = 0; i + 1 < items.size(); ++i)
            if (items[i] == name && !items[i + 1].starts_with(L"--")) return items[i + 1];
        return std::wstring(def);
    }
};

Args ParseArgs() {
    Args a;
    int argc = 0;
    if (wchar_t** argv = ::CommandLineToArgvW(::GetCommandLineW(), &argc)) {
        for (int i = 1; i < argc; ++i) a.items.emplace_back(argv[i]);
        ::LocalFree(argv);
    }
    return a;
}

void Print(const std::wstring& text) {
    static bool attached = false;
    HANDLE out = ::GetStdHandle(STD_OUTPUT_HANDLE);
    if ((!out || out == INVALID_HANDLE_VALUE) && !attached) {
        attached = ::AttachConsole(ATTACH_PARENT_PROCESS) != FALSE;
        out = ::GetStdHandle(STD_OUTPUT_HANDLE);
    }
    if (!out || out == INVALID_HANDLE_VALUE) return;
    const std::string utf8 = Narrow(text + L"\n");
    DWORD written = 0;
    DWORD mode = 0;
    if (::GetConsoleMode(out, &mode)) {
        const std::wstring w = text + L"\n";
        ::WriteConsoleW(out, w.data(), static_cast<DWORD>(w.size()), &written, nullptr);
    } else {
        ::WriteFile(out, utf8.data(), static_cast<DWORD>(utf8.size()), &written, nullptr);
    }
}

int PrintStatus() {
    AppModel m;
    m.Init(false);
    {
        const auto& b = m.bridgeStatus;
        Print(std::format(L"Bridge: {} state={} {} -> {} Hz latency={:.1f} ms fifo={:.1f}/{:.0f} ms drift={:+.1f} ppm ({}) "
                          L"underruns={} resyncs={} msg=\"{}\"",
                          m.bridgeRunning ? L"running" : L"not running", static_cast<int>(b.state), b.inRate, b.outRate,
                          b.totalLatencyMs, b.fifoMs, b.targetMs, b.driftPpm, b.driftLocked ? L"locked" : L"locking",
                          b.underruns, b.resyncs, b.message));
        Print(std::format(L"        delivery {:.2f} ms (observed {:.2f}..{:.2f}) level bias {:+.2f} ms", b.deliveryMs, b.deliveryMinMs,
                          b.deliveryMaxMs, b.levelBiasMs));
        if (m.bridgeRunning) {
            Print(std::format(L"        {}{}{}, limiter {} (min gain {:.2f} dB, {} frames limited), clipped={}{}",
                              b.dop         ? std::format(L"DoP DSD{}", b.dsdRate / 44100)
                              : b.nativeDsd ? std::format(L"native DSD{} via ASIO \"{}\" ({} samples/buffer, switch {:.3f} ms, "
                                                          L"{} missed)",
                                                          b.dsdRate / 44100, b.asioDriver, b.asioBufferSamples, b.asioSwitchMs,
                                                          b.lateSwitches)
                                            : std::wstring(L"PCM"),
                              std::format(L", {} roll-off, {} phase{}{}", dsp::ResponseName(static_cast<dsp::FilterResponse>(b.filterResponse)),
                                          dsp::PhaseName(static_cast<dsp::FilterPhase>(b.filterPhase)),
                                          b.filterDesign ? L", equiripple" : L"",
                                          b.interpTaps ? std::format(L", {} taps x2 + {}-tap interpolator", b.taps, b.interpTaps)
                                                       : std::format(L", {} taps", b.taps)),
                              b.apodizing ? L", apodizing" : L"",
                              b.limiter ? L"on" : L"off", 20.0 * std::log10(std::max(b.limiterMinGain, 1e-6f)),
                              b.limitedFrames, b.clipped,
                              b.dop || b.nativeDsd
                                  ? std::format(L", noise shaped to {:.0f} kHz, look-ahead {}, modulator resets={} clipped={}",
                                                b.dsdNoiseBandHz / 1000.0,
                                                b.dsdLookAheadPaths ? std::format(L"{} paths x {} bits", b.dsdLookAheadPaths,
                                                                                  b.dsdLookAheadDepth)
                                                                    : std::wstring(L"off"),
                                                b.modulatorResets, b.modulatorClips)
                                                   : std::wstring()));
            Print(std::format(L"        warmth: {}",
                              b.warmth ? std::format(L"{} {:.0f} %", dsp::WarmthName(static_cast<dsp::WarmthType>(b.warmth - 1)),
                                                     b.warmthAmount * 100.0)
                                       : std::wstring(L"off")));
            auto dbfs = [](float peak) { return peak > 1e-6f ? std::format(L"{:.1f}", 20.0 * std::log10(peak)) : std::wstring(L"-inf"); };
            Print(std::format(L"        peaks: in L {} R {} dBFS, out L {} R {} dBFS", dbfs(b.peakIn[0]), dbfs(b.peakIn[1]),
                              dbfs(b.peakOut[0]), dbfs(b.peakOut[1])));
            Print(std::format(L"        cpu {:.0f} us avg / {:.0f} us max per {:.1f} ms period, late wake-ups={} (longest gap "
                              L"{:.1f} ms), slow periods={}, rebuffers={}, discontinuities={}",
                              b.cpuAvgUs, b.cpuMaxUs, b.periodMs, b.lateWakeups, b.maxWakeGapMs, b.slowPeriods, b.rebuffers,
                              b.discontinuities));
            Print(std::format(L"        click protection: {} glitch(es) smoothed (underruns {}, re-syncs {}, source gaps {}), "
                              L"{} fade(s), {} invalid sample(s) replaced",
                              b.declicks, b.underruns, b.resyncs, b.declickGaps, b.fades, b.badSamples));
            Print(std::format(L"        plug-ins: {} ({} in the chain), latency {:.1f} ms, {:.0f} us per period, invalid samples={}, "
                              L"passed-by periods={}",
                              b.pluginsOn ? L"on" : L"off", b.pluginCount, b.pluginLatencyMs, b.pluginCpuUs, b.pluginBadSamples,
                              b.pluginSkips));
            static const wchar_t* kPluginStates[] = {L"loading", L"ready", L"idle", L"failed", L"crashed", L"quarantined"};
            for (uint32_t i = 0; i < std::min(b.pluginCount, kBridgeMaxPlugins); ++i) {
                const BridgePluginStatus& p = b.plugins[i];
                Print(std::format(L"          #{} \"{}\" {}{}{}{} {}->{} ch, latency {} frames, {:.0f} us avg / {:.0f} us max{}{}", p.id,
                                  p.name, kPluginStates[std::min<uint32_t>(static_cast<uint32_t>(p.state), 5)],
                                  p.flags & kPluginFlagEnabled ? L"" : L" (bypassed)",
                                  p.flags & kPluginFlagProcessing ? L" processing" : L"",
                                  p.flags & kPluginFlagEditorOpen ? L" editor open" : L"", p.inputs, p.outputs, p.latencyFrames,
                                  p.cpuUs, p.cpuMaxUs, p.message[0] ? L": " : L"", p.message));
            }
            std::wstring formats;
            for (uint32_t i = 0; i < std::min<uint32_t>(b.formatCount, kBridgeMaxFormats); ++i)
                formats += std::format(L" {}:{}/{}", b.formats[i].rate, b.formats[i].validBits, b.formats[i].bits);
            Print(L"        exclusive formats:" + formats);
        }
    }
    Print(L"");
    for (const auto& e : m.renderDevices) {
        const std::wstring role = e.id == m.bridge.sourceId ? L" [bridge source]" : e.id == m.bridge.outputId ? L" [bridge output]" : L"";
        Print(std::format(L"{}{}{}", e.name, e.id == m.defaultDeviceId ? L" [default]" : L"", role));
        Print(std::format(L"    {} format={} mix={}", e.id, FormatToString(e.deviceFormat), FormatToString(e.mixFormat)));
    }
    return 0;
}

int RenderPages(const Args& args) {
    ComScope com(COINIT_APARTMENTTHREADED);
    const std::wstring outDir = args.Value(L"--out", L"shots");
    const std::wstring which = args.Value(L"--render-page", L"all");
    const int width = std::stoi(args.Value(L"--width", L"1280"));
    const int height = std::stoi(args.Value(L"--height", L"840"));
    AppModel m;
    m.Init(args.Has(L"--demo"));
    if (m.demo) {  // --output-mode pcm|dop|native: the Bridge page in another output mode
        const std::wstring mode = args.Value(L"--output-mode");
        if (mode == L"dop") m.bridge.outputMode = OutputMode::Dop;
        else if (mode == L"native") m.bridge.outputMode = OutputMode::DsdNative;
        const std::wstring response = args.Value(L"--response"), design = args.Value(L"--design"), phase = args.Value(L"--phase");
        if (response == L"gaussian") m.bridge.filterResponse = dsp::FilterResponse::Gaussian;
        else if (response == L"slow") m.bridge.filterResponse = dsp::FilterResponse::Slow;
        else if (response == L"nos") m.bridge.filterResponse = dsp::FilterResponse::Nos;
        if (design == L"equiripple") m.bridge.filterDesign = dsp::FilterDesign::Equiripple;
        if (phase == L"minimum") m.bridge.filterPhase = dsp::FilterPhase::Minimum;
        else if (phase == L"intermediate") m.bridge.filterPhase = dsp::FilterPhase::Intermediate;
        if (args.Has(L"--apodizing")) m.bridge.apodizing = true;
        m.RebuildDemoPreview();
    }
    ::CreateDirectoryW(outDir.c_str(), nullptr);

    ui::Renderer renderer;
    if (auto r = renderer.InitOffscreen(); !r) {
        Print(L"Renderer init failed: " + r.error().Message());
        return 2;
    }
    ui::Ui uiCtx;
    Shell shell;
    ShellState state;
    const ui::Theme theme = ui::Theme::Load();
    constexpr const wchar_t* kNames[] = {L"bridge", L"filter", L"tone", L"plugins", L"devices", L"log"};
    int saved = 0;
    for (float scale : {1.0f, 1.5f}) {
        for (int p = 0; p < static_cast<int>(Page::Count); ++p) {
            if (which != L"all" && which.find(kNames[p]) == std::wstring::npos) continue;
            m.page = static_cast<Page>(p);
            // Let background work (filter preview, device formats) finish before the final frame.
            for (int i = 0; i < 3000; ++i) {  // the plug-in scan may take a minute on first use
                m.now = 10.0 + i * 0.05;
                m.Tick();
                const bool waiting = (m.page == Page::Filter && (!m.preview || m.previewPending)) ||
                                     (m.page == Page::Plugins && m.pluginScanning);
                if (!waiting) break;
                std::this_thread::sleep_for(std::chrono::milliseconds(20));
            }
            const UINT pw = static_cast<UINT>(static_cast<float>(width) * scale);
            const UINT ph = static_cast<UINT>(static_cast<float>(height) * scale);
            const ui::Rect client{0, 0, static_cast<float>(width), static_cast<float>(height)};
            for (int frame = 0; frame < 3; ++frame) {
                auto* dc = renderer.BeginOffscreen(pw, ph, 96.f * scale);
                if (!dc) return 3;
                ui::Input input;
                m.now = 20.0 + frame * 2.0;  // large steps so animations settle
                uiCtx.Begin(dc, theme, input, m.now, nullptr);
                shell.Draw(uiCtx, client, m, state);
                uiCtx.End();
                if (frame == 2) {
                    const std::wstring path = std::format(L"{}\\{}_{:.0f}.png", outDir, kNames[p], scale * 100.f);
                    if (auto r = renderer.EndOffscreenAndSave(path); !r) {
                        Print(L"Save failed: " + r.error().Message());
                        return 4;
                    }
                    ++saved;
                } else {
                    dc->EndDraw();
                }
            }
        }
    }
    Print(std::format(L"Saved {} images to {}", saved, outDir));
    return 0;
}

}  // namespace

int WINAPI wWinMain(HINSTANCE, HINSTANCE, PWSTR, int showCmd) {
    ::SetProcessDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);
    const Args args = ParseArgs();
    ComScope com(COINIT_APARTMENTTHREADED);

    if (args.Has(L"--status")) return PrintStatus();
    if (args.Has(L"--render-page")) return RenderPages(args);

    // Single instance: bring the existing window to the front.
    UniqueHandle mutex(::CreateMutexW(nullptr, TRUE, L"Local\\dgmod.SingleInstance"));
    if (::GetLastError() == ERROR_ALREADY_EXISTS) {
        if (HWND existing = ::FindWindowW(MainWindow::kClassName, nullptr)) {
            if (::IsIconic(existing)) ::ShowWindow(existing, SW_RESTORE);
            ::SetForegroundWindow(existing);
        }
        return 0;
    }

    AppModel model;
    model.Init(args.Has(L"--demo"));
    MainWindow window(model);
    const bool smoke = args.Has(L"--smoke");
    if (smoke) {
        const std::wstring dir = args.Value(L"--out");
        if (!dir.empty()) ::CreateDirectoryW(dir.c_str(), nullptr);
        window.EnableSmokeTest(std::stod(args.Value(L"--smoke", L"8")), dir);
    }
    if (auto r = window.Create(showCmd); !r) {
        if (smoke) Print(L"window creation failed: " + r.error().Message());
        else ::MessageBoxW(nullptr, r.error().Message().c_str(), L"dgmod", MB_ICONERROR);
        return 1;
    }
    const int code = window.Run();
    if (smoke)
        Print(std::format(L"smoke: {} frames, {} captures, {}", window.FramesRendered(), window.Captures(),
                          window.OnOtherDesktop() ? L"other virtual desktop" : L"off-screen"));
    return smoke && window.FramesRendered() == 0 ? 1 : code;
}
