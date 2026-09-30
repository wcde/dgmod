#include "app/Pages.h"

#include <cmath>
#include <format>

namespace dgmod::app::pages {

using namespace ui;

namespace {

std::wstring PresetDescription(dsp::QualityPreset q) {
    switch (q) {
        case dsp::QualityPreset::Fast: return L"100 dB stop band, 90 % pass band. Lowest CPU and latency.";
        case dsp::QualityPreset::Balanced: return L"120 dB stop band, 94 % pass band. Transparent for 16-bit material.";
        case dsp::QualityPreset::High: return L"140 dB stop band, 96 % pass band. Below the 24-bit noise floor.";
        case dsp::QualityPreset::Ultra: return L"150 dB stop band, 98 % pass band. Steepest filter, highest CPU and latency.";
        default: return L"Your own stop-band attenuation and pass-band edge.";
    }
}

// Magnitude response plot: dB scale from +6 down to the stop band, linear frequency axis.
void ResponsePlot(Ui& ui, Rect r, const FilterPreview& p) {
    const auto& t = ui.T();
    Rect area = r;
    const Rect xAxis = CutBottom(area, 18.f, 4.f);
    const Rect yAxis = CutLeft(area, 52.f, 6.f);
    const Rect plot = area;
    if (plot.w < 20.f || plot.h < 20.f || p.responseDb.empty()) return;

    const float top = 6.f;
    // NOS: the hold's own roll-off (0 to -40 dB) is what matters; its anti-alias filter lies beyond the plot.
    const float bottom = p.info.spec.response == dsp::FilterResponse::Nos
                             ? -60.f
                             : -static_cast<float>(std::ceil((p.spec.attenuationDb + 25.0) / 20.0) * 20.0);
    auto mapY = [&](float db) { return plot.y + (top - std::clamp(db, bottom, top)) / (top - bottom) * plot.h; };
    auto mapX = [&](double hz) { return plot.x + static_cast<float>(hz / p.maxFreqHz) * plot.w; };

    TextStyle ls = ui.Secondary(11.f);
    ls.color = t.textTertiary;
    for (float db = 0.f; db >= bottom; db -= 20.f) {
        const float y = std::round(mapY(db)) + 0.5f;
        ui.Line({plot.x, y}, {plot.R(), y}, db == 0.f ? WithAlpha(t.text, 0.18f) : t.chartGrid);
        TextStyle st = ls;
        st.h = HAlign::Right;
        ui.Text({yAxis.x, y - 8.f, yAxis.w, 16.f}, std::format(L"{:.0f} dB", db), st);
    }
    const double stepHz = p.maxFreqHz > 100000 ? 20000 : p.maxFreqHz > 50000 ? 10000 : 5000;
    for (double f = 0; f <= p.maxFreqHz + 1; f += stepHz) {
        const float x = std::round(mapX(f)) + 0.5f;
        ui.Line({x, plot.y}, {x, plot.B()}, t.chartGrid);
        TextStyle st = ls;
        st.h = HAlign::Center;
        ui.Text({x - 30.f, xAxis.y, 60.f, xAxis.h}, std::format(L"{:g}k", f / 1000.0), st);
    }

    // Pass band and stop band edges (NOS: the hold's sin(x)/x roll-off instead).
    if (p.info.spec.response == dsp::FilterResponse::Nos) {
        TextStyle es = ls;
        es.color = t.accentText;
        ui.Text({plot.x + 8.f, plot.y + 4.f, 360.f, 16.f},
                std::format(L"sample hold · anti-alias filter {:.0f}k–{:.0f}k", p.info.passbandHz / 1000.0,
                            p.info.stopbandHz / 1000.0),
                es);
    } else {
    const Color edge = WithAlpha(t.accentText, 0.55f);
    const float xp = mapX(p.info.passbandHz), xs = mapX(p.info.stopbandHz);
    ui.Fill({xp, plot.y, std::max(1.f, xs - xp), plot.h}, WithAlpha(t.accent, 0.07f));
    ui.Line({xp, plot.y}, {xp, plot.B()}, edge);
    ui.Line({xs, plot.y}, {xs, plot.B()}, edge);
    TextStyle es = ls;
    es.color = t.accentText;
    es.h = HAlign::Right;
    ui.Text({xp - 124.f, plot.y + 4.f, 120.f, 16.f}, std::format(L"pass {:.1f}k", p.info.passbandHz / 1000.0), es);
    es.h = HAlign::Left;
    ui.Text({xs + 4.f, plot.y + 4.f, 120.f, 16.f}, std::format(L"stop {:.2f}k", p.info.stopbandHz / 1000.0), es);

    // Required attenuation.
    const float ya = std::round(mapY(-static_cast<float>(p.spec.attenuationDb))) + 0.5f;
    ui.Line({xs, ya}, {plot.R(), ya}, WithAlpha(t.caution, 0.7f));
    }

    std::vector<D2D1_POINT_2F> pts;
    pts.reserve(p.responseDb.size());
    for (size_t i = 0; i < p.responseDb.size(); ++i) {
        const double f = p.maxFreqHz * double(i) / double(p.responseDb.size() - 1);
        pts.push_back({mapX(f), mapY(p.responseDb[i])});
    }
    ui.PushClip(plot);
    ui.FillArea(pts, plot.B(), WithAlpha(t.accent, 0.28f), WithAlpha(t.accent, 0.02f));
    ui.Polyline(pts, t.accent, 1.6f);
    ui.PopClip();
}

// Impulse response around the main peak: time axis in ms relative to the peak, amplitude normalized to the peak.
void ImpulsePlot(Ui& ui, Rect r, const FilterPreview& p) {
    const auto& t = ui.T();
    Rect area = r;
    const Rect xAxis = CutBottom(area, 18.f, 4.f);
    const Rect yAxis = CutLeft(area, 52.f, 6.f);
    const Rect plot = area;
    if (plot.w < 20.f || plot.h < 20.f || p.impulse.size() < 2) return;

    float lo = 0.f, hi = 1.f;
    for (const float v : p.impulse) lo = std::min(lo, v), hi = std::max(hi, v);
    lo = std::floor(lo * 10.f - 0.5f) / 10.f;
    hi = 1.1f;
    auto mapY = [&](float v) { return plot.y + (hi - v) / (hi - lo) * plot.h; };
    auto mapX = [&](double ms) {
        return plot.x + static_cast<float>((ms - p.impulseStartMs) / (p.impulseEndMs - p.impulseStartMs)) * plot.w;
    };

    TextStyle ls = ui.Secondary(11.f);
    ls.color = t.textTertiary;
    for (float v = 1.f; v >= lo - 1e-3f; v -= 0.5f) {
        const float y = std::round(mapY(v)) + 0.5f;
        ui.Line({plot.x, y}, {plot.R(), y}, v == 0.f ? WithAlpha(t.text, 0.18f) : t.chartGrid);
        TextStyle st = ls;
        st.h = HAlign::Right;
        ui.Text({yAxis.x, y - 8.f, yAxis.w, 16.f}, std::format(L"{:.1f}", v), st);
    }
    const double span = p.impulseEndMs - p.impulseStartMs;
    const double step = span > 1.2 ? 0.25 : span > 0.6 ? 0.1 : 0.05;
    for (double ms = std::ceil(p.impulseStartMs / step) * step; ms <= p.impulseEndMs + 1e-9; ms += step) {
        const float x = std::round(mapX(ms)) + 0.5f;
        ui.Line({x, plot.y}, {x, plot.B()}, std::abs(ms) < 1e-9 ? WithAlpha(t.accentText, 0.55f) : t.chartGrid);
        TextStyle st = ls;
        st.h = HAlign::Center;
        ui.Text({x - 30.f, xAxis.y, 60.f, xAxis.h}, std::format(L"{:+.2f} ms", ms), st);
    }

    std::vector<D2D1_POINT_2F> pts;
    pts.reserve(p.impulse.size());
    for (size_t i = 0; i < p.impulse.size(); ++i) {
        const double ms = p.impulseStartMs + span * double(i) / double(p.impulse.size() - 1);
        pts.push_back({mapX(ms), mapY(p.impulse[i])});
    }
    ui.PushClip(plot);
    ui.Polyline(pts, t.accent, 1.6f);
    ui.PopClip();
}

}  // namespace

void Filter(Ui& ui, Rect area, AppModel& m) {
    const auto& t = ui.T();
    const Id sid = ui.MakeId(L"resampler-scroll");
    auto& sc = m.scroll[static_cast<int>(Page::Filter)];
    const float y0 = ui.BeginScroll(sid, area, sc);
    Rect r{area.x + kPadX, y0 + kPadTop, area.w - 2 * kPadX, 10000.f};
    const float startY = r.y;

    PageHeader(ui, r, L"Filter", L"Oversampling filter of the bridge; changes apply to the running bridge within a second");
    if (!m.bridgeError.empty()) Notice(ui, r, Severity::Critical, L"Settings not saved", m.bridgeError);

    BridgeConfig& s = m.bridge;
    bool edited = false;

    // ---- Quality
    SectionTitle(ui, r, L"Quality");
    {
        const Rect card = CutTop(r, 118.f, 4.f);
        ui.Card(card);
        Rect in = card.Inset(18.f, 14.f);
        static const std::wstring kPresets[] = {L"Fast", L"Balanced", L"High", L"Ultra", L"Custom"};
        int q = static_cast<int>(s.quality);
        if (ui.Segmented(ui.MakeId(L"preset"), CutTop(in, 36.f, 10.f), kPresets, q)) {
            s.quality = static_cast<dsp::QualityPreset>(q);
            edited = true;
        }
        const auto plan = dsp::Oversampler::Plan(m.PreviewSourceRate(), m.PreviewDeviceRate(), s.Spec());
        TextStyle ds = ui.Secondary(13.f);
        ui.Text(CutTop(in, 20.f, 4.f), PresetDescription(s.quality), ds);
        ds.color = t.textTertiary;
        const std::wstring stages =
            plan.Cascade() ? std::format(L"{} taps at {} + {}-tap interpolator", plan.filter.taps, RateText(plan.filter.outRate),
                                         plan.interp.taps)
                           : std::format(L"{} taps", plan.filter.taps);
        ui.Text(CutTop(in, 18.f), std::format(L"{} → {}: {}, latency {:.2f} ms, ~{:.0f} M multiply-adds/s per channel",
                                              RateText(m.PreviewSourceRate()), RateText(m.PreviewDeviceRate()), stages,
                                              plan.LatencySeconds() * 1e3, plan.MacsPerSecondPerChannel() / 1e6),
                ds);
    }
    if (s.quality == dsp::QualityPreset::Custom) {
        TextStyle vs = ui.Body(13.f);
        vs.h = HAlign::Right;
        const Rect c1 = SettingRow(ui, r, L"Stop-band attenuation", L"Suppression of images above the source Nyquist", 320.f);
        float a = static_cast<float>(s.custom.attenuationDb);
        if (ui.Slider(ui.MakeId(L"atten"), {c1.x, c1.y, c1.w - 76.f, c1.h}, a, static_cast<float>(dsp::kMinAttenuationDb),
                      static_cast<float>(dsp::kMaxAttenuationDb), 1.f)) {
            s.custom.attenuationDb = a;
            edited = true;
        }
        ui.Text({c1.R() - 70.f, c1.y, 70.f, c1.h}, std::format(L"{:.0f} dB", s.custom.attenuationDb), vs);
        const Rect c2 = SettingRow(ui, r, L"Pass-band edge", L"Highest frequency kept flat, relative to the source Nyquist", 320.f);
        float pb = static_cast<float>(s.custom.passband * 100.0);
        if (ui.Slider(ui.MakeId(L"passband"), {c2.x, c2.y, c2.w - 76.f, c2.h}, pb, static_cast<float>(dsp::kMinPassband * 100.0),
                      static_cast<float>(dsp::kMaxPassband * 100.0), 0.1f)) {
            s.custom.passband = pb / 100.0;
            edited = true;
        }
        ui.Text({c2.R() - 70.f, c2.y, 70.f, c2.h}, std::format(L"{:.1f} %", s.custom.passband * 100.0), vs);
    }

    // ---- Impulse response shape
    SectionTitle(ui, r, L"Shape");
    const bool nos = s.filterResponse == dsp::FilterResponse::Nos;
    {
        const Rect c = SettingRow(ui, r, L"Roll-off",
                                  L"Sharp: flat to Nyquist · Gaussian: 10× shorter ringing · Slow: soft treble · NOS: no filter",
                                  440.f);
        // Display order Sharp, Gaussian, Slow, NOS (stored values 0, 3, 1, 2).
        static const std::wstring kResponses[] = {L"Sharp", L"Gaussian", L"Slow", L"NOS"};
        static constexpr dsp::FilterResponse kResponseOrder[] = {dsp::FilterResponse::Sharp, dsp::FilterResponse::Gaussian,
                                                                 dsp::FilterResponse::Slow, dsp::FilterResponse::Nos};
        int rs = static_cast<int>(std::find(std::begin(kResponseOrder), std::end(kResponseOrder), s.filterResponse) -
                                  std::begin(kResponseOrder));
        if (ui.Segmented(ui.MakeId(L"response"), c, kResponses, rs)) {
            s.filterResponse = kResponseOrder[rs];
            edited = true;
        }
    }
    {
        const Rect c = SettingRow(ui, r, L"Phase",
                                  nos ? L"NOS has no ringing: always linear phase"
                                      : L"Linear: ringing before and after · Intermediate: 30 dB less before · Minimum: none before",
                                  360.f);
        // Display order Linear, Intermediate, Minimum (stored values 0, 2, 1).
        static const std::wstring kPhases[] = {L"Linear", L"Intermediate", L"Minimum"};
        static constexpr dsp::FilterPhase kOrder[] = {dsp::FilterPhase::Linear, dsp::FilterPhase::Intermediate,
                                                      dsp::FilterPhase::Minimum};
        const dsp::FilterPhase shown = nos ? dsp::FilterPhase::Linear : s.filterPhase;
        int ph = static_cast<int>(std::find(std::begin(kOrder), std::end(kOrder), shown) - std::begin(kOrder));
        if (ui.Segmented(ui.MakeId(L"phase"), c, kPhases, ph, !nos)) {
            s.filterPhase = kOrder[ph];
            edited = true;
        }
    }
    const bool sharp = s.filterResponse == dsp::FilterResponse::Sharp;
    const bool apodizable = sharp || s.filterResponse == dsp::FilterResponse::Gaussian;
    {
        const Rect c = SettingRow(ui, r, L"Apodizing",
                                  apodizable ? L"Stop band below the source Nyquist: also removes the recording's own filter ringing"
                                             : L"Sharp and Gaussian only: the slow and NOS responses are already down at the "
                                               L"source Nyquist",
                                  60.f);
        bool apod = apodizable && s.apodizing;
        if (ui.Toggle(ui.MakeId(L"apodizing"), {c.R() - 44.f, c.y, 44.f, c.h}, apod, {}, apodizable)) {
            s.apodizing = apod;
            edited = true;
        }
    }
    {
        // Equiripple needs the two-stage oversampler (output at least twice the source rate).
        const auto plan = dsp::Oversampler::Plan(m.PreviewSourceRate(), m.PreviewDeviceRate(), s.Spec());
        const bool cascade = plan.Cascade();
        const Rect c = SettingRow(ui, r, L"Design",
                                  !sharp      ? L"Sharp roll-off only"
                                  : !cascade  ? L"Equiripple needs an output rate of at least twice the source rate"
                                              : L"Kaiser: windowed sinc · Equiripple: optimal (Parks-McClellan), 15-25 % "
                                                L"shorter for the same edges",
                                  260.f);
        static const std::wstring kDesigns[] = {L"Kaiser", L"Equiripple"};
        int ds = sharp && cascade ? static_cast<int>(s.filterDesign) : 0;
        if (ui.Segmented(ui.MakeId(L"design"), c, kDesigns, ds, sharp && cascade)) {
            s.filterDesign = static_cast<dsp::FilterDesign>(ds);
            edited = true;
        }
    }
    {
        const Rect card = CutTop(r, 250.f, 4.f);
        ui.Card(card);
        Rect in = card.Inset(18.f, 14.f);
        Rect head = CutTop(in, 22.f, 8.f);
        TextStyle ls = ui.Secondary(13.f);
        ui.Text(head,
                std::format(L"Impulse response · {} roll-off · {} phase{} · peak at 0", dsp::ResponseName(s.filterResponse),
                            dsp::PhaseName(nos ? dsp::FilterPhase::Linear : s.filterPhase),
                            s.apodizing && apodizable ? L" · apodizing" : L""),
                ls);
        if (m.preview) ImpulsePlot(ui, in, *m.preview);
    }

    // ---- Level
    SectionTitle(ui, r, L"Level");
    {
        const Rect c = SettingRow(ui, r, L"Headroom", L"Attenuation before oversampling; prevents clipping of inter-sample peaks", 320.f);
        float h = static_cast<float>(s.headroomDb);
        if (ui.Slider(ui.MakeId(L"headroom"), {c.x, c.y, c.w - 76.f, c.h}, h, 0.f, 12.f, 0.5f)) {
            s.headroomDb = h;
            edited = true;
        }
        TextStyle vs = ui.Body(13.f);
        vs.h = HAlign::Right;
        ui.Text({c.R() - 70.f, c.y, 70.f, c.h}, s.headroomDb > 0 ? std::format(L"−{:.1f} dB", s.headroomDb) : L"0 dB", vs);
    }
    {
        const Rect c = SettingRow(ui, r, L"True-peak limiter",
                                  L"Holds inter-sample peaks of loud masters at −0.1 dBFS instead of clipping them",
                                  60.f);
        edited |= ui.Toggle(ui.MakeId(L"limiter"), {c.R() - 44.f, c.y, 44.f, c.h}, s.peakLimiter);
    }

    // ---- Filter response
    SectionTitle(ui, r, L"Filter response");
    {
        const Rect card = CutTop(r, 360.f, 4.f);
        ui.Card(card);
        Rect in = card.Inset(18.f, 14.f);
        Rect head = CutTop(in, 32.f, 10.f);
        TextStyle ls = ui.Secondary(13.f);
        ui.Text(head, std::format(L"Source {} → output {}", RateText(m.PreviewSourceRate()), RateText(m.PreviewDeviceRate())), ls);
        const Rect info = CutBottom(in, 20.f, 8.f);
        if (m.preview) {
            ResponsePlot(ui, in, *m.preview);
            const auto& k = m.preview->info;
            const auto& plan = m.preview->plan;
            TextStyle is = ui.Secondary(12.f);
            is.color = t.textTertiary;
            const std::wstring window = k.spec.design == dsp::FilterDesign::Equiripple ? std::wstring(L"equiripple")
                                        : k.gaussSigma > 0 ? std::format(L"Gaussian σ {:.1f}", k.gaussSigma)
                                                           : std::format(L"Kaiser β {:.2f}", k.beta);
            const std::wstring layout =
                plan.Cascade() ? std::format(L"{} taps, x2 exact · {}-tap interpolator × {} phases", k.taps, plan.interp.taps,
                                             plan.interp.tablePhases)
                               : std::format(L"{} taps × {} table phases (cubic interpolation)", k.taps, k.tablePhases);
            ui.Text(info, std::format(L"{} · {} · latency {:.2f} ms{}", layout, window, plan.LatencySeconds() * 1e3,
                                      m.previewPending ? L" · updating…" : L""),
                    is);
        } else {
            ui.ProgressRing({in.Cx(), in.Cy()}, 14.f, t.accent);
            ui.RequestFrame();
        }
    }

    if (edited) m.BridgeEdited();
    ui.EndScroll(sid, area, sc, r.y - startY + kPadTop + 16.f);
}

}  // namespace dgmod::app::pages
