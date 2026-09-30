#include "app/Shell.h"

#include "app/Pages.h"

#include <format>

namespace dgmod::app {

using namespace ui;

namespace {

struct NavItem {
    Page page;
    wchar_t icon;
    const wchar_t* title;
};

constexpr NavItem kNav[] = {
    {Page::Bridge, glyph::Lightning, L"Bridge"},
    {Page::Filter, glyph::Audio, L"Filter"},
    {Page::Tone, glyph::Equalizer, L"Tone"},
    {Page::Plugins, glyph::Puzzle, L"Plugins"},
    {Page::Devices, glyph::Speakers, L"Devices"},
    {Page::Log, glyph::List, L"Log"},
};

}  // namespace

CaptionButton Shell::CaptionHit(float x, float y, float clientW) {
    if (y < 0 || y >= kCaptionH) return CaptionButton::None;
    if (x >= clientW - kCaptionW) return CaptionButton::Close;
    if (x >= clientW - 2 * kCaptionW) return CaptionButton::Maximize;
    if (x >= clientW - 3 * kCaptionW) return CaptionButton::Minimize;
    return CaptionButton::None;
}

bool Shell::InDragRegion(float x, float y, float clientW) {
    return y >= 0 && y < kTitleH && CaptionHit(x, y, clientW) == CaptionButton::None;
}

void Shell::Draw(Ui& ui, Rect client, AppModel& m, const ShellState& s) {
    const auto& t = ui.T();
    ui.Fill(client, t.windowBg);
    auto& in = ui.In();

    // Keyboard shortcuts.
    if (in.ctrl)
        for (int i = 0; i < static_cast<int>(std::size(kNav)); ++i)
            if (in.KeyPressed('1' + i)) m.page = kNav[i].page;
    if (in.KeyPressed(VK_F5)) m.RefreshEndpoints();

    // ---- Title bar
    const Rect title{client.x, client.y, client.w, kTitleH};
    const Rect appIcon{title.x + 16.f, title.Cy() - 10.f, 20.f, 20.f};
    ui.Fill(appIcon, t.accent, 5.f);
    ui.Icon(appIcon, glyph::Audio, 12.f, t.onAccent);
    TextStyle ts = ui.Body(12.f);
    ts.color = s.active ? t.text : t.textTertiary;
    const float titleW = ui.Measure(kProductTitle, ts).x;
    ui.Text({appIcon.R() + 12.f, title.y, titleW + 4.f, title.h}, kProductTitle, ts);
    float px = appIcon.R() + 12.f + titleW + 16.f;
    auto pill = [&](std::wstring_view text, Color dot) {
        TextStyle st = ui.Secondary(12.f);
        const float w = ui.Measure(text, st).x + 30.f;
        const Rect p{px, title.Cy() - 12.f, w, 24.f};
        ui.Fill(p, t.control, 12.f);
        ui.Stroke(p, t.controlStroke, 12.f);
        ui.Circle({p.x + 12.f, p.Cy()}, 4.f, dot);
        ui.Text({p.x + 22.f, p.y, w - 24.f, p.h}, text, st);
        px += w + 8.f;
    };
    if (m.demo) pill(L"Demo data", t.series[2]);

    if (s.drawCaptionButtons) {
        const CaptionButton btns[] = {CaptionButton::Minimize, CaptionButton::Maximize, CaptionButton::Close};
        for (int i = 0; i < 3; ++i) {
            const Rect b{client.R() - kCaptionW * static_cast<float>(3 - i), title.y, kCaptionW, kCaptionH};
            const bool hover = s.hover == btns[i];
            const bool pressed = s.pressed == btns[i] && hover;
            Color fg = s.active ? t.text : t.textTertiary;
            if (btns[i] == CaptionButton::Close && (hover || pressed)) {
                ui.Fill(b, pressed ? Rgb(0xC42B1C, 0.9f) : Rgb(0xC42B1C), 0);
                fg = Rgb(0xFFFFFF);
            } else if (hover) {
                ui.Fill(b, pressed ? t.subtlePressed : t.subtleHover, 0);
            }
            const wchar_t g = btns[i] == CaptionButton::Minimize   ? glyph::Minimize
                              : btns[i] == CaptionButton::Maximize ? (s.maximized ? glyph::Restore : glyph::Maximize)
                                                                   : glyph::Close;
            ui.Icon(b, g, 10.f, fg);
        }
    }

    // ---- Navigation pane
    const Rect pane{client.x, client.y + kTitleH, kPaneW, client.h - kTitleH};
    const Rect navArea = pane.Inset(6.f, 4.f);
    const float itemH = 40.f;
    const int selIndex = static_cast<int>(m.page);
    const float pillY = ui.Anim(ui.MakeId(L"nav-pill"), navArea.y + itemH * static_cast<float>(selIndex) + 12.f, 16.f);
    for (int i = 0; i < static_cast<int>(std::size(kNav)); ++i) {
        const Rect item{navArea.x, navArea.y + itemH * static_cast<float>(i), navArea.w, itemH - 4.f};
        const Interaction it = ui.Interact(ui.MakeId(std::format(L"nav{}", i)), item);
        const bool sel = m.page == kNav[i].page;
        if (sel) ui.Fill(item, it.hovered ? t.subtleHover : WithAlpha(t.subtleHover, t.subtleHover.a * 0.9f), 4.f);
        else if (it.hovered) ui.Fill(item, it.held ? t.subtlePressed : t.subtleHover, 4.f);
        if (it.clicked) m.page = kNav[i].page;
        ui.Icon({item.x + 12.f, item.y, 20.f, item.h}, kNav[i].icon, 16.f, t.text);
        ui.Text({item.x + 48.f, item.y, item.w - 100.f, item.h}, kNav[i].title, ui.Body(14.f));

        std::wstring badge;
        Color bfg = t.textSecondary, bbg = t.neutralBadge;
        if (kNav[i].page == Page::Bridge && m.bridgeRunning) {
            badge = m.bridgeStatus.state == BridgeState::Playing ? L"on" : L"…";
            bfg = t.dark ? Rgb(0x000000) : Rgb(0xFFFFFF);
            bbg = m.bridgeStatus.state == BridgeState::Playing ? t.success : t.caution;
        }
        if (kNav[i].page == Page::Plugins && !m.plugins.plugins.empty()) {
            // Number of plug-ins; red when one failed or crashed.
            bool bad = false;
            for (const auto& e : m.plugins.plugins) {
                const BridgePluginStatus* st = m.PluginStatus(e.id);
                bad |= e.quarantined || (st && (st->state == PluginRunState::Failed || st->state == PluginRunState::Crashed ||
                                                st->state == PluginRunState::Quarantined));
            }
            badge = bad ? L"!" : std::to_wstring(m.plugins.plugins.size());
            if (bad) {
                bfg = t.dark ? Rgb(0x000000) : Rgb(0xFFFFFF);
                bbg = t.critical;
            } else if (!m.plugins.enabled) {
                bfg = t.textTertiary;
            }
        }
        if (!badge.empty()) {
            const float w = std::max(22.f, ui.BadgeWidth(badge) - 4.f);
            ui.Badge({item.R() - w - 10.f, item.Cy() - 10.f, w, 20.f}, badge, bfg, bbg);
        }
    }
    ui.Fill({navArea.x + 1.f, pillY, 3.f, 16.f}, t.accent, 1.5f);

    // ---- Content layer
    const Rect content{client.x + kPaneW, client.y + kTitleH, client.w - kPaneW, client.h - kTitleH};
    const Rect layer{content.x, content.y, content.w + 16.f, content.h + 16.f};
    ui.Fill(layer, t.layer, 8.f);
    ui.Stroke(layer, t.layerStroke, 8.f);
    ui.PushClip(content);
    ui.PushId(static_cast<uint64_t>(m.page));
    switch (m.page) {
        case Page::Bridge: pages::Bridge(ui, content, m); break;
        case Page::Filter: pages::Filter(ui, content, m); break;
        case Page::Tone: pages::Tone(ui, content, m); break;
        case Page::Plugins: pages::Plugins(ui, content, m); break;
        case Page::Devices: pages::Devices(ui, content, m); break;
        case Page::Log: pages::Log(ui, content, m); break;
        default: break;
    }
    ui.PopId();
    ui.PopClip();

    // ---- Toast
    if (!m.toast.empty() && m.now < m.toastUntil) {
        const std::wstring text = m.toast;
        ui.Overlay([&ui, text, content] {
            const auto& th = ui.T();
            TextStyle st = ui.Body(13.f);
            const float w = std::min(content.w - 40.f, ui.Measure(text, st).x + 56.f);
            const Rect box{content.Cx() - w * 0.5f, content.B() - 72.f, w, 44.f};
            ui.Shadow(box, 8.f, 12.f);
            ui.Fill(box, th.popup, 8.f);
            ui.Stroke(box, th.popupStroke, 8.f);
            ui.Icon({box.x + 12.f, box.y, 20.f, box.h}, glyph::Info, 16.f, th.accentText);
            ui.Text({box.x + 42.f, box.y, box.w - 50.f, box.h}, text, st);
        });
        ui.RequestFrame();
    }
}

}  // namespace dgmod::app
