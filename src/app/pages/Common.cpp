#include "app/Pages.h"

#include <cmath>
#include <format>

namespace dgmod::app::pages {

using namespace ui;

Rect PageHeader(Ui& ui, Rect& r, std::wstring_view title, std::wstring_view subtitle) {
    Rect head = CutTop(r, subtitle.empty() ? 44.f : 64.f, 16.f);
    ui.Text({head.x, head.y, head.w, 40.f}, title, ui.Title(28.f));
    if (!subtitle.empty()) ui.Text({head.x, head.y + 40.f, head.w * 0.7f, 20.f}, subtitle, ui.Secondary(13.f));
    const float titleW = ui.Measure(title, ui.Title(28.f)).x;
    return Rect{head.x + titleW + 24.f, head.y + 4.f, head.w - titleW - 24.f, 32.f};
}

void SectionTitle(Ui& ui, Rect& r, std::wstring_view title) {
    ui.Text(CutTop(r, 28.f, 4.f), title, ui.Strong(16.f));
}

void StatTile(Ui& ui, Rect r, std::wstring_view label, std::wstring_view value, std::wstring_view sub, Color valueColor,
              wchar_t icon) {
    ui.Card(r);
    Rect in = r.Inset(16.f, 12.f);
    Rect top = CutTop(in, 18.f, 4.f);
    if (icon) ui.Icon(CutLeft(top, 16.f, 8.f), icon, 14.f, ui.T().textSecondary);
    ui.Text(top, label, ui.Secondary(12.f));
    TextStyle vs = ui.Title(24.f);
    vs.color = valueColor;
    while (vs.size > 15.f && ui.Measure(value, vs).x > in.w) vs.size -= 1.f;
    ui.Text(CutTop(in, 34.f, 2.f), value, vs);
    TextStyle ss = ui.Secondary(12.f);
    ss.color = ui.T().textTertiary;
    ui.Text(CutTop(in, 18.f), sub, ss);
}

void EmptyState(Ui& ui, Rect r, wchar_t icon, std::wstring_view title, std::wstring_view text) {
    const auto& t = ui.T();
    const float cy = r.Cy() - 30.f;
    ui.Icon({r.x, cy - 40.f, r.w, 40.f}, icon, 32.f, t.textTertiary);
    TextStyle ts = ui.Strong(15.f);
    ts.h = HAlign::Center;
    ui.Text({r.x, cy + 6.f, r.w, 22.f}, title, ts);
    TextStyle ss = ui.Secondary(13.f);
    ss.h = HAlign::Center;
    ss.wrap = true;
    ss.v = VAlign::Top;
    ui.Text({r.x + 24.f, cy + 32.f, r.w - 48.f, 60.f}, text, ss);
}

bool Notice(Ui& ui, Rect& r, Severity severity, std::wstring_view title, std::wstring_view message,
            std::wstring_view action, bool actionEnabled) {
    const float actionW = action.empty() ? 0.f : std::max(140.f, ui.Measure(action, ui.Body(14.f)).x + 40.f);
    const float h = ui.InfoBarHeight(r.w, title, message, actionW);
    const Rect bar = CutTop(r, h, kGap);
    const Rect act = ui.InfoBar(bar, severity, title, message, actionW);
    if (action.empty()) return false;
    return ui.Button(ui.MakeId(std::wstring(L"notice-") + std::wstring(title)), act, action, ButtonKind::Standard, 0,
                     actionEnabled);
}

Rect SettingRow(Ui& ui, Rect& r, std::wstring_view label, std::wstring_view description, float controlWidth, float height) {
    const Rect row = CutTop(r, height, 4.f);
    ui.Card(row);
    Rect in = row.Inset(18.f, 10.f);
    const Rect control = CutRight(in, controlWidth, 16.f);
    if (description.empty()) {
        ui.Text(in, label, ui.Body(14.f));
    } else {
        ui.Text({in.x, in.Cy() - 20.f, in.w, 20.f}, label, ui.Body(14.f));
        TextStyle ds = ui.Secondary(12.f);
        ds.color = ui.T().textSecondary;
        ui.Text({in.x, in.Cy(), in.w, 20.f}, description, ds);
    }
    return {control.x, row.Cy() - 16.f, control.w, 32.f};
}

std::wstring RateText(uint32_t hz) {
    if (hz == 0) return L"—";
    if (hz % 1000 == 0) return std::format(L"{} kHz", hz / 1000);
    return std::format(L"{:g} kHz", hz / 1000.0);
}

std::wstring FormatText(const StreamFormat& f) {
    if (!f.sampleRate) return L"—";
    std::wstring depth;
    if (f.isFloat) depth = std::format(L"{}-bit float", f.bitsPerSample);
    else if (f.validBits && f.validBits != f.bitsPerSample) depth = std::format(L"{}-bit ({}-bit container)", f.validBits, f.bitsPerSample);
    else depth = std::format(L"{}-bit", f.bitsPerSample);
    return std::format(L"{} · {} · {} ch", depth, RateText(f.sampleRate), f.channels);
}

std::wstring DbfsText(float linear) {
    if (linear <= 1e-7f) return L"−∞";
    return std::format(L"{:.1f} dBFS", 20.0 * std::log10(linear));
}

std::wstring CountText(uint64_t v) {
    std::wstring s = std::to_wstring(v);
    for (int i = static_cast<int>(s.size()) - 3; i > 0; i -= 3) s.insert(static_cast<size_t>(i), L" ");
    return s;
}

std::wstring TimeText(uint64_t fileTime) {
    if (!fileTime) return L"—";
    FILETIME ft{static_cast<DWORD>(fileTime), static_cast<DWORD>(fileTime >> 32)};
    FILETIME local{};
    SYSTEMTIME st{};
    ::FileTimeToLocalFileTime(&ft, &local);
    ::FileTimeToSystemTime(&local, &st);
    return std::format(L"{:02}:{:02}:{:02}.{:03}", st.wHour, st.wMinute, st.wSecond, st.wMilliseconds);
}

}  // namespace dgmod::app::pages
