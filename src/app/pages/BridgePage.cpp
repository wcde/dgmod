#include "app/Pages.h"

#include "bridge/SlipCopier.h"

#include <shellapi.h>

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <format>
#include <utility>
#include <vector>

namespace dgmod::app::pages {

using namespace ui;

namespace {

std::wstring StateText(BridgeState s) {
    switch (s) {
        case BridgeState::Starting: return L"Starting";
        case BridgeState::Buffering: return L"Waiting for audio";
        case BridgeState::Playing: return L"Playing";
        case BridgeState::Waiting: return L"Waiting";
        case BridgeState::Error: return L"Error";
        default: return L"Stopped";
    }
}

float MeterValue(float linear) {
    if (linear <= 1e-6f) return 0.f;
    return std::clamp(static_cast<float>((20.0 * std::log10(linear) + 60.0) / 60.0), 0.f, 1.f);
}

constexpr double kLevelWindowSec = 6.0;  // span of the level graphs
constexpr double kLevelLeadSec = 0.04;   // the right edge trails the clock by ~a period so the newest data reaches it
constexpr float kClipLevel = 0.999f;     // -0.01 dBFS

std::wstring DbText(float linear) {
    if (linear < 0.001f) return L"−∞";  // below the -60 dB floor of the graph
    return std::format(L"{:.1f}", 20.0 * std::log10(linear));
}

// Scrolling level history of one stage: L above the centre line, R mirrored below it, both on a -60..0 dBFS scale.
// Samples are grouped into columns fixed in time (not in pixels), so the curve scrolls smoothly without shimmering.
void LevelGraph(Ui& ui, Rect r, std::wstring_view title, const AppModel& m, bool output) {
    const auto& t = ui.T();
    const auto& hist = m.bridgeMeter;
    const double right = m.bridgeMeterNow - kLevelLeadSec;
    const auto level = [output](const AppModel::LevelSample& s, int c) { return output ? s.out[c] : s.in[c]; };
    const Color colors[2] = {t.series[0], t.series[1]};

    // Header: title and the peaks of the last half second (slow enough to read).
    Rect head = CutTop(r, 20.f, 8.f);
    float hold[2]{};
    for (auto it = hist.rbegin(); it != hist.rend() && it->t > right - 0.5; ++it)
        for (int c = 0; c < 2; ++c) hold[c] = std::max(hold[c], level(*it, c));
    {
        TextStyle us = ui.Secondary(12.f);
        us.color = t.textTertiary;
        ui.Text(CutRight(head, 30.f, 4.f), L"dBFS", us);
        for (int c = 1; c >= 0; --c) {
            TextStyle vs = ui.Body(13.f);
            vs.mono = true;
            vs.h = HAlign::Right;
            vs.color = hold[c] >= kClipLevel ? t.critical : t.text;
            ui.Text(CutRight(head, 44.f, 4.f), DbText(hold[c]), vs);
            ui.Text(CutRight(head, 10.f, 4.f), c ? L"R" : L"L", ui.Secondary(12.f));
            const Rect sw = CutRight(head, 8.f, c ? 16.f : 0.f);
            ui.Fill({sw.x, sw.Cy() - 4.f, 8.f, 8.f}, colors[c], 2.f);
        }
        ui.Text(head, title, ui.Strong(13.f));
    }

    Rect xAxis = CutBottom(r, 14.f, 4.f);
    Rect yAxis = CutLeft(r, 26.f, 6.f);
    const Rect plot = r;
    if (plot.w <= 8.f || plot.h <= 8.f) return;
    const float cy = plot.Cy(), half = plot.h * 0.5f;
    const auto mapY = [&](float v, int c) { return cy + (c ? 1.f : -1.f) * v * half; };
    const auto mapX = [&](double time) { return plot.R() - static_cast<float>((right - time) / kLevelWindowSec) * plot.w; };

    // Grid: 0 / -20 / -40 dBFS on both halves, a line per second.
    TextStyle gs = ui.Secondary(11.f);
    gs.color = t.textTertiary;
    gs.h = HAlign::Right;
    for (int db : {0, -20, -40}) {
        const float v = float(db + 60) / 60.f;
        for (int c = 0; c < 2; ++c) {
            const float y = std::round(std::clamp(mapY(v, c), plot.y, plot.B() - 1.f)) + 0.5f;
            ui.Line({plot.x, y}, {plot.R(), y}, t.chartGrid);
            ui.Text({yAxis.x, y - 8.f, yAxis.w, 16.f}, std::format(L"{}", db), gs);
        }
    }
    for (int s = 1; s < static_cast<int>(kLevelWindowSec); ++s) {
        const float x = std::round(plot.R() - float(s / kLevelWindowSec) * plot.w) + 0.5f;
        ui.Line({x, plot.y}, {x, plot.B()}, WithAlpha(t.chartGrid, t.chartGrid.a * 0.6f));
    }
    ui.Line({plot.x, std::round(cy) + 0.5f}, {plot.R(), std::round(cy) + 0.5f}, t.divider);
    gs.h = HAlign::Left;
    ui.Text({plot.x, xAxis.y, plot.w * 0.5f, xAxis.h}, std::format(L"−{:.0f} s", kLevelWindowSec), gs);
    gs.h = HAlign::Right;
    ui.Text({plot.Cx(), xAxis.y, plot.w * 0.5f, xAxis.h}, L"now", gs);

    // Curves: max per column of ~2 px; a pause of the bridge breaks them.
    ui.PushClip(plot);
    const double colDur = kLevelWindowSec * 2.0 / plot.w;
    std::vector<D2D1_POINT_2F> pts[2];
    std::vector<std::pair<float, int>> clips;  // x, channel
    const auto draw = [&] {
        for (int c = 0; c < 2; ++c) {
            if (pts[c].size() >= 2) {
                ui.FillArea(pts[c], cy, WithAlpha(colors[c], 0.42f), WithAlpha(colors[c], 0.05f));
                ui.Polyline(pts[c], colors[c], 1.25f);
            }
            pts[c].clear();
        }
    };
    int64_t col = INT64_MIN;
    float peak[2]{};
    double lastT = -1e300;
    const auto flush = [&] {
        const float x = mapX((double(col) + 0.5) * colDur);
        for (int c = 0; c < 2; ++c) {
            pts[c].push_back(D2D1::Point2F(x, mapY(MeterValue(peak[c]), c)));
            if (peak[c] >= kClipLevel) clips.emplace_back(x, c);
        }
    };
    for (const auto& s : hist) {
        if (s.t < right - kLevelWindowSec - colDur) continue;
        if (s.t > right) break;
        const auto k = static_cast<int64_t>(std::floor(s.t / colDur));
        if (k != col) {
            if (col != INT64_MIN) flush();
            if (s.t - lastT > 0.25) draw();
            col = k;
            peak[0] = peak[1] = 0.f;
        }
        for (int c = 0; c < 2; ++c) peak[c] = std::max(peak[c], level(s, c));
        lastT = s.t;
    }
    if (col != INT64_MIN) flush();
    draw();
    for (const auto& [x, c] : clips) ui.Fill({x - 1.f, c ? plot.B() - 3.f : plot.y, 2.f, 3.f}, t.critical);
    ui.PopClip();
}

// Device picker over the active playback devices; returns true when the selection changed. A selected device that is
// not active (unplugged, or held by its ASIO driver) is shown by its saved name and `missingNote`.
bool DeviceCombo(Ui& ui, Id id, Rect r, const AppModel& m, std::wstring& selected, const std::wstring& exclude,
                 const std::wstring& savedName, std::wstring_view missingNote) {
    std::vector<std::wstring> names;
    std::vector<const RenderEndpoint*> items;
    int index = -1;
    for (const auto& d : m.renderDevices) {
        if (d.id == exclude) continue;
        if (d.id == selected) index = static_cast<int>(items.size());
        std::wstring label = d.name;
        if (d.mixFormat.sampleRate) label += std::format(L"  ·  {}", RateText(d.mixFormat.sampleRate));
        names.push_back(label);
        items.push_back(&d);
    }
    if (index < 0) {
        names.insert(names.begin(), selected.empty()   ? std::wstring(L"Choose a device…")
                                    : savedName.empty() ? L"(" + std::wstring(missingNote) + L")"
                                                        : savedName + L"  ·  " + std::wstring(missingNote));
        items.insert(items.begin(), nullptr);
        index = 0;
    }
    int chosen = index;
    if (!ui.Combo(id, r, names, chosen) || chosen == index || !items[size_t(chosen)]) return false;
    selected = items[size_t(chosen)]->id;
    return true;
}

std::wstring AgoText(double sec) {
    if (sec < 60.0) return std::format(L"{:.0f} s", std::max(sec, 0.0));
    if (sec < 3600.0) return std::format(L"{:.0f} min", sec / 60.0);
    return std::format(L"{:.1f} h", sec / 3600.0);
}

// What the click protection did in the current session: glitches smoothed by kind, fades, replaced samples.
void ClickProtectionCard(Ui& ui, Rect& r, const BridgeStatusData& st) {
    const auto& t = ui.T();
    const Rect card = CutTop(r, 104.f, kGap);
    ui.Card(card);
    Rect in = card.Inset(18.f, 12.f);
    Rect head = CutTop(in, 22.f, 10.f);
    ui.Icon(CutLeft(head, 16.f, 8.f), glyph::Shield, 14.f, t.textSecondary);
    const double ago = st.lastDeclickTime ? double(FileTimeNow() - st.lastDeclickTime) * 1e-7 : -1.0;
    const bool recent = ago >= 0.0 && ago < 10.0;
    const std::wstring badge = ago < 0.0 ? std::wstring(L"no glitches") : L"last " + AgoText(ago) + L" ago";
    const float bw = ui.BadgeWidth(badge);
    const Rect br = CutRight(head, bw, 8.f);
    ui.Badge({br.x, br.Cy() - 10.f, br.w, 20.f}, badge, recent ? t.caution : t.success, recent ? t.cautionBg : t.successBg);
    ui.Text(head, L"Click protection", ui.Strong(14.f));

    struct Item {
        const wchar_t* label;
        uint64_t value;
        Color hot;  // color of a non-zero value
        const wchar_t* tip;
    };
    const Item items[] = {
        {L"Glitches smoothed", st.declicks, t.caution, L"Underruns, re-syncs and source gaps turned into a 5 ms transition"},
        {L"Underruns", st.underruns, t.caution, L"The buffer ran dry: the audio was continued by prediction and faded out"},
        {L"Re-syncs", st.resyncs, t.caution, L"Excess audio dropped from the buffer, crossfaded over the jump"},
        {L"Source gaps", st.declickGaps, t.caution, L"Samples lost in the captured stream, crossfaded over the gap"},
        {L"Fades", st.fades, t.text, L"Soft starts and stops: playback start/stop, sound starting or ending in the source"},
        {L"Invalid samples", st.badSamples, t.critical, L"NaN, infinity or above +18 dBFS from an application, replaced"},
    };
    const auto cols = Columns(in, static_cast<int>(std::size(items)), 12.f);
    for (size_t i = 0; i < std::size(items); ++i) {
        Rect c = cols[i];
        ui.Tooltip(ui.MakeId(std::wstring(L"declick-") + items[i].label), c, items[i].tip);
        TextStyle vs = ui.Strong(20.f);
        vs.color = items[i].value ? items[i].hot : t.textTertiary;
        ui.Text(CutTop(c, 28.f, 2.f), std::format(L"{}", items[i].value), vs);
        ui.Text(CutTop(c, 16.f), items[i].label, ui.Secondary(12.f));
    }
}

}  // namespace

void Bridge(Ui& ui, Rect area, AppModel& m) {
    const auto& t = ui.T();
    const Id sid = ui.MakeId(L"bridge-scroll");
    auto& sc = m.scroll[static_cast<int>(Page::Bridge)];
    const float y0 = ui.BeginScroll(sid, area, sc);
    Rect r{area.x + kPadX, y0 + kPadTop, area.w - 2 * kPadX, 10000.f};
    const float startY = r.y;

    const BridgeStatusData& st = m.bridgeStatus;
    const bool running = m.bridgeRunning;
    const Rect actions =
        PageHeader(ui, r, L"Bridge", L"Oversampling of everything Windows plays: virtual cable → dgmod → DAC in exclusive mode");
    {
        Rect a = actions;
        const bool configured = !m.bridge.sourceId.empty() && !m.bridge.outputId.empty();
        if (ui.Button(ui.MakeId(L"bridge-start"), CutRight(a, 130.f, 8.f), running ? L"Stop" : L"Start",
                      running ? ButtonKind::Standard : ButtonKind::Accent, running ? glyph::Stop : glyph::Play,
                      running || configured)) {
            if (running) m.StopBridge();
            else m.StartBridge();
        }
        // A/B switch: the DAC gets the source audio untouched at the source rate.
        if (ui.Button(ui.MakeId(L"bridge-bypass"), CutRight(a, 130.f, 8.f), m.bridge.bypass ? L"Bypass on" : L"Bypass",
                      m.bridge.bypass ? ButtonKind::Accent : ButtonKind::Standard, glyph::Switch, running)) {
            m.bridge.bypass = !m.bridge.bypass;
            m.BridgeEdited();
        }
    }

    // ---- Guidance and problems
    const RenderEndpoint* source = m.FindRenderDevice(m.bridge.sourceId);
    if (!source) {
        if (Notice(ui, r, Severity::Info, L"Enable a virtual audio device",
                   L"Windows plays into a virtual cable (for example VB-Audio Voicemeeter AUX Input or VB-CABLE), dgmod "
                   L"oversamples that mix and sends it to the DAC. Enable one under Sound settings → All sound devices, then "
                   L"choose it as the source below.",
                   L"Open Sound settings"))
            ::ShellExecuteW(nullptr, L"open", L"ms-settings:sound", nullptr, nullptr, SW_SHOWNORMAL);
    }
    if (running && (st.state == BridgeState::Waiting || st.state == BridgeState::Error) && st.message[0])
        Notice(ui, r, st.state == BridgeState::Error ? Severity::Critical : Severity::Warning,
               st.state == BridgeState::Error ? L"The bridge hit an error and will retry" : L"The bridge is waiting", st.message);
    if (m.bridgeForeign &&
        Notice(ui, r, Severity::Warning, L"Another version of the bridge is running",
               L"Its status cannot be read by this version of dgmod, and a new bridge cannot start while it runs. Restart "
               L"it to switch to this version.",
               L"Restart"))
        m.StartBridge();
    if (!m.bridgeError.empty()) Notice(ui, r, Severity::Critical, L"Settings not saved", m.bridgeError);
    if (m.bridge.Dop() && !(running && st.dop && st.state == BridgeState::Playing))
        Notice(ui, r, Severity::Warning, L"Turn the volume down before the first DSD start",
               L"DSD reaches the DAC as DoP: 16 DSD bits and a marker in every 24-bit PCM sample. A DAC that recognises "
               L"the markers switches to DSD (its display shows DSD); one that does not plays them as loud noise.");
    if (m.bridge.NativeDsd() && !m.demo && m.asioDrivers.empty())
        Notice(ui, r, Severity::Critical, L"No ASIO driver is installed",
               L"Native DSD goes to the DAC through the ASIO driver of its maker (for example the FiiO USB DAC driver). "
               L"Install it, or use DSD (DoP).");
    else if (m.bridge.NativeDsd() && !(running && st.nativeDsd && st.state == BridgeState::Playing))
        Notice(ui, r, Severity::Info, L"Native DSD through the ASIO driver",
               L"The bit stream reaches the DAC as raw DSD through its ASIO driver, without DoP markers, up to DSD512 and "
               L"beyond. The driver switches the DAC to DSD itself. Keep the volume low for the first start with a new DAC.");
    if (running && st.modulatorResets)
        Notice(ui, r, Severity::Warning, L"The DSD modulator was overloaded",
               std::format(L"{} reset(s) of the delta-sigma loop (each is an audible click). Keep the true-peak limiter on "
                           L"or choose the standard DSD level.",
                           st.modulatorResets));
    else if (running && st.modulatorClips)
        Notice(ui, r, Severity::Info, L"The DSD modulator reached its limit",
               std::format(L"Peaks beyond its stable range were held by its overload protection for {:.1f} ms (briefly more "
                           L"noise, no click). Keep the true-peak limiter on or choose the standard DSD level to avoid it.",
                           st.dsdRate ? st.modulatorClips * 1000.0 / st.dsdRate / std::max<uint32_t>(st.channels, 1) : 0.0));
    if (running && st.badSamples)
        Notice(ui, r, Severity::Warning, L"Invalid samples were replaced",
               std::format(L"{} sample(s) arrived as NaN, infinity or above +18 dBFS (a misbehaving application or effect) "
                           L"and were replaced before they could reach the filters as a spike.",
                           st.badSamples));
    if (running && m.bridge.bypass) {
        const bool active = st.bypass != 0;
        const bool settled = st.state == BridgeState::Playing || st.state == BridgeState::Buffering;
        if (active)
            Notice(ui, r, Severity::Info, L"Bypass: oversampling is off",
                   std::format(L"The DAC runs at the source rate ({}) and receives the audio unchanged: no filter, no headroom, "
                               L"no dither. Clock drift is absorbed by rare one-sample slips, each smoothed over {} samples. "
                               L"Press Bypass again to return to oversampling; the next start of the bridge always oversamples.",
                               RateText(st.inRate), bridge::kSlipRampFrames));
        else if (settled)
            Notice(ui, r, Severity::Warning, L"Bypass is not possible with this DAC",
                   std::format(L"The DAC does not accept {} in exclusive mode, so the bridge keeps oversampling.",
                               RateText(st.inRate)));
    }
    if (m.outputHasEqualizerApo)
        Notice(ui, r, Severity::Info, L"Effects on the DAC are bypassed while the bridge runs",
               L"Exclusive mode skips the Windows audio engine of the output device, including Equalizer APO. Install "
               L"Equalizer APO on the source (virtual) device with its Configurator so it runs before oversampling.");

    // ---- Live status
    {
        const Rect tiles = CutTop(r, 104.f, kGap);
        const auto cols = Columns(tiles, 4, kGap);
        const Color stateColor = !running                               ? t.textSecondary
                                 : st.state == BridgeState::Playing     ? t.success
                                 : st.state == BridgeState::Error       ? t.critical
                                                                        : t.caution;
        StatTile(ui, cols[0], L"State", running ? StateText(st.state) : L"Stopped",
                 running ? std::format(L"{} rebuffer(s) · {} resync(s) · {} late", st.rebuffers, st.resyncs,
                                       st.lateWakeups + st.lateSwitches)
                         : L"press Start",
                 stateColor, glyph::Lightning);
        const auto phase = static_cast<dsp::FilterPhase>(st.filterPhase);
        const std::wstring base = st.apodizing ? std::format(L"{} · apodizing", phase == dsp::FilterPhase::Intermediate ? L"interm."
                                                                                  : phase == dsp::FilterPhase::Minimum ? L"min-phase"
                                                                                                                       : L"linear")
                                                : std::format(L"{} phase", dsp::PhaseName(phase));
        const auto response = static_cast<dsp::FilterResponse>(st.filterResponse);
        const std::wstring design = st.filterDesign == static_cast<uint32_t>(dsp::FilterDesign::Equiripple) ? L" · equiripple" : L"";
        const std::wstring shape = response == dsp::FilterResponse::Nos        ? std::wstring(L"NOS")
                                   : response == dsp::FilterResponse::Slow     ? L"slow · " + base
                                   : response == dsp::FilterResponse::Gaussian ? L"Gaussian · " + base
                                                                               : base + design;
        const std::wstring lookAhead = st.dsdLookAheadPaths ? std::format(L" · look-ahead {}", st.dsdLookAheadPaths) : L"";
        StatTile(ui, cols[1], L"Source → output",
                 !running || !st.outRate ? std::wstring(L"—")
                 : st.dop || st.nativeDsd ? std::format(L"{} → DSD{}", RateText(st.inRate), st.dsdRate / 44100)
                                         : std::format(L"{} → {}", RateText(st.inRate), RateText(st.outRate)),
                 !running || !st.outRate ? std::wstring(L"not running")
                 : st.bypass             ? std::format(L"bypass · bit-exact · {}-bit · {} ch", st.outValidBits, st.channels)
                 : st.dop       ? std::format(L"DoP {}{} · {}", RateText(st.outRate), lookAhead, shape)
                 : st.nativeDsd ? std::format(L"native · ASIO{} · {}", lookAhead, shape)
                          : std::format(L"{}-bit · {} taps · {}", st.outValidBits,
                                        st.interpTaps ? std::format(L"{}+{}", st.taps, st.interpTaps) : std::format(L"{}", st.taps),
                                        shape),
                 t.accentText, glyph::Audio);
        StatTile(ui, cols[2], L"Latency", running && st.totalLatencyMs > 0 ? std::format(L"{:.0f} ms", st.totalLatencyMs) : L"—",
                 running ? std::format(L"buffer {:.1f} / {:.0f} ms · period {:.1f} ms", st.fifoMs, st.targetMs, st.periodMs)
                         : L"buffer + period + filter",
                 t.text, glyph::Clock);
        const double load = st.periodMs > 0 ? st.cpuAvgUs / (st.periodMs * 1000.0) * 100.0 : 0.0;
        std::wstring level;
        if (st.limiter && st.limiterMinGain < 0.9999f)
            level = std::format(L"limiting {:.1f} dB", 20.0 * std::log10(std::max(st.limiterMinGain, 1e-6f)));
        else if (st.clipped)
            level = std::format(L"{} clipped", st.clipped);
        else
            level = st.limiter ? L"limiter idle" : L"no clipping";
        StatTile(ui, cols[3], L"Clock drift", running ? std::format(L"{:+.1f} ppm", st.driftPpm) : L"—",
                 running ? std::format(L"{} · CPU {:.1f} % · {}", st.driftLocked ? L"locked" : L"locking", load, level)
                         : L"virtual device vs. DAC",
                 t.text, glyph::Processing);
    }
    if (running) {
        m.PollBridgeMeter(kLevelWindowSec + 1.0);
        const Rect card = CutTop(r, 200.f, kGap);
        ui.Card(card);
        const auto halves = Columns(card.Inset(18.f, 14.f), 2, 32.f);
        LevelGraph(ui, halves[0], L"Source", m, false);
        LevelGraph(ui, halves[1], L"Output", m, true);
        ui.RequestFrame();  // the graphs scroll at the display refresh rate
    }
    if (running) ClickProtectionCard(ui, r, st);

    // ---- Devices
    SectionTitle(ui, r, L"Devices");
    bool edited = false;
    {
        const Rect c = SettingRow(ui, r, L"Source", L"Virtual device Windows plays into (set as default while running)", 360.f);
        edited |= DeviceCombo(ui, ui.MakeId(L"bridge-source"), c, m, m.bridge.sourceId, m.bridge.outputId,
                              m.bridge.sourceName, L"not available");
    }
    {
        const Rect c = SettingRow(ui, r, L"Output",
                                  m.bridge.NativeDsd() ? L"Your DAC; native DSD reaches it through its ASIO driver"
                                                       : L"Your DAC; opened in exclusive mode",
                                  360.f);
        // While the ASIO driver holds the DAC for native DSD, Windows does not list it as an active device.
        const bool viaAsio = m.bridgeRunning && m.bridgeStatus.nativeDsd && m.bridge.outputId == m.bridgeStatus.outputId;
        if (DeviceCombo(ui, ui.MakeId(L"bridge-output"), c, m, m.bridge.outputId, m.bridge.sourceId, m.bridge.outputName,
                        viaAsio ? L"in use via ASIO" : L"not available")) {
            edited = true;
            m.bridge.outputRate = 0;
            m.bridgeFormatsFor.clear();
        }
    }
    {
        const Rect c = SettingRow(ui, r, L"Output", L"Oversampled PCM, or DSD from dgmod's own delta-sigma modulator", 360.f);
        static const std::wstring kModes[] = {L"PCM", L"DSD (DoP)", L"DSD (native)"};
        int mode = static_cast<int>(m.bridge.outputMode);
        if (ui.Segmented(ui.MakeId(L"bridge-mode"), c, kModes, mode)) {
            m.bridge.outputMode = static_cast<OutputMode>(mode);
            edited = true;
        }
    }
    if (m.bridge.NativeDsd()) {
        const Rect c = SettingRow(ui, r, L"ASIO driver", L"The DAC maker's driver; it must support native DSD", 360.f);
        const AsioDriverEntry* matched = FindAsioDriver(m.asioDrivers, L"", [&] {
            const RenderEndpoint* out = m.FindRenderDevice(m.bridge.outputId);
            return out ? out->name : m.bridge.outputName;
        }());
        std::vector<std::wstring> labels{matched ? std::format(L"Automatic ({})", matched->name)
                                                 : std::wstring(L"Automatic (none matches the output)")};
        int index = 0;
        for (size_t i = 0; i < m.asioDrivers.size(); ++i) {
            labels.push_back(m.asioDrivers[i].name);
            if (_wcsicmp(m.bridge.asioDriver.c_str(), m.asioDrivers[i].name.c_str()) == 0) index = static_cast<int>(i) + 1;
        }
        if (!m.bridge.asioDriver.empty() && index == 0) {  // configured but not installed (any more)
            labels.push_back(m.bridge.asioDriver + L" · not installed");
            index = static_cast<int>(labels.size()) - 1;
        }
        if (ui.Combo(ui.MakeId(L"bridge-asio-driver"), c, labels, index)) {
            m.bridge.asioDriver = index == 0 ? std::wstring() : index <= static_cast<int>(m.asioDrivers.size())
                                                                     ? m.asioDrivers[size_t(index - 1)].name
                                                                     : m.bridge.asioDriver;
            edited = true;
        }
    }
    if (m.bridge.NativeDsd()) {
        const Rect c = SettingRow(ui, r, L"DSD rate", L"Bit rate of the modulator, sent to the ASIO driver as raw DSD", 360.f);
        const AsioDriverEntry* driver = m.NativeDsdDriver();
        const bool known = driver && m.asioDsdMask && driver->name == m.asioMaskDriver;
        auto offered = [&](uint32_t mult) { return !known || (m.asioDsdMask & DsdMaskBit(mult)) != 0; };
        uint32_t highest = 0;
        for (const uint32_t mult : kNativeDsdMultiples)
            if (!highest && mult <= kNativeDsdAutoMax && known && offered(mult)) highest = mult;
        std::vector<std::wstring> labels{highest ? std::format(L"Highest available (DSD{})", highest)
                                                 : std::wstring(L"Highest available (up to DSD512)")};
        static constexpr uint32_t kChoices[] = {64, 128, 256, 512, 1024};
        int index = 0;
        for (size_t i = 0; i < std::size(kChoices); ++i) {
            const uint32_t mult = kChoices[i];
            labels.push_back(std::format(L"DSD{} · {:.4g} MHz{}{}", mult, 44100.0 * mult / 1e6,
                                         mult > kNativeDsdAutoMax ? L" · high CPU load" : L"",
                                         offered(mult) ? L"" : L" · not offered by the driver"));
            if (m.bridge.dsdMultiple == mult) index = static_cast<int>(i) + 1;
        }
        if (ui.Combo(ui.MakeId(L"bridge-native-rate"), c, labels, index)) {
            m.bridge.dsdMultiple = index == 0 ? 0u : kChoices[size_t(index - 1)];
            edited = true;
        }
    }
    if (m.bridge.Dsd()) {
        if (m.bridge.Dop()) {
            const Rect c = SettingRow(ui, r, L"DSD rate", L"Bit rate of the modulator; DoP needs 24-bit at 1/16 of it", 360.f);
            auto offered = [&](uint32_t mult) {
                if (m.bridgeFormats.empty()) return true;  // unknown yet
                for (const auto& f : m.bridgeFormats)
                    if (f.sampleRate == DopFrameRate(mult) && f.validBits >= 24) return true;
                return false;
            };
            uint32_t highest = 0;
            for (const uint32_t mult : kDsdMultiples)
                if (!highest && offered(mult)) highest = mult;
            std::vector<std::wstring> labels{highest ? std::format(L"Highest available (DSD{})", highest)
                                                     : std::wstring(L"Highest available")};
            static constexpr uint32_t kChoices[] = {64, 128, 256};
            int index = 0;
            for (size_t i = 0; i < std::size(kChoices); ++i) {
                const uint32_t mult = kChoices[i];
                labels.push_back(std::format(L"DSD{} · {:.4g} MHz · DoP {}{}", mult, 44100.0 * mult / 1e6,
                                             RateText(DopFrameRate(mult)), offered(mult) ? L"" : L" · not offered by the DAC"));
                if (m.bridge.dsdMultiple == mult) index = static_cast<int>(i) + 1;
            }
            if (ui.Combo(ui.MakeId(L"bridge-dsd-rate"), c, labels, index)) {
                m.bridge.dsdMultiple = index == 0 ? 0u : kChoices[size_t(index - 1)];
                edited = true;
            }
        }
        {
            const Rect c = SettingRow(ui, r, L"DSD level",
                                      L"Modulation depth at PCM full scale: 50 % is the SACD reference, +3 dB gives 71 %", 260.f);
            static const std::wstring kLevels[] = {L"Standard", L"+3 dB"};
            int lv = m.bridge.dsdHighLevel ? 1 : 0;
            if (ui.Segmented(ui.MakeId(L"bridge-dsd-level"), c, kLevels, lv)) {
                m.bridge.dsdHighLevel = lv == 1;
                edited = true;
            }
        }
        {
            // The rate in use when running, else the chosen one (0 = highest available: unknown here).
            const uint32_t rate = running && (st.dop || st.nativeDsd) && st.dsdRate ? st.dsdRate : 44100u * m.bridge.dsdMultiple;
            const bool tooFast = rate > dsp::kLookAheadMaxRate;
            const Rect c = SettingRow(
                ui, r, L"Modulator look-ahead",
                tooFast ? L"Not available above DSD256"
                        : L"Pruned tree search 64 bits ahead: stable closer to full scale",
                260.f);
            static const std::wstring kModes[] = {L"Off", L"Standard", L"High"};
            int mode = static_cast<int>(m.bridge.dsdLookAhead);
            if (ui.Segmented(ui.MakeId(L"bridge-dsd-lookahead"), c, kModes, mode)) {
                m.bridge.dsdLookAhead = static_cast<dsp::LookAhead>(mode);
                edited = true;
            }
        }
    }
    if (!m.bridge.Dsd()) {
        const Rect c = SettingRow(ui, r, L"Output format", L"The highest rate gives the largest oversampling ratio", 360.f);
        uint32_t highest = 0;
        for (const auto& f : m.bridgeFormats) highest = std::max(highest, f.sampleRate);
        std::vector<std::wstring> labels{highest ? std::format(L"Highest available ({})", RateText(highest))
                                                 : std::wstring(L"Highest available")};
        int index = 0;
        for (size_t i = 0; i < m.bridgeFormats.size(); ++i) {
            const auto& f = m.bridgeFormats[i];
            labels.push_back(f.label);
            if (m.bridge.outputRate == f.sampleRate && m.bridge.outputBits == f.bits && m.bridge.outputValidBits == f.validBits)
                index = static_cast<int>(i) + 1;
        }
        if (ui.Combo(ui.MakeId(L"bridge-format"), c, labels, index)) {
            if (index == 0) {
                m.bridge.outputRate = 0;
            } else {
                const auto& f = m.bridgeFormats[size_t(index - 1)];
                m.bridge.outputRate = f.sampleRate;
                m.bridge.outputBits = f.bits;
                m.bridge.outputValidBits = f.validBits;
            }
            edited = true;
        }
    }

    // ---- Options
    SectionTitle(ui, r, L"Options");
    {
        const Rect c = SettingRow(ui, r, L"Buffer", L"Between the virtual device and the DAC; larger is safer, smaller has less latency", 320.f);
        float ms = static_cast<float>(m.bridge.bufferMs);
        if (ui.Slider(ui.MakeId(L"bridge-buffer"), {c.x, c.y, c.w - 76.f, c.h}, ms, 10.f, 200.f, 5.f)) {
            m.bridge.bufferMs = static_cast<uint32_t>(ms);
            edited = true;
        }
        TextStyle vs = ui.Body(13.f);
        vs.h = HAlign::Right;
        ui.Text({c.R() - 70.f, c.y, 70.f, c.h}, std::format(L"{} ms", m.bridge.bufferMs), vs);
    }
    {
        const Rect c = SettingRow(ui, r, L"Output period",
                                  m.bridge.NativeDsd()
                                      ? L"ASIO buffer size; longer rides out system stalls (the driver rounds to its sizes)"
                                      : L"Device period in exclusive mode; longer rides out system stalls, adds latency",
                                  320.f);
        static constexpr uint32_t kPeriods[] = {5, 10, 20, 40, 80};
        std::vector<std::wstring> labels{m.bridge.NativeDsd() ? L"Automatic (driver setting)"
                                         : m.bridge.Dop()     ? L"Automatic (10 ms)"
                                                              : L"Automatic (5 ms)"};
        int index = 0;
        for (size_t i = 0; i < std::size(kPeriods); ++i) {
            labels.push_back(std::format(L"{} ms", kPeriods[i]));
            if (m.bridge.periodMs == kPeriods[i]) index = static_cast<int>(i) + 1;
        }
        if (ui.Combo(ui.MakeId(L"bridge-period"), c, labels, index)) {
            m.bridge.periodMs = index == 0 ? 0u : kPeriods[size_t(index - 1)];
            edited = true;
        }
    }
    {
        const Rect c = SettingRow(ui, r, L"TPDF dither", L"For 16/24-bit output; digital silence stays silent", 60.f);
        edited |= ui.Toggle(ui.MakeId(L"bridge-dither"), {c.R() - 44.f, c.y, 44.f, c.h}, m.bridge.dither);
    }
    {
        const Rect c = SettingRow(ui, r, L"Switch the default device",
                                  L"Make the source the default playback device while the bridge runs, restore it afterwards", 60.f);
        edited |= ui.Toggle(ui.MakeId(L"bridge-default"), {c.R() - 44.f, c.y, 44.f, c.h}, m.bridge.switchDefault);
    }
    {
        const Rect c = SettingRow(ui, r, L"Start with Windows", L"Run the bridge in the background after sign-in", 60.f);
        bool on = m.bridgeAutostart;
        if (ui.Toggle(ui.MakeId(L"bridge-autostart"), {c.R() - 44.f, c.y, 44.f, c.h}, on)) m.SetAutostart(on);
    }
    if (edited) m.BridgeEdited();

    ui.EndScroll(sid, area, sc, r.y - startY + kPadTop + 16.f);
}

}  // namespace dgmod::app::pages
