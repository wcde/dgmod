#include "app/Pages.h"

#include "dsp/Tone.h"

#include <cmath>
#include <format>
#include <vector>

namespace dgmod::app::pages {

using namespace ui;

namespace {

// Response of the warmth equalizer relative to 1 kHz (the tonal balance), log frequency axis 20 Hz .. 20 kHz.
void TonePlot(Ui& ui, Rect r, const dsp::WarmthResponse& eq, double rate, bool on) {
    const auto& t = ui.T();
    Rect area = r;
    const Rect xAxis = CutBottom(area, 18.f, 4.f);
    const Rect yAxis = CutLeft(area, 52.f, 6.f);
    const Rect plot = area;
    if (plot.w < 20.f || plot.h < 20.f) return;
    constexpr float kTop = 6.f, kBottom = -6.f;
    auto mapY = [&](double db) { return plot.y + static_cast<float>((kTop - std::clamp(db, double(kBottom), double(kTop))) / (kTop - kBottom)) * plot.h; };
    auto mapX = [&](double hz) { return plot.x + static_cast<float>(std::log(hz / 20.0) / std::log(1000.0)) * plot.w; };
    TextStyle ls = ui.Secondary(11.f);
    ls.color = t.textTertiary;
    for (float db = kTop; db >= kBottom; db -= 3.f) {
        const float y = std::round(mapY(db)) + 0.5f;
        ui.Line({plot.x, y}, {plot.R(), y}, db == 0.f ? WithAlpha(t.text, 0.18f) : t.chartGrid);
        TextStyle st = ls;
        st.h = HAlign::Right;
        ui.Text({yAxis.x, y - 8.f, yAxis.w, 16.f}, std::format(L"{:+.0f} dB", db), st);
    }
    for (const double hz : {20.0, 50.0, 100.0, 200.0, 500.0, 1000.0, 2000.0, 5000.0, 10000.0, 20000.0}) {
        const float x = std::round(mapX(hz)) + 0.5f;
        ui.Line({x, plot.y}, {x, plot.B()}, t.chartGrid);
        TextStyle st = ls;
        st.h = HAlign::Center;
        ui.Text({x - 30.f, xAxis.y, 60.f, xAxis.h}, hz >= 1000 ? std::format(L"{:g}k", hz / 1000) : std::format(L"{:g}", hz), st);
    }
    const double ref = eq.Magnitude(1000.0, rate);
    std::vector<D2D1_POINT_2F> pts;
    constexpr int kPoints = 300;
    for (int i = 0; i < kPoints; ++i) {
        const double hz = 20.0 * std::pow(1000.0, double(i) / (kPoints - 1));
        pts.push_back({mapX(hz), mapY(on ? 20.0 * std::log10(eq.Magnitude(hz, rate) / ref) : 0.0)});
    }
    ui.PushClip(plot);
    ui.Polyline(pts, on ? t.accent : t.textTertiary, 1.8f);
    ui.PopClip();
}

}  // namespace

void Tone(Ui& ui, Rect area, AppModel& m) {
    const Id sid = ui.MakeId(L"tone-scroll");
    auto& sc = m.scroll[static_cast<int>(Page::Tone)];
    const float y0 = ui.BeginScroll(sid, area, sc);
    Rect r{area.x + kPadX, y0 + kPadTop, area.w - 2 * kPadX, 10000.f};
    const float startY = r.y;

    PageHeader(ui, r, L"Tone", L"Analog warmth of the bridge; changes apply to the running bridge without a dropout");
    if (!m.bridgeError.empty()) Notice(ui, r, Severity::Critical, L"Settings not saved", m.bridgeError);
    if (m.bridgeRunning && m.bridgeStatus.bypass)
        Notice(ui, r, Severity::Info, L"Bypass is on",
               L"The bridge passes the audio through bit-exact; tone stages apply when it oversamples again.");

    dsp::ToneSettings& tone = m.bridge.tone;
    bool edited = false;
    TextStyle vs = ui.Body(13.f);
    vs.h = HAlign::Right;

    SectionTitle(ui, r, L"Warmth");
    {
        const Rect c = SettingRow(ui, r, L"Analog warmth",
                                  L"The tonal balance of a tape or tube stage, with faint harmonics on loud peaks only", 60.f);
        edited |= ui.Toggle(ui.MakeId(L"warmth"), {c.R() - 44.f, c.y, 44.f, c.h}, tone.warmth);
    }
    {
        const Rect c = SettingRow(ui, r, L"Character",
                                  L"Tape: head bump in the deep bass, soft top · Tube: fuller low mids, smoother presence", 240.f);
        static const std::wstring kTypes[] = {L"Tube", L"Tape"};
        int ty = static_cast<int>(tone.warmthType);
        if (ui.Segmented(ui.MakeId(L"warmth-type"), c, kTypes, ty, tone.warmth)) {
            tone.warmthType = static_cast<dsp::WarmthType>(ty);
            edited = true;
        }
    }
    {
        const Rect c = SettingRow(ui, r, L"Amount", L"Depth of the response and of the harmonics", 320.f);
        float a = static_cast<float>(tone.warmthAmount * 100.0);
        if (ui.Slider(ui.MakeId(L"warmth-amount"), {c.x, c.y, c.w - 76.f, c.h}, a, 0.f, 100.f, 5.f, tone.warmth)) {
            tone.warmthAmount = a / 100.0;
            edited = true;
        }
        ui.Text({c.R() - 70.f, c.y, 70.f, c.h}, std::format(L"{:.0f} %", tone.warmthAmount * 100.0), vs);
    }
    {
        constexpr double kRate = 48000.0;
        const auto eq = dsp::WarmthEqDesign(tone.warmthType, tone.warmthAmount, kRate);
        const auto k = dsp::WarmthCoefficients(tone.warmthType, tone.warmthAmount);
        const bool tape = tone.warmthType == dsp::WarmthType::Tape;
        const double full = tape ? k.k3 / 4 : k.k2 / 2;                // main harmonic at 0 dBFS
        const double quiet = tape ? k.k3 / 4 * 0.01 : k.k2 / 2 * 0.1;  // at -20 dBFS
        const Rect card = CutTop(r, 300.f, 4.f);
        ui.Card(card);
        Rect in = card.Inset(18.f, 14.f);
        TextStyle ls = ui.Secondary(13.f);
        ui.Text(CutTop(in, 20.f, 8.f),
                tone.warmth ? std::format(L"Tonal balance (re 1 kHz) · level {:.1f} dB lower so peaks never rise · {} harmonic "
                                          L"{:.2g} % at 0 dBFS, {:.2g} % at −20 dBFS",
                                          -20.0 * std::log10(eq.gain), tape ? L"3rd" : L"2nd", 100.0 * full, 100.0 * quiet)
                            : std::wstring(L"Tonal balance (re 1 kHz) · off"),
                ls);
        TonePlot(ui, in, eq, kRate, tone.warmth);
    }

    if (edited) m.BridgeEdited();
    ui.EndScroll(sid, area, sc, r.y - startY + kPadTop + 16.f);
}

}  // namespace dgmod::app::pages
