#include "app/Pages.h"

#include <algorithm>
#include <format>

namespace dgmod::app::pages {

using namespace ui;

namespace {

constexpr wchar_t kGlyphMore = 0xE712;
constexpr wchar_t kGlyphAdd = 0xE710;
constexpr wchar_t kGlyphOpenEditor = 0xE8A7;  // "open in new window"

enum MenuCommand : int { kRemove = 1, kReveal, kWindowsScaling, kReload };

std::wstring FormatName(PluginFormat f) { return f == PluginFormat::Vst3 ? L"VST 3" : L"VST 2"; }

std::wstring LatencyText(uint32_t frames, uint32_t rate) {
    if (!frames) return L"no latency";
    return rate ? std::format(L"latency {:.1f} ms", frames * 1000.0 / rate) : std::format(L"latency {} samples", frames);
}

// One plug-in of the chain: its name and state, and its controls.
void ChainRow(Ui& ui, Rect& r, AppModel& m, size_t index) {
    const auto& t = ui.T();
    PluginEntry& e = m.plugins.plugins[index];
    const uint32_t id = e.id;
    const BridgePluginStatus* st = m.PluginStatus(id);
    ui.PushId(id);
    const Rect card = CutTop(r, 72.f, 4.f);
    ui.Card(card);
    Rect in = card.Inset(16.f, 12.f);

    // Number in the chain.
    const Rect num = CutLeft(in, 28.f, 12.f);
    {
        const Rect b{num.x, in.Cy() - 14.f, 28.f, 28.f};
        ui.Fill(b, e.enabled && m.plugins.enabled ? WithAlpha(t.accent, 0.18f) : t.control, 14.f);
        TextStyle ns = ui.Strong(13.f);
        ns.h = HAlign::Center;
        ns.color = e.enabled && m.plugins.enabled ? t.accent : t.textTertiary;
        ui.Text(b, std::to_wstring(index + 1), ns);
    }

    // Controls, right to left.
    bool failed = e.quarantined;
    if (st) failed |= st->state == PluginRunState::Failed || st->state == PluginRunState::Crashed ||
                      st->state == PluginRunState::Quarantined;
    const Rect menu = CutRight(in, 32.f, 4.f);
    const Id menuId = ui.MakeId(L"menu");
    if (ui.IconButton(menuId, {menu.x, in.Cy() - 16.f, 32.f, 32.f}, kGlyphMore, L"More"))
        ui.OpenMenu(menuId,
                    {MenuItem{L"Let Windows scale the editor", e.windowsScaling ? glyph::Check : wchar_t(0), kWindowsScaling},
                     MenuItem{L"Reload in the bridge", glyph::Refresh, kReload, {}, m.bridgeRunning && !e.quarantined},
                     MenuItem{L"Show the file", glyph::Document, kReveal}, MenuItem::Separator(),
                     MenuItem{L"Remove", glyph::Delete, kRemove}},
                    {menu.x, in.Cy() + 16.f});
    switch (ui.TakeMenuCommand(menuId)) {
        case kRemove:
            ui.PopId();
            m.RemovePlugin(id);
            return;
        case kReveal: m.RevealPlugin(id); break;
        case kReload: m.RetryPlugin(id); break;
        case kWindowsScaling:
            e.windowsScaling = !e.windowsScaling;
            m.PluginsEdited();
            m.ShowToast(e.windowsScaling ? L"The editor opens scaled by Windows next time" : L"The editor opens at the display's DPI next time");
            break;
        default: break;
    }
    const Rect down = CutRight(in, 32.f, 2.f), up = CutRight(in, 32.f, 12.f);
    if (ui.IconButton(ui.MakeId(L"up"), {up.x, in.Cy() - 16.f, 32.f, 32.f}, glyph::ChevronUp, L"Earlier in the chain", index > 0)) {
        ui.PopId();
        m.MovePlugin(id, -1);
        return;
    }
    if (ui.IconButton(ui.MakeId(L"down"), {down.x, in.Cy() - 16.f, 32.f, 32.f}, glyph::ChevronDown, L"Later in the chain",
                      index + 1 < m.plugins.plugins.size())) {
        ui.PopId();
        m.MovePlugin(id, +1);
        return;
    }
    const Rect tog = CutRight(in, 44.f, 16.f);
    if (ui.Toggle(ui.MakeId(L"on"), {tog.x, in.Cy() - 16.f, 44.f, 32.f}, e.enabled, {}, !failed)) m.PluginsEdited();
    ui.Tooltip(ui.MakeId(L"on-tip"), tog, e.enabled ? L"On (off = bypassed, kept running for a seamless switch)" : L"Bypassed");
    const Rect action = CutRight(in, 112.f, 16.f);
    const Rect btn{action.x, in.Cy() - 16.f, action.w, 32.f};
    if (failed) {
        if (ui.Button(ui.MakeId(L"retry"), btn, L"Retry", ButtonKind::Standard, glyph::Refresh, m.bridgeRunning || e.quarantined))
            m.RetryPlugin(id);
    } else {
        const bool open = st && (st->flags & kPluginFlagEditorOpen);
        const bool can = st && (st->flags & kPluginFlagEditor) &&
                         (st->state == PluginRunState::Ready || st->state == PluginRunState::Idle);
        if (ui.Button(ui.MakeId(L"editor"), btn, open ? L"Show" : L"Editor", ButtonKind::Standard, kGlyphOpenEditor, can))
            m.OpenPluginEditor(id);
    }

    // Name and state.
    ui.PushClip(in);
    TextStyle ns = ui.Strong(14.f);
    const std::wstring name = st && st->name[0] ? std::wstring(st->name) : e.name;
    const float nameW = std::min(ui.Measure(name, ns).x, in.w);
    ui.Text({in.x, in.Cy() - 20.f, nameW + 2.f, 20.f}, name, ns);
    TextStyle vs = ui.Secondary(12.f);
    vs.color = t.textTertiary;
    ui.Text({in.x + nameW + 8.f, in.Cy() - 20.f, in.w - nameW - 8.f, 20.f},
            std::format(L"{}{}{}", e.vendor, e.vendor.empty() ? L"" : L" · ", FormatName(e.format)), vs);
    TextStyle ss = ui.Secondary(12.f);
    std::wstring status;
    if (e.quarantined) {
        status = L"Crashed the bridge while loading: not loaded until you retry";
        ss.color = t.critical;
    } else if (!m.bridgeRunning) {
        status = L"Loads when the bridge runs";
        ss.color = t.textTertiary;
    } else if (!st || st->state == PluginRunState::Loading) {
        status = L"Loading…";
    } else if (failed) {
        status = st->message[0] ? std::wstring(st->message) : std::wstring(L"Failed");
        ss.color = t.critical;
    } else if (st->state == PluginRunState::Idle) {
        status = L"Loaded · starts with the audio session";
    } else {
        status = std::format(L"{} → {} ch · {} · {:.0f} µs per period (peak {:.0f})", st->inputs, st->outputs,
                             LatencyText(st->latencyFrames, m.bridgeStatus.inRate), st->cpuUs, st->cpuMaxUs);
        if (!e.enabled) status += L" · bypassed";
        else if (!m.plugins.enabled) status += L" · all plug-ins off";
        if (st->message[0]) {
            status += L" · ";
            status += st->message;
            ss.color = t.caution;
        }
    }
    ui.Text({in.x, in.Cy() + 2.f, in.w, 18.f}, status, ss);
    ui.PopClip();
    ui.PopId();
}

// One installed effect with its Add button.
void CatalogRow(Ui& ui, Rect& r, AppModel& m, const PluginClassInfo& c, size_t index) {
    const auto& t = ui.T();
    const Rect row = CutTop(r, 52.f, 2.f);
    ui.Card(row);
    Rect in = row.Inset(16.f, 8.f);
    ui.PushId(static_cast<uint64_t>(index) + 1000);
    const size_t uses = std::count_if(m.plugins.plugins.begin(), m.plugins.plugins.end(), [&](const PluginEntry& e) {
        return e.path == c.path && e.classId == c.classId;
    });
    const Rect add = CutRight(in, 96.f, 12.f);
    if (ui.Button(ui.MakeId(L"add"), {add.x, in.Cy() - 16.f, add.w, 32.f}, L"Add", ButtonKind::Standard, kGlyphAdd,
                  m.plugins.plugins.size() < kMaxPlugins && !m.demo))
        m.AddPlugin(c);
    if (uses) {
        const std::wstring b = uses > 1 ? std::format(L"in chain ×{}", uses) : std::wstring(L"in chain");
        const float bw = ui.BadgeWidth(b);
        ui.Badge(CutRight(in, bw, 12.f).Inset(0.f, (in.h - 20.f) / 2.f), b, t.textSecondary, t.neutralBadge);
    }
    const Rect fmt = CutRight(in, 56.f, 12.f);
    TextStyle fs = ui.Secondary(12.f);
    fs.h = HAlign::Right;
    fs.color = t.textTertiary;
    ui.Text(fmt, FormatName(c.format), fs);
    ui.PushClip(in);
    ui.Text({in.x, in.Cy() - 18.f, in.w, 18.f}, c.name, ui.Body(14.f));
    TextStyle ss = ui.Secondary(12.f);
    ss.color = t.textTertiary;
    std::wstring sub = c.vendor;
    if (!c.category.empty()) sub += (sub.empty() ? L"" : L" · ") + c.category;
    if (!c.version.empty() && c.format == PluginFormat::Vst3) sub += (sub.empty() ? L"" : L" · ") + c.version;  // VST 2 versions are vendor-coded numbers
    ui.Text({in.x, in.Cy() + 1.f, in.w, 16.f}, sub, ss);
    ui.PopClip();
    ui.PopId();
}

}  // namespace

void Plugins(Ui& ui, Rect area, AppModel& m) {
    const auto& t = ui.T();
    const Id sid = ui.MakeId(L"plugins-scroll");
    auto& sc = m.scroll[static_cast<int>(Page::Plugins)];
    const float y0 = ui.BeginScroll(sid, area, sc);
    Rect r{area.x + kPadX, y0 + kPadTop, area.w - 2 * kPadX, 10000.f};
    const float startY = r.y;

    Rect actions = PageHeader(ui, r, L"Plugins",
                              L"VST 3 and VST 2 effects at the source rate, after the tone stages; changes apply without a dropout");
    if (ui.Button(ui.MakeId(L"plugins-browse"), CutRight(actions, 150.f, 8.f), L"Add from file", ButtonKind::Standard,
                  glyph::Document, !m.demo && m.plugins.plugins.size() < kMaxPlugins))
        m.BrowsePlugin();

    if (!m.pluginsError.empty()) Notice(ui, r, Severity::Critical, L"Plug-ins not saved", m.pluginsError);
    if (!m.bridgeRunning && !m.plugins.plugins.empty())
        Notice(ui, r, Severity::Info, L"The bridge is not running",
               L"It hosts the plug-ins: they load and their editors open while it runs.");
    else if (m.bridgeRunning && m.bridgeStatus.bypass)
        Notice(ui, r, Severity::Info, L"Bypass is on", L"The bridge passes the audio through bit-exact, without the plug-ins.");
    if (m.bridgeRunning && m.bridgeStatus.pluginBadSamples)
        Notice(ui, r, Severity::Warning, L"Invalid output",
               std::format(L"The plug-ins produced {} invalid samples (replaced by silence).", CountText(m.bridgeStatus.pluginBadSamples)));

    SectionTitle(ui, r, L"Chain");
    {
        std::wstring summary = L"Processed in order, before the oversampling filter; bypassed plug-ins keep running";
        if (m.bridgeRunning && m.bridgeStatus.pluginCount) {
            uint32_t active = 0;
            for (uint32_t i = 0; i < std::min(m.bridgeStatus.pluginCount, kBridgeMaxPlugins); ++i)
                active += (m.bridgeStatus.plugins[i].flags & kPluginFlagProcessing) && (m.bridgeStatus.plugins[i].flags & kPluginFlagEnabled);
            summary = std::format(L"{} of {} active · added latency {:.1f} ms · {:.0f} µs per period", active,
                                  m.plugins.plugins.size(), m.bridgeStatus.pluginLatencyMs, m.bridgeStatus.pluginCpuUs);
        }
        const Rect c = SettingRow(ui, r, L"Plug-ins", summary, 60.f);
        if (ui.Toggle(ui.MakeId(L"plugins-on"), {c.R() - 44.f, c.y, 44.f, c.h}, m.plugins.enabled)) m.PluginsEdited();
    }
    if (m.plugins.plugins.empty()) {
        const Rect card = CutTop(r, 72.f, 4.f);
        ui.Card(card);
        Rect in = card.Inset(18.f, 12.f);
        ui.Icon(CutLeft(in, 24.f, 12.f), glyph::Puzzle, 18.f, t.textTertiary);
        ui.Text({in.x, in.Cy() - 19.f, in.w, 20.f}, L"No plug-ins in the chain", ui.Body(14.f));
        TextStyle ss = ui.Secondary(12.f);
        ss.color = t.textTertiary;
        ui.Text({in.x, in.Cy() + 1.f, in.w, 18.f}, L"Add effects from the list below, or a plug-in file from anywhere", ss);
    }
    for (size_t i = 0; i < m.plugins.plugins.size(); ++i) ChainRow(ui, r, m, i);

    // ---- Installed plug-ins
    CutTop(r, 12.f);
    {
        Rect head = CutTop(r, 32.f, 6.f);
        const Rect rescan = CutRight(head, 120.f, 8.f);
        if (ui.Button(ui.MakeId(L"plugins-rescan"), rescan, L"Rescan", ButtonKind::Standard, glyph::Refresh,
                      !m.pluginScanning && !m.demo))
            m.ScanPlugins(true);
        static const std::wstring kFilters[] = {L"All", L"VST 3", L"VST 2"};
        ui.Segmented(ui.MakeId(L"plugins-filter"), CutRight(head, 220.f, 16.f), kFilters, m.pluginFilter);
        if (m.pluginScanning) {
            const Rect ring = CutRight(head, 24.f, 8.f);
            ui.ProgressRing({ring.Cx(), ring.Cy()}, 8.f, t.accent);
            TextStyle ps = ui.Secondary(12.f);
            ps.h = HAlign::Right;
            ui.Text(CutRight(head, 180.f, 4.f),
                    m.pluginScanTotal ? std::format(L"Scanning {} of {}", m.pluginScanDone, m.pluginScanTotal)
                                      : std::wstring(L"Looking for plug-ins"),
                    ps);
            ui.RequestFrame();
        }
        ui.Text(head, L"Installed plug-ins", ui.Strong(16.f));
    }
    size_t shown = 0;
    for (size_t i = 0; i < m.pluginCatalog.size(); ++i) {
        const PluginClassInfo& c = m.pluginCatalog[i];
        if ((m.pluginFilter == 1 && c.format != PluginFormat::Vst3) || (m.pluginFilter == 2 && c.format != PluginFormat::Vst2))
            continue;
        CatalogRow(ui, r, m, c, i);
        ++shown;
    }
    if (!shown && !m.pluginScanning) {
        TextStyle ss = ui.Secondary(13.f);
        ss.color = t.textTertiary;
        ui.Text(CutTop(r, 40.f, 4.f),
                m.pluginCatalogLoaded ? L"No effects found in the standard plug-in folders (Common Files\\VST3, VSTPlugins)"
                                      : L"Open this page to look for plug-ins",
                ss);
    }
    if (!m.pluginProblems.empty()) {
        CutTop(r, 8.f);
        Rect row = CutTop(r, 32.f, 4.f);
        const Rect btn = CutRight(row, 96.f, 8.f);
        if (ui.Button(ui.MakeId(L"plugins-problems"), btn, m.pluginShowProblems ? L"Hide" : L"Show", ButtonKind::Subtle))
            m.pluginShowProblems = !m.pluginShowProblems;
        TextStyle ps = ui.Secondary(13.f);
        ui.Text(row, std::format(L"{} plug-in file{} cannot be used", m.pluginProblems.size(), m.pluginProblems.size() == 1 ? L"" : L"s"),
                ps);
        if (m.pluginShowProblems) {
            for (const auto& p : m.pluginProblems) {
                const Rect card = CutTop(r, 48.f, 2.f);
                ui.Card(card);
                Rect in = card.Inset(16.f, 6.f);
                ui.PushClip(in);
                ui.Text({in.x, in.y, in.w, 18.f}, p.path, ui.Body(13.f));
                TextStyle es = ui.Secondary(12.f);
                es.color = t.caution;
                ui.Text({in.x, in.y + 18.f, in.w, 16.f}, p.error, es);
                ui.PopClip();
            }
        }
    }

    ui.EndScroll(sid, area, sc, r.y - startY + kPadTop + 16.f);
}

}  // namespace dgmod::app::pages
