#include "app/Pages.h"

#include <algorithm>

namespace dgmod::app::pages {

using namespace ui;

void Log(Ui& ui, Rect area, AppModel& m) {
    Rect r{area.x + kPadX, area.y + kPadTop, area.w - 2 * kPadX, area.h - kPadTop - 20.f};
    const Rect actions = PageHeader(ui, r, L"Log", L"Sessions, device changes and glitches of the bridge");
    Rect a = actions;
    if (ui.Button(ui.MakeId(L"open-folder"), CutRight(a, 150.f, 8.f), L"Open folder", ButtonKind::Standard, glyph::Document))
        m.OpenLogFolder();
    if (ui.Button(ui.MakeId(L"copy"), CutRight(a, 110.f, 8.f), L"Copy", ButtonKind::Standard, glyph::Copy)) {
        Ui::CopyToClipboard(ui.Hwnd(), m.logTail);
        m.ShowToast(L"Copied to the clipboard");
    }

    const Rect body = r;
    ui.Card(body);
    if (m.logTail.empty()) {
        EmptyState(ui, body, glyph::List, L"No log yet", L"The bridge writes its log when it runs.");
        return;
    }
    const Id lid = ui.MakeId(L"logfile");
    static ScrollState scroll;
    const Rect view = body.Inset(4.f);
    TextStyle st = ui.Body(12.f);
    st.mono = true;
    st.wrap = true;
    st.v = VAlign::Top;
    const float h = ui.Measure(m.logTail, st, view.w - 32.f).y + 24.f;
    // The newest lines are at the end: follow them unless the reader scrolled up.
    static float followedHeight = -1.f;
    if (h != followedHeight) {
        const float previousEnd = std::max(0.f, followedHeight - view.h);
        if (followedHeight < 0.f || scroll.target >= previousEnd - 1.f) scroll.offset = scroll.target = std::max(0.f, h - view.h);
        followedHeight = h;
    }
    const float y = ui.BeginScroll(lid, view, scroll);
    ui.Text({view.x + 14.f, y + 10.f, view.w - 32.f, h}, m.logTail, st);
    ui.EndScroll(lid, view, scroll, h);
}

}  // namespace dgmod::app::pages
