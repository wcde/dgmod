#include "ui/Renderer.h"
#include "ui/Ui.h"

#include <algorithm>
#include <cmath>
#include <numbers>

namespace dgmod::ui {

bool Ui::Button(Id id, Rect r, std::wstring_view label, ButtonKind kind, wchar_t icon, bool enabled) {
    const Interaction it = enabled ? Interact(id, r) : Interaction{};
    const auto& t = *theme_;
    Color fill{}, fg{};
    constexpr float radius = 4.f;
    switch (kind) {
        case ButtonKind::Accent:
            fill = !enabled ? t.controlPressed : it.held ? t.accentPressed : it.hovered ? t.accentHover : t.accent;
            fg = !enabled ? t.textDisabled : t.onAccent;
            Fill(r, fill, radius);
            break;
        case ButtonKind::Standard:
            fill = !enabled ? t.controlPressed : it.held ? t.controlPressed : it.hovered ? t.controlHover : t.control;
            fg = !enabled ? t.textDisabled : it.held ? t.textSecondary : t.text;
            Fill(r, fill, radius);
            Stroke(r, t.controlStroke, radius);
            if (enabled && !it.held)
                Line({r.x + radius, r.B() - 0.5f}, {r.R() - radius, r.B() - 0.5f}, t.controlStrokeBottom, 1.f);
            break;
        case ButtonKind::Subtle:
            fill = it.held ? t.subtlePressed : it.hovered ? t.subtleHover : Color{};
            fg = !enabled ? t.textDisabled : t.text;
            Fill(r, fill, radius);
            break;
    }
    TextStyle st = Body(14.f);
    st.color = fg;
    const float iconSize = 16.f;
    if (label.empty()) {
        if (icon) Icon(r, icon, iconSize, fg);
    } else {
        const float tw = Measure(label, st).x;
        const float total = tw + (icon ? iconSize + 8.f : 0.f);
        float x = r.Cx() - total * 0.5f;
        if (icon) {
            Icon({x, r.y, iconSize, r.h}, icon, iconSize, fg);
            x += iconSize + 8.f;
        }
        st.h = HAlign::Left;
        Text({x, r.y, std::min(tw + 2.f, r.R() - x), r.h}, label, st);
    }
    return it.clicked;
}

bool Ui::IconButton(Id id, Rect r, wchar_t icon, std::wstring_view tooltip, bool enabled) {
    const bool clicked = Button(id, r, {}, ButtonKind::Subtle, icon, enabled);
    if (!tooltip.empty()) Tooltip(id, r, std::wstring(tooltip));
    return clicked;
}

bool Ui::Toggle(Id id, Rect r, bool& value, std::wstring_view label, bool enabled) {
    const Interaction it = enabled ? Interact(id, r) : Interaction{};
    const auto& t = *theme_;
    bool changed = false;
    if (it.clicked) {
        value = !value;
        changed = true;
    }
    const float pos = Anim(id, value ? 1.f : 0.f, 18.f);
    const Rect track{r.x, r.Cy() - 10.f, 40.f, 20.f};
    if (value) {
        Fill(track, !enabled ? t.textDisabled : it.hovered ? t.accentHover : t.accent, 10.f);
    } else {
        Fill(track, it.hovered ? t.controlHover : t.control, 10.f);
        Stroke(track, !enabled ? t.textDisabled : t.textSecondary, 10.f);
    }
    const float knob = (it.held ? 17.f : it.hovered ? 14.f : 12.f) * (0.85f + 0.15f * pos);
    const float kx = track.x + 10.f + pos * 20.f;
    Circle({kx, track.Cy()}, knob * 0.5f, value ? t.onAccent : (!enabled ? t.textDisabled : t.textSecondary));
    if (!label.empty()) {
        TextStyle st = Body(14.f);
        if (!enabled) st.color = t.textDisabled;
        Text({track.R() + 12.f, r.y, r.R() - track.R() - 12.f, r.h}, label, st);
    }
    return changed;
}

bool Ui::Slider(Id id, Rect r, float& value, float minValue, float maxValue, float step, bool enabled) {
    const Interaction it = enabled ? Interact(id, r) : Interaction{};
    const auto& t = *theme_;
    const Rect track{r.x + 10.f, r.Cy() - 2.f, std::max(0.f, r.w - 20.f), 4.f};
    bool changed = false;
    if (enabled && it.held && track.w > 0.f && maxValue > minValue) {
        float v = minValue + std::clamp((in_->mouse.x - track.x) / track.w, 0.f, 1.f) * (maxValue - minValue);
        if (step > 0.f) v = minValue + std::round((v - minValue) / step) * step;
        v = std::clamp(v, minValue, maxValue);
        if (v != value) {
            value = v;
            changed = true;
        }
    }
    const float frac = maxValue > minValue ? std::clamp((value - minValue) / (maxValue - minValue), 0.f, 1.f) : 0.f;
    Fill(track, WithAlpha(t.text, 0.36f), 2.f);
    Fill({track.x, track.y, track.w * frac, track.h}, enabled ? t.accent : t.textDisabled, 2.f);
    const Point c{track.x + track.w * frac, track.Cy()};
    Circle(c, 10.f, Rgb(0x454545));
    Circle(c, 10.f, t.controlStroke, false, 1.f);
    const float inner = Anim(id, it.held ? 5.f : it.hovered ? 7.f : 6.f, 20.f);
    Circle(c, inner, enabled ? (it.hovered || it.held ? t.accentHover : t.accent) : t.textDisabled);
    if (it.hovered || it.held) SetCursor(IDC_HAND);
    return changed;
}

bool Ui::Segmented(Id id, Rect r, std::span<const std::wstring> items, int& selected, bool enabled) {
    if (items.empty()) return false;
    const auto& t = *theme_;
    Fill(r, t.control, 6.f);
    Stroke(r, t.controlStroke, 6.f);
    const float w = r.w / static_cast<float>(items.size());
    const int sel = std::clamp(selected, 0, static_cast<int>(items.size()) - 1);
    const float x = Anim(id, r.x + w * static_cast<float>(sel), 16.f);
    const Rect pill{x + 3.f, r.y + 3.f, w - 6.f, r.h - 6.f};
    Fill(pill, t.dark ? Rgb(0xFFFFFF, 0.09f) : Rgb(0xFFFFFF, 1.f), 4.f);
    Stroke(pill, t.controlStroke, 4.f);
    Fill({pill.Cx() - 8.f, pill.B() - 3.f, 16.f, 3.f}, enabled ? t.accent : t.textDisabled, 1.5f);
    bool changed = false;
    for (size_t i = 0; i < items.size(); ++i) {
        const Rect seg{r.x + w * static_cast<float>(i), r.y, w, r.h};
        const Interaction it = enabled ? Interact(id + i + 1, seg) : Interaction{};
        if (it.hovered && static_cast<int>(i) != sel) Fill(seg.Inset(3.f), t.subtleHover, 4.f);
        if (it.clicked && static_cast<int>(i) != selected) {
            selected = static_cast<int>(i);
            changed = true;
        }
        TextStyle st = static_cast<int>(i) == sel ? Strong(13.f) : Body(13.f);
        st.h = HAlign::Center;
        if (!enabled) st.color = t.textDisabled;
        else if (static_cast<int>(i) != sel) st.color = t.textSecondary;
        Text(seg.Inset(6.f, 0), items[i], st);
    }
    return changed;
}

bool Ui::Combo(Id id, Rect r, std::span<const std::wstring> items, int& selected, bool enabled) {
    const auto& t = *theme_;
    bool changed = false;
    if (popup_.resultOwner == id && popup_.resultIndex >= 0) {
        if (popup_.resultIndex != selected) changed = true;
        selected = popup_.resultIndex;
        popup_.resultOwner = 0;
        popup_.resultIndex = -1;
    }
    const bool open = popup_.open && popup_.owner == id;
    const Interaction it = enabled ? Interact(id, r) : Interaction{};
    Fill(r, it.held ? t.controlPressed : it.hovered ? t.controlHover : t.control, 4.f);
    Stroke(r, t.controlStroke, 4.f);
    if (enabled) Line({r.x + 4.f, r.B() - 0.5f}, {r.R() - 4.f, r.B() - 0.5f}, t.controlStrokeBottom);
    TextStyle st = Body(14.f);
    if (!enabled) st.color = t.textDisabled;
    const std::wstring_view current =
        selected >= 0 && selected < static_cast<int>(items.size()) ? std::wstring_view(items[selected]) : L"—";
    Text({r.x + 11.f, r.y, r.w - 44.f, r.h}, current, st);
    Icon({r.R() - 34.f, r.y, 24.f, r.h}, glyph::ChevronDown, 12.f, enabled ? t.textSecondary : t.textDisabled);

    if (it.clicked && !items.empty()) {
        if (open) {
            popup_.open = false;
        } else {
            const float itemH = 34.f;
            const float h = std::min(static_cast<float>(items.size()) * itemH + 8.f, 360.f);
            const D2D1_SIZE_F vs = dc_->GetSize();
            Rect pr{r.x - 4.f, r.B() + 4.f, std::max(r.w + 8.f, 200.f), h};
            if (pr.B() > vs.height - 8.f) pr.y = r.y - 4.f - h;
            if (pr.y < 8.f) pr.y = 8.f;
            if (pr.R() > vs.width - 8.f) pr.x = vs.width - 8.f - pr.w;
            popup_ = Popup{id, pr, true, -1, 0};
        }
    }
    if (popup_.open && popup_.owner == id) {
        std::vector<std::wstring> copy(items.begin(), items.end());
        const Rect pr = popup_.rect;
        const int sel = selected;
        Overlay([this, copy = std::move(copy), pr, sel, id] {
            const auto& th = *theme_;
            Shadow(pr, 8.f, 14.f);
            Fill(pr, th.popup, 8.f);
            Stroke(pr, th.popupStroke, 8.f);
            const float itemH = 34.f;
            PushClip(pr.Inset(1.f));
            for (size_t i = 0; i < copy.size(); ++i) {
                const Rect ir{pr.x + 4.f, pr.y + 4.f + itemH * static_cast<float>(i), pr.w - 8.f, itemH};
                if (ir.y > pr.B()) break;
                const Interaction iit = Interact(id + 1000 + i, ir);
                if (iit.hovered || static_cast<int>(i) == sel)
                    Fill(ir.Inset(0, 1.f), iit.held ? th.subtlePressed : th.subtleHover, 4.f);
                if (static_cast<int>(i) == sel) Fill({ir.x, ir.Cy() - 8.f, 3.f, 16.f}, th.accent, 1.5f);
                Text({ir.x + 12.f, ir.y, ir.w - 20.f, ir.h}, copy[i], Body(14.f));
                if (iit.clicked) {
                    popup_.resultOwner = id;
                    popup_.resultIndex = static_cast<int>(i);
                    popup_.open = false;
                    wantFrame_ = true;
                }
            }
            PopClip();
        });
    }
    return changed;
}

void Ui::Card(Rect r, float radius) {
    Fill(r, theme_->card, radius);
    Stroke(r, theme_->cardStroke, radius);
}

float Ui::BadgeWidth(std::wstring_view text, float size) {
    TextStyle st = Strong(size);
    return Measure(text, st).x + 16.f;
}

void Ui::Badge(Rect r, std::wstring_view text, Color fg, Color bg) {
    Fill(r, bg, r.h * 0.5f);
    TextStyle st = Strong(12.f);
    st.color = fg;
    st.h = HAlign::Center;
    Text(r, text, st);
}

void Ui::SeverityIcon(Rect r, Severity s, float size) {
    const auto& t = *theme_;
    const Point c{r.Cx(), r.Cy()};
    Circle(c, size * 0.5f, t.SeverityColor(s));
    const Color fg = t.dark ? Rgb(0x000000, 0.9f) : Rgb(0xFFFFFF);
    const Rect g{c.x - size * 0.5f, c.y - size * 0.5f, size, size};
    switch (s) {
        case Severity::Ok: Icon(g, glyph::Check, size * 0.6f, fg); break;
        case Severity::Critical: Icon(g, 0xE711, size * 0.55f, fg); break;
        case Severity::Warning: {
            TextStyle st = Strong(size * 0.75f);
            st.weight = 700;
            st.color = fg;
            st.h = HAlign::Center;
            Text(g, L"!", st);
            break;
        }
        case Severity::Info: {
            TextStyle st = Strong(size * 0.75f);
            st.weight = 700;
            st.color = fg;
            st.h = HAlign::Center;
            Text(g, L"i", st);
            break;
        }
    }
}

float Ui::InfoBarHeight(float width, std::wstring_view title, std::wstring_view message, float actionWidth) {
    TextStyle st = Body(14.f);
    st.wrap = true;
    const std::wstring text = std::wstring(title) + L"  " + std::wstring(message);
    const float textW = std::max(50.f, width - 48.f - 16.f - (actionWidth > 0 ? actionWidth + 12.f : 0.f));
    return std::max(48.f, Measure(text, st, textW).y + 28.f);
}

Rect Ui::InfoBar(Rect r, Severity s, std::wstring_view title, std::wstring_view message, float actionWidth) {
    const auto& t = *theme_;
    Fill(r, t.SeverityBg(s), 6.f);
    Stroke(r, t.cardStroke, 6.f);
    SeverityIcon({r.x + 14.f, r.y + 14.f, 20.f, 20.f}, s, 16.f);
    TextStyle st = Body(14.f);
    st.wrap = true;
    st.v = VAlign::Top;
    const float reserve = actionWidth > 0 ? actionWidth + 12.f : 0.f;
    const Rect textR{r.x + 48.f, r.y + 14.f, r.w - 48.f - 16.f - reserve, r.h - 20.f};
    const std::wstring text = std::wstring(title) + L"  " + std::wstring(message);
    if (auto layout = Layout(text, st, textR.w, textR.h)) {
        layout->SetFontWeight(DWRITE_FONT_WEIGHT_SEMI_BOLD, DWRITE_TEXT_RANGE{0, static_cast<UINT32>(title.size())});
        dc_->DrawTextLayout(D2D1::Point2F(textR.x, textR.y), layout.Get(), Brush(t.text));
    }
    return Rect{r.R() - 16.f - actionWidth, r.Cy() - 16.f, actionWidth, 32.f};
}

void Ui::ProgressRing(Point center, float radius, Color c) {
    const float start = static_cast<float>(std::fmod(time_ * 360.0, 360.0));
    const float sweep = 100.f + 60.f * static_cast<float>(std::sin(time_ * 2.0));
    ComPtr<ID2D1PathGeometry> geo;
    if (FAILED(Factories::Get().d2d->CreatePathGeometry(&geo))) return;
    ComPtr<ID2D1GeometrySink> sink;
    geo->Open(&sink);
    auto pt = [&](float deg) {
        const float rad = deg * std::numbers::pi_v<float> / 180.f;
        return D2D1::Point2F(center.x + radius * std::cos(rad), center.y + radius * std::sin(rad));
    };
    sink->BeginFigure(pt(start), D2D1_FIGURE_BEGIN_HOLLOW);
    sink->AddArc(D2D1::ArcSegment(pt(start + sweep), D2D1::SizeF(radius, radius), 0, D2D1_SWEEP_DIRECTION_CLOCKWISE,
                                  sweep > 180 ? D2D1_ARC_SIZE_LARGE : D2D1_ARC_SIZE_SMALL));
    sink->EndFigure(D2D1_FIGURE_END_OPEN);
    sink->Close();
    dc_->DrawGeometry(geo.Get(), Brush(c), 2.5f);
    wantFrame_ = true;
}

void Ui::Meter(Rect r, float value, Color c) {
    Fill(r, theme_->control, r.h * 0.5f);
    const float v = std::clamp(value, 0.f, 1.f);
    if (v > 0) Fill({r.x, r.y, std::max(r.h, r.w * v), r.h}, c, r.h * 0.5f);
}

float Ui::BeginScroll(Id id, Rect view, ScrollState& s) {
    (void)id;
    const float k = 1.f - std::exp(-22.f * dt_);
    s.offset += (s.target - s.offset) * k;
    if (std::fabs(s.target - s.offset) < 0.5f) s.offset = s.target;
    else wantFrame_ = true;
    PushClip(view);
    return view.y - s.offset;
}

void Ui::EndScroll(Id id, Rect view, ScrollState& s, float contentHeight) {
    PopClip();
    s.content = contentHeight;
    const float maxOff = std::max(0.f, contentHeight - view.h);
    if (maxOff > 0 && in_->wheel != 0 && Hover(view)) {
        // Respect the system "lines per notch" setting (~30 DIP per line); WHEEL_PAGESCROLL scrolls a page.
        UINT lines = 3;
        ::SystemParametersInfoW(SPI_GETWHEELSCROLLLINES, 0, &lines, 0);
        const float step = lines == WHEEL_PAGESCROLL ? view.h * 0.9f : 30.f * static_cast<float>(std::max(1u, lines));
        // Accumulate from the current target so fast consecutive notches add up instead of restarting.
        s.target = std::clamp(s.target - in_->wheel * step, 0.f, maxOff);
        in_->wheel = 0;
    }
    s.target = std::clamp(s.target, 0.f, maxOff);
    s.offset = std::clamp(s.offset, 0.f, maxOff);
    // The target may have changed after this frame's BeginScroll: make sure the animation starts right away.
    if (std::fabs(s.target - s.offset) >= 0.5f) wantFrame_ = true;
    if (maxOff <= 0) return;

    const float thumbH = std::max(28.f, view.h * view.h / contentHeight);
    const float travel = view.h - thumbH - 4.f;
    const float thumbY = view.y + 2.f + travel * (s.offset / maxOff);
    const Rect bar{view.R() - 12.f, view.y, 12.f, view.h};
    const Rect thumb{bar.x, thumbY, bar.w, thumbH};
    const Id barId = id ^ 0x5CA11BA7ull;
    const Interaction it = Interact(barId, bar);
    if (it.hovered && in_->pressed) {
        if (thumb.Contains(in_->mouse)) {
            s.dragging = true;
            s.dragStartMouse = in_->mouse.y;
            s.dragStartOffset = s.offset;
        } else {
            s.target = std::clamp(s.target + (in_->mouse.y < thumbY ? -view.h : view.h), 0.f, maxOff);
        }
    }
    if (s.dragging) {
        if (in_->down && travel > 0) {
            s.target = s.offset =
                std::clamp(s.dragStartOffset + (in_->mouse.y - s.dragStartMouse) * maxOff / travel, 0.f, maxOff);
        } else {
            s.dragging = false;
        }
    }
    const float expand = Anim(barId, (it.hovered || s.dragging) ? 1.f : 0.f, 16.f);
    const float w = 3.f + 3.f * expand;
    if (expand > 0.01f) Fill({bar.R() - 10.f, view.y + 1.f, 8.f, view.h - 2.f}, WithAlpha(theme_->popup, 0.6f * expand), 4.f);
    Fill({bar.R() - 3.f - w, thumbY, w, thumbH}, theme_->textTertiary, w * 0.5f);
}

TableResult Ui::Table(Id id, Rect r, std::span<const TableColumn> cols, int rowCount, TableState& state,
                      const std::function<TableCell(int, int)>& cell, float rowHeight) {
    const auto& t = *theme_;
    TableResult result;
    bool& sortChanged = result.sortChanged;
    const float headerH = 34.f;
    Rect header = CutTop(r, headerH);

    // Column widths
    float fixed = 0, flexTotal = 0;
    for (const auto& c : cols) {
        fixed += c.width;
        flexTotal += c.flex;
    }
    // Flexible columns keep a minimum width; when the table is too narrow, fixed columns shrink proportionally
    // (down to 55%) instead of squeezing names and descriptions to nothing.
    const float total = header.w - 12.f;
    float flexMin = 0;
    auto minOf = [](const TableColumn& c) { return c.flex > 0 ? (c.minWidth > 0 ? c.minWidth : 110.f * c.flex) : 0.f; };
    std::vector<bool> hidden(cols.size(), false);
    for (const auto& c : cols) flexMin += minOf(c);
    // 1) hide optional columns (highest priority first) until the rest fits
    while (total - fixed < flexMin) {
        int best = -1;
        for (size_t i = 0; i < cols.size(); ++i)
            if (!hidden[i] && cols[i].hidePriority > 0 && (best < 0 || cols[i].hidePriority > cols[best].hidePriority))
                best = static_cast<int>(i);
        if (best < 0) break;
        hidden[best] = true;
        fixed -= cols[best].width;
        flexTotal -= cols[best].flex;
        flexMin -= minOf(cols[best]);
    }
    // 2) if still too narrow, shrink fixed columns proportionally (down to 55%)
    std::vector<float> widths;
    const float scale = total - fixed >= flexMin || fixed <= 0 ? 1.f : std::max(0.55f, (total - flexMin) / fixed);
    const float flexAvail = std::max(0.f, total - fixed * scale);
    for (size_t i = 0; i < cols.size(); ++i)
        widths.push_back(hidden[i] ? 0.f
                                   : cols[i].width * scale + (flexTotal > 0 ? flexAvail * cols[i].flex / flexTotal : 0.f));

    // Header
    float x = header.x;
    for (size_t i = 0; i < cols.size(); ++i) {
        if (widths[i] <= 0) continue;
        const Rect hc{x, header.y, widths[i], header.h};
        const Interaction it = Interact(id + 7000 + i, hc);
        if (it.hovered) Fill(hc.Inset(1.f, 3.f), t.subtleHover, 4.f);
        if (it.clicked) {
            if (state.sortColumn == static_cast<int>(i)) state.sortDescending = !state.sortDescending;
            else {
                state.sortColumn = static_cast<int>(i);
                state.sortDescending = cols[i].align == HAlign::Right;
            }
            sortChanged = true;
        }
        TextStyle st = Secondary(12.f);
        st.weight = 600;
        st.h = cols[i].align;
        Rect tr = hc.Inset(10.f, 0);
        if (state.sortColumn == static_cast<int>(i)) {
            const Rect ir = cols[i].align == HAlign::Right ? CutLeft(tr, 14.f) : CutRight(tr, 14.f);
            Icon(ir, state.sortDescending ? glyph::ChevronDown : glyph::ChevronUp, 9.f, t.textSecondary);
        }
        Text(tr, cols[i].title, st);
        x += widths[i];
    }
    Line({header.x, header.B() - 0.5f}, {header.R(), header.B() - 0.5f}, t.divider);

    // Keyboard navigation when the table was last clicked.
    static Id focused = 0;
    if (focused == id && rowCount > 0 && !blocked_ && !menu_.open) {
        const int before = state.selected;
        if (in_->KeyPressed(VK_DOWN)) state.selected = std::min(rowCount - 1, state.selected + 1);
        if (in_->KeyPressed(VK_UP)) state.selected = std::max(0, state.selected - 1);
        if (in_->KeyPressed(VK_HOME)) state.selected = 0;
        if (in_->KeyPressed(VK_END)) state.selected = rowCount - 1;
        if (state.selected != before) state.scrollToRow = state.selected;
        if (in_->KeyPressed(VK_RETURN) && state.selected >= 0 && state.selected < rowCount) result.activated = state.selected;
        if (in_->ctrl && in_->KeyPressed('C') && state.selected >= 0 && state.selected < rowCount) {
            std::wstring line;
            for (size_t c = 0; c < cols.size(); ++c) {
                if (c) line += L'\t';
                line += cell(state.selected, static_cast<int>(c)).text;
            }
            CopyToClipboard(hwnd_, line);
        }
    }

    // Body
    if (state.scrollToRow >= 0 && state.scrollToRow < rowCount) {
        const float top = rowHeight * static_cast<float>(state.scrollToRow);
        if (top < state.scroll.target) state.scroll.target = top;
        else if (top + rowHeight > state.scroll.target + r.h) state.scroll.target = top + rowHeight - r.h;
        state.scrollToRow = -1;
    }
    const float originY = BeginScroll(id, r, state.scroll);
    const int first = std::max(0, static_cast<int>((r.y - originY) / rowHeight));
    const int last = std::min(rowCount, static_cast<int>((r.B() - originY) / rowHeight) + 1);
    for (int row = first; row < last; ++row) {
        const Rect rr{r.x, originY + rowHeight * static_cast<float>(row), r.w - 12.f, rowHeight};
        const Interaction it = Interact(id + 100000 + static_cast<uint64_t>(row), rr);
        const bool sel = state.selected == row;
        if (it.clicked || it.rightClicked) {
            state.selected = row;
            focused = id;
        }
        if (it.doubleClicked) result.activated = row;
        if (it.rightClicked) result.context = row;
        if (sel || it.hovered) Fill(rr.Inset(2.f, 1.f), it.held ? t.subtlePressed : t.subtleHover, 4.f);
        if (sel) Fill({rr.x + 2.f, rr.Cy() - 8.f, 3.f, 16.f}, t.accent, 1.5f);
        float cx = rr.x;
        for (size_t c = 0; c < cols.size(); ++c) {
            if (widths[c] <= 0) continue;
            const Rect cr{cx, rr.y, widths[c], rr.h};
            const TableCell tc = cell(row, static_cast<int>(c));
            Rect content = cr.Inset(10.f, 0);
            if (tc.bar >= 0) {
                const float bw = content.w * std::clamp(tc.bar, 0.f, 1.f);
                const Color bc = tc.barColor ? *tc.barColor : t.accent;
                const Rect br = cols[c].align == HAlign::Right ? Rect{content.R() - bw, rr.y + 6.f, bw, rr.h - 12.f}
                                                                : Rect{content.x, rr.y + 6.f, bw, rr.h - 12.f};
                Fill(br, WithAlpha(bc, 0.22f), 3.f);
            }
            if (tc.icon) {
                const Rect ir = CutLeft(content, 18.f, 6.f);
                Icon(ir, *tc.icon, 14.f, tc.iconColor ? *tc.iconColor : t.textSecondary);
            }
            TextStyle st = Body(13.f);
            st.h = cols[c].align;
            if (tc.color) st.color = *tc.color;
            Text(content, tc.text, st);
            if (it.hovered && !tc.text.empty() && Measure(tc.text, st).x > content.w)
                Tooltip(id + 200000 + static_cast<uint64_t>(row) * 64 + c, cr, tc.text);
            cx += widths[c];
        }
    }
    if (rowCount == 0) {
        TextStyle st = Secondary(13.f);
        st.h = HAlign::Center;
        Text({r.x, r.y + 20.f, r.w, 24.f}, L"No data", st);
    }
    EndScroll(id, r, state.scroll, rowHeight * static_cast<float>(rowCount) + 4.f);
    return result;
}

}  // namespace dgmod::ui
