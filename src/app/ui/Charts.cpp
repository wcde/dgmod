#include "ui/Renderer.h"
#include "ui/Ui.h"

#include <algorithm>
#include <cmath>
#include <format>

namespace dgmod::ui {
namespace {

std::wstring DefaultFormat(float v) {
    if (v >= 1000000.f) return std::format(L"{:.0f} s", v / 1000000.f);
    if (v >= 1000.f) return std::format(L"{:g} ms", std::round(v / 100.f) / 10.f);
    if (v >= 10.f || v == 0.f) return std::format(L"{:.0f} us", v);
    return std::format(L"{:g} us", std::round(v * 10.f) / 10.f);
}

ID2D1StrokeStyle* DashStyle() {
    static ComPtr<ID2D1StrokeStyle> dash;
    if (!dash) {
        const float dashes[] = {4.f, 3.f};
        Factories::Get().d2d->CreateStrokeStyle(
            D2D1::StrokeStyleProperties(D2D1_CAP_STYLE_FLAT, D2D1_CAP_STYLE_FLAT, D2D1_CAP_STYLE_FLAT,
                                        D2D1_LINE_JOIN_MITER, 10.f, D2D1_DASH_STYLE_CUSTOM, 0.f),
            dashes, 2, &dash);
    }
    return dash.Get();
}

}  // namespace

void Ui::Chart(Rect r, std::span<const ChartSeries> series, std::span<const ChartThreshold> thresholds,
               const ChartOptions& opt) {
    const auto& t = *theme_;
    const auto fmt = opt.format ? opt.format : std::function<std::wstring(float)>(DefaultFormat);

    bool hasLegend = false;
    for (const auto& s : series) hasLegend |= !s.label.empty();
    Rect area = r;
    if (hasLegend) {
        Rect legend = CutTop(area, 20.f, 6.f);
        float lx = legend.x + 52.f;
        for (const auto& s : series) {
            if (s.label.empty()) continue;
            Fill({lx, legend.Cy() - 4.f, 8.f, 8.f}, s.color, 2.f);
            TextStyle st = Secondary(12.f);
            const float w = Measure(s.label, st).x;
            Text({lx + 14.f, legend.y, w + 4.f, legend.h}, s.label, st);
            lx += w + 32.f;
        }
    }
    const bool hasX = !opt.xLeftLabel.empty() || !opt.xRightLabel.empty();
    Rect xAxis = hasX ? CutBottom(area, 18.f, 4.f) : Rect{};
    Rect yAxis = CutLeft(area, 48.f, 4.f);
    const Rect plot = area;
    if (plot.w <= 4 || plot.h <= 4) return;

    size_t count = 0;
    float vmax = 0;
    for (const auto& s : series) {
        count = std::max(count, s.values.size());
        for (float v : s.values) vmax = std::max(vmax, v);
    }
    for (const auto& th : thresholds) vmax = std::max(vmax, th.value * 1.05f);
    const size_t slots = opt.capacity > 0 ? static_cast<size_t>(opt.capacity) : std::max<size_t>(count, 2);

    float yMin = 0, yMax = opt.maxValue;
    if (opt.logScale) {
        yMin = opt.minValue > 0 ? opt.minValue : 1.f;
        if (yMax <= 0) yMax = std::pow(10.f, std::ceil(std::log10(std::max(vmax * 1.2f, yMin * 10.f))));
    } else if (yMax <= 0) {
        yMax = std::max(vmax * 1.15f, 1e-3f);
        const float mag = std::pow(10.f, std::floor(std::log10(yMax)));
        const float norm = yMax / mag;
        yMax = (norm <= 2.f ? 2.f : norm <= 5.f ? 5.f : 10.f) * mag;
    }
    auto mapY = [&](float v) {
        float tt;
        if (opt.logScale) {
            const float lv = std::log10(std::max(v, yMin));
            tt = (lv - std::log10(yMin)) / (std::log10(yMax) - std::log10(yMin));
        } else {
            tt = v / yMax;
        }
        return plot.B() - std::clamp(tt, 0.f, 1.f) * plot.h;
    };

    // Grid + y labels
    std::vector<float> ticks;
    if (opt.logScale) {
        for (float v = yMin; v <= yMax * 1.001f; v *= 10.f) ticks.push_back(v);
    } else {
        for (int i = 0; i <= 4; ++i) ticks.push_back(yMax * static_cast<float>(i) / 4.f);
    }
    for (float v : ticks) {
        const float y = std::round(mapY(v)) + 0.5f;
        Line({plot.x, y}, {plot.R(), y}, t.chartGrid);
        TextStyle st = Secondary(11.f);
        st.h = HAlign::Right;
        st.color = t.textTertiary;
        Text({yAxis.x, y - 8.f, yAxis.w - 4.f, 16.f}, fmt(v), st);
    }
    if (hasX) {
        TextStyle st = Secondary(11.f);
        st.color = t.textTertiary;
        Text({plot.x, xAxis.y, plot.w * 0.5f, xAxis.h}, opt.xLeftLabel, st);
        st.h = HAlign::Right;
        Text({plot.Cx(), xAxis.y, plot.w * 0.5f, xAxis.h}, opt.xRightLabel, st);
    }

    PushClip(plot);
    const float step = plot.w / static_cast<float>(slots);
    for (const auto& s : series) {
        const size_t n = std::min(s.values.size(), slots);
        const size_t offset = s.values.size() - n;
        if (s.bars) {
            const float bw = std::max(1.f, step * 0.72f);
            for (size_t i = 0; i < n; ++i) {
                const float v = s.values[offset + i];
                if (v <= (opt.logScale ? yMin : 0.f)) continue;
                const float x = plot.R() - static_cast<float>(n - i) * step + (step - bw) * 0.5f;
                Color c = s.color;
                for (const auto& th : thresholds)
                    if (v >= th.value) c = th.color;
                const float y = mapY(v);
                Fill({x, y, bw, plot.B() - y}, c, bw > 4.f ? 1.5f : 0.f);
            }
        } else if (n >= 2) {
            std::vector<D2D1_POINT_2F> pts;
            pts.reserve(n);
            for (size_t i = 0; i < n; ++i) {
                const float x = plot.R() - static_cast<float>(n - 1 - i) * step - step * 0.5f;
                pts.push_back(D2D1::Point2F(x, mapY(s.values[offset + i])));
            }
            if (s.fill) FillArea(pts, plot.B(), WithAlpha(s.color, 0.32f), WithAlpha(s.color, 0.02f));
            Polyline(pts, s.color, 1.6f);
        }
    }
    for (const auto& th : thresholds) {
        const float y = std::round(mapY(th.value)) + 0.5f;
        dc_->DrawLine(D2D1::Point2F(plot.x, y), D2D1::Point2F(plot.R(), y), Brush(WithAlpha(th.color, 0.8f)), 1.f,
                      DashStyle());
        if (!th.label.empty()) {
            TextStyle st = Secondary(11.f);
            st.color = th.color;
            st.h = HAlign::Right;
            Text({plot.x, y - 16.f, plot.w - 4.f, 14.f}, th.label, st);
        }
    }
    PopClip();
}

void Ui::HistogramChart(Rect r, std::span<const uint64_t> buckets, float (*bucketLow)(int), Color c, float warnUs,
                        float critUs) {
    const auto& t = *theme_;
    Rect area = r;
    Rect xAxis = CutBottom(area, 16.f, 4.f);
    // Keep edge axis labels ("1 us", "10 ms") inside the rect so neighbouring charts do not overlap.
    const Rect plot = area.Inset(22.f, 0);
    if (plot.w <= 4 || plot.h <= 4 || buckets.empty()) return;
    // x range: 1 µs .. 20 ms
    const float lo = std::log10(1.f), hi = std::log10(20000.f);
    auto mapX = [&](float us) { return plot.x + (std::log10(std::max(us, 1.f)) - lo) / (hi - lo) * plot.w; };
    uint64_t maxCount = 0;
    for (uint64_t b : buckets) maxCount = std::max(maxCount, b);
    for (float v : {1.f, 10.f, 100.f, 1000.f, 10000.f}) {
        const float x = std::round(mapX(v)) + 0.5f;
        Line({x, plot.y}, {x, plot.B()}, t.chartGrid);
        TextStyle st = Secondary(11.f);
        st.color = t.textTertiary;
        st.h = HAlign::Center;
        const wchar_t* label = v >= 1000.f ? (v >= 10000.f ? L"10 ms" : L"1 ms") : (v >= 100.f ? L"100 us" : v >= 10.f ? L"10 us" : L"1 us");
        Text({x - 30.f, xAxis.y, 60.f, xAxis.h}, label, st);
    }
    Line({plot.x, plot.B() - 0.5f}, {plot.R(), plot.B() - 0.5f}, t.divider);
    if (!maxCount) return;
    const float logMax = std::log10(static_cast<float>(maxCount) + 1.f);
    for (int i = 0; i < static_cast<int>(buckets.size()); ++i) {
        if (!buckets[i]) continue;
        const float l = bucketLow(i), h = bucketLow(i + 1);
        if (h < 1.f || l > 20000.f) continue;
        const float x0 = mapX(l), x1 = mapX(h);
        const float frac = std::log10(static_cast<float>(buckets[i]) + 1.f) / logMax;
        const float bh = std::max(2.f, frac * plot.h);
        const Color col = l >= critUs ? t.critical : (l >= warnUs ? t.caution : c);
        Fill({x0 + 0.5f, plot.B() - bh, std::max(1.f, x1 - x0 - 1.f), bh}, col, 1.f);
    }
}

}  // namespace dgmod::ui
