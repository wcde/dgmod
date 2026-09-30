#include "app/Pages.h"

#include <format>
#include <functional>

namespace dgmod::app::pages {

using namespace ui;

namespace {

std::wstring RoleText(const AppModel& m, const RenderEndpoint& e) {
    if (e.id == m.bridge.sourceId) return L"Bridge source";
    if (e.id == m.bridge.outputId) return L"Bridge output";
    return L"—";
}

}  // namespace

void Devices(Ui& ui, Rect area, AppModel& m) {
    const auto& t = ui.T();
    const Id sid = ui.MakeId(L"devices-scroll");
    auto& sc = m.scroll[static_cast<int>(Page::Devices)];
    const float y0 = ui.BeginScroll(sid, area, sc);
    Rect r{area.x + kPadX, y0 + kPadTop, area.w - 2 * kPadX, 10000.f};
    const float startY = r.y;

    const Rect actions = PageHeader(ui, r, L"Devices", L"Playback devices and the format Windows mixes them at");
    if (ui.Button(ui.MakeId(L"refresh"), {actions.R() - 120.f, actions.y, 120.f, 32.f}, L"Refresh", ButtonKind::Standard,
                  glyph::Refresh))
        m.RefreshEndpoints();

    // ---- Endpoint table
    SectionTitle(ui, r, L"Playback devices");
    static const TableColumn kCols[] = {
        {L"Device", 0, 1.6f, HAlign::Left, 180.f},
        {L"Role", 130, 0, HAlign::Left, 0, 1},
        {L"Device format", 0, 1.f, HAlign::Left, 150.f},
        {L"Mix format", 0, 1.f, HAlign::Left, 150.f, 2},
    };
    const float tableH = 38.f + 32.f * static_cast<float>(std::min<size_t>(m.renderDevices.size(), 10)) + 8.f;
    const Rect tableR = CutTop(r, std::max(tableH, 120.f), kGap);
    ui.Card(tableR);
    m.devicesTable.selected = m.current;
    const Color accent = t.accentText;
    std::function<TableCell(int, int)> cell = [&](int row, int col) -> TableCell {
        const RenderEndpoint& e = m.renderDevices[size_t(row)];
        switch (col) {
            case 0: return {e.name + (e.id == m.defaultDeviceId ? L"  ·  default" : L"")};
            case 1: {
                const std::wstring role = RoleText(m, e);
                return {role, role != L"—" ? &accent : nullptr};
            }
            case 2: return {e.deviceFormat.sampleRate ? FormatText(e.deviceFormat) : L"—"};
            case 3: return {e.mixFormat.sampleRate ? FormatText(e.mixFormat) : L"—"};
            default: return {};
        }
    };
    ui.Table(ui.MakeId(L"endpoints"), tableR.Inset(4.f), kCols, static_cast<int>(m.renderDevices.size()), m.devicesTable,
             cell);
    if (m.devicesTable.selected != m.current) m.Select(m.devicesTable.selected);

    // ---- Selected endpoint
    if (const RenderEndpoint* ep = m.Current()) {
        SectionTitle(ui, r, ep->name);
        const Rect card = CutTop(r, 96.f, kGap);
        ui.Card(card);
        Rect in = card.Inset(20.f, 14.f);

        Rect row = CutTop(in, 32.f, 10.f);
        ui.Text(CutLeft(row, 110.f, 8.f), L"Device format", ui.Body(14.f));
        if (!m.formatOptions.empty()) {
            std::vector<std::wstring> labels;
            for (const auto& o : m.formatOptions) labels.push_back(o.label);
            ui.Combo(ui.MakeId(L"format"), CutLeft(row, 300.f, 8.f), labels, m.formatChoice);
            const auto& f = ep->deviceFormat;
            const bool changed = m.formatChoice >= 0 &&
                                 (m.formatOptions[size_t(m.formatChoice)].sampleRate != f.sampleRate ||
                                  m.formatOptions[size_t(m.formatChoice)].bits != f.bitsPerSample ||
                                  m.formatOptions[size_t(m.formatChoice)].validBits != f.validBits);
            if (ui.Button(ui.MakeId(L"apply-format"), CutLeft(row, 100.f, 8.f), L"Apply", ButtonKind::Standard, 0, changed))
                m.ApplyDeviceFormat();
        } else {
            ui.Text(row, L"Reading supported formats…", ui.Secondary(13.f));
        }
        TextStyle ns = ui.Secondary(12.f);
        ns.color = t.textTertiary;
        ns.wrap = true;
        ns.v = VAlign::Top;
        ui.Text(in,
                ep->id == m.bridge.sourceId
                    ? std::wstring(L"Windows mixes every application at this rate; it is the input rate of the bridge.")
                    : L"Endpoint " + ep->id,
                ns);
    }

    ui.EndScroll(sid, area, sc, r.y - startY + kPadTop + 16.f);
}

}  // namespace dgmod::app::pages
