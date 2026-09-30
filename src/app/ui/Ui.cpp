#include "ui/Ui.h"

#include "ui/Renderer.h"

#include <algorithm>
#include <cmath>

namespace dgmod::ui {
namespace {

constexpr uint64_t kFnvBasis = 1469598103934665603ull;
constexpr uint64_t kFnvPrime = 1099511628211ull;

uint64_t Fnv(const void* data, size_t len, uint64_t seed) {
    uint64_t h = seed;
    const auto* p = static_cast<const uint8_t*>(data);
    for (size_t i = 0; i < len; ++i) {
        h ^= p[i];
        h *= kFnvPrime;
    }
    return h;
}

struct FontNames {
    std::wstring text = L"Segoe UI";
    std::wstring display = L"Segoe UI";
    std::wstring icons = L"Segoe MDL2 Assets";
    std::wstring mono = L"Consolas";
};

const FontNames& Fonts() {
    static const FontNames names = [] {
        FontNames n;
        ComPtr<IDWriteFontCollection> fonts;
        if (auto* dw = Factories::Get().dwrite.Get(); dw && SUCCEEDED(dw->GetSystemFontCollection(&fonts))) {
            auto has = [&](const wchar_t* family) {
                UINT32 index = 0;
                BOOL exists = FALSE;
                return SUCCEEDED(fonts->FindFamilyName(family, &index, &exists)) && exists;
            };
            if (has(L"Segoe UI Variable Text")) n.text = L"Segoe UI Variable Text";
            if (has(L"Segoe UI Variable Display")) n.display = L"Segoe UI Variable Display";
            if (has(L"Segoe Fluent Icons")) n.icons = L"Segoe Fluent Icons";
            if (has(L"Cascadia Mono")) n.mono = L"Cascadia Mono";
        }
        return n;
    }();
    return names;
}

}  // namespace

Rect Rect::Intersect(const Rect& o) const {
    const float x0 = std::max(x, o.x), y0 = std::max(y, o.y);
    const float x1 = std::min(R(), o.R()), y1 = std::min(B(), o.B());
    return {x0, y0, std::max(0.f, x1 - x0), std::max(0.f, y1 - y0)};
}

Rect CutTop(Rect& r, float h, float gap) {
    Rect out{r.x, r.y, r.w, h};
    r.y += h + gap;
    r.h -= h + gap;
    return out;
}
Rect CutBottom(Rect& r, float h, float gap) {
    Rect out{r.x, r.B() - h, r.w, h};
    r.h -= h + gap;
    return out;
}
Rect CutLeft(Rect& r, float w, float gap) {
    Rect out{r.x, r.y, w, r.h};
    r.x += w + gap;
    r.w -= w + gap;
    return out;
}
Rect CutRight(Rect& r, float w, float gap) {
    Rect out{r.R() - w, r.y, w, r.h};
    r.w -= w + gap;
    return out;
}

std::vector<Rect> Columns(Rect r, int n, float gap) {
    std::vector<Rect> out;
    if (n <= 0) return out;
    const float w = (r.w - gap * static_cast<float>(n - 1)) / static_cast<float>(n);
    for (int i = 0; i < n; ++i) out.push_back({r.x + static_cast<float>(i) * (w + gap), r.y, w, r.h});
    return out;
}

std::vector<Rect> ColumnsW(Rect r, std::initializer_list<float> weights, float gap) {
    std::vector<Rect> out;
    float total = 0;
    for (float w : weights) total += w;
    const float avail = r.w - gap * static_cast<float>(weights.size() - 1);
    float x = r.x;
    for (float w : weights) {
        const float cw = avail * w / total;
        out.push_back({x, r.y, cw, r.h});
        x += cw + gap;
    }
    return out;
}

Ui::Ui() = default;

void Ui::Begin(ID2D1DeviceContext* dc, const Theme& theme, Input& input, double timeSec, HWND hwnd) {
    dc_ = dc;
    theme_ = &theme;
    in_ = &input;
    hwnd_ = hwnd;
    dt_ = lastTime_ > 0 ? static_cast<float>(std::clamp(timeSec - lastTime_, 0.0, 0.1)) : 0.016f;
    lastTime_ = timeSec;
    time_ = timeSec;
    wantFrame_ = false;
    cursor_ = IDC_ARROW;
    idStack_.clear();
    clips_.clear();
    overlays_.clear();
    tipSeen_ = false;

    if (brushOwner_ != dc_) {
        brush_.Reset();
        dc_->CreateSolidColorBrush(D2D1::ColorF(0, 0, 0, 1), &brush_);
        brushOwner_ = dc_;
    }

    if (popup_.open && in_->pressed && !popup_.rect.Contains(in_->mouse)) {
        popup_.open = false;
        swallowClick_ = true;
    }
    menuOpenedThisFrame_ = false;
    blocked_ = false;
    if (menu_.open) {
        const bool outside = !menu_.rect.Contains(in_->mouse);
        if (in_->pressed && outside) {
            menu_.open = false;
            swallowClick_ = true;
        } else if (in_->rightReleased && outside) {
            menu_.open = false;  // the right click may open another menu at the new position
        }
        if (in_->KeyPressed(VK_ESCAPE)) {
            menu_.open = false;
            std::erase(in_->keys, static_cast<unsigned>(VK_ESCAPE));
        }
    }
    if (swallowClick_) {
        in_->pressed = false;
        in_->doubleClick = false;
        if (in_->released) {
            in_->released = false;
            swallowClick_ = false;
        }
    }
    if (!in_->down && !in_->released) active_ = 0;
}

void Ui::End() {
    while (!clips_.empty()) PopClip();
    inOverlay_ = true;
    for (size_t i = 0; i < overlays_.size(); ++i) {
        auto fn = std::move(overlays_[i]);
        fn();
    }
    inOverlay_ = false;
    overlays_.clear();
    if (menu_.open) DrawMenu();
    if (in_->released) active_ = 0;
    if (!tipSeen_) tipId_ = 0;
    // State changed by input is only visible in the next frame (immediate mode): always render one more.
    if (in_->pressed || in_->released || in_->doubleClick || in_->rightReleased || in_->wheel != 0 ||
        !in_->keys.empty())
        wantFrame_ = true;
}

Id Ui::MakeId(std::wstring_view s) const {
    return Fnv(s.data(), s.size() * sizeof(wchar_t), idStack_.empty() ? kFnvBasis : idStack_.back());
}

Id Ui::MakeId(uint64_t n) const { return Fnv(&n, sizeof(n), idStack_.empty() ? kFnvBasis : idStack_.back()); }

void Ui::PushId(std::wstring_view s) { idStack_.push_back(MakeId(s)); }
void Ui::PushId(uint64_t n) { idStack_.push_back(MakeId(n)); }
void Ui::PopId() {
    if (!idStack_.empty()) idStack_.pop_back();
}

void Ui::PushClip(Rect r) {
    const Rect c = clips_.empty() ? r : r.Intersect(clips_.back());
    clips_.push_back(c);
    dc_->PushAxisAlignedClip(c.D2D(), D2D1_ANTIALIAS_MODE_ALIASED);
}

void Ui::PopClip() {
    if (clips_.empty()) return;
    clips_.pop_back();
    dc_->PopAxisAlignedClip();
}

bool Ui::Hover(Rect r) const {
    if (!in_->inside) return false;
    const Point p = in_->mouse;
    if (!r.Contains(p)) return false;
    if (!inOverlay_ && !Clip().Contains(p)) return false;
    if (blocked_ && !inOverlay_) return false;
    if (popup_.open && !inOverlay_ && popup_.rect.Contains(p)) return false;
    if (menu_.open && !inMenu_ && menu_.rect.Contains(p)) return false;
    return true;
}

Interaction Ui::Interact(Id id, Rect r) {
    Interaction it;
    const bool over = Hover(r);
    it.hovered = over && (active_ == 0 || active_ == id);
    if (it.hovered && in_->pressed) active_ = id;
    it.held = active_ == id && in_->down;
    it.clicked = in_->released && active_ == id && over;
    it.doubleClicked = it.hovered && in_->doubleClick;
    it.rightClicked = over && in_->rightReleased;
    return it;
}

float Ui::Anim(Id id, float target, float speed) {
    auto [it, inserted] = anims_.try_emplace(id, target);
    if (inserted) return target;
    float& v = it->second;
    const float k = 1.f - std::exp(-speed * dt_);
    v += (target - v) * k;
    if (std::fabs(target - v) < 0.002f) v = target;
    else wantFrame_ = true;
    return v;
}

ID2D1SolidColorBrush* Ui::Brush(Color c) {
    brush_->SetColor(c);
    return brush_.Get();
}

void Ui::Fill(Rect r, Color c, float radius) {
    if (r.Empty() || c.a <= 0.f) return;
    if (radius > 0) dc_->FillRoundedRectangle(D2D1::RoundedRect(r.D2D(), radius, radius), Brush(c));
    else dc_->FillRectangle(r.D2D(), Brush(c));
}

void Ui::Stroke(Rect r, Color c, float radius, float width) {
    if (r.Empty() || c.a <= 0.f) return;
    const Rect s = r.Inset(width * 0.5f);
    if (radius > 0) dc_->DrawRoundedRectangle(D2D1::RoundedRect(s.D2D(), radius, radius), Brush(c), width);
    else dc_->DrawRectangle(s.D2D(), Brush(c), width);
}

void Ui::Line(Point a, Point b, Color c, float width) {
    dc_->DrawLine(D2D1::Point2F(a.x, a.y), D2D1::Point2F(b.x, b.y), Brush(c), width);
}

void Ui::Circle(Point center, float radius, Color c, bool fill, float width) {
    const auto e = D2D1::Ellipse(D2D1::Point2F(center.x, center.y), radius, radius);
    if (fill) dc_->FillEllipse(e, Brush(c));
    else dc_->DrawEllipse(e, Brush(c), width);
}

IDWriteTextFormat* Ui::Format(float size, int weight, bool mono, bool icon) {
    const uint64_t key = static_cast<uint64_t>(size * 100.f) | (static_cast<uint64_t>(weight) << 24) |
                         (static_cast<uint64_t>(mono) << 40) | (static_cast<uint64_t>(icon) << 41);
    if (auto it = formats_.find(key); it != formats_.end()) return it->second.Get();
    const auto& f = Fonts();
    const std::wstring& family = icon ? f.icons : (mono ? f.mono : (size >= 20.f ? f.display : f.text));
    ComPtr<IDWriteTextFormat> fmt;
    Factories::Get().dwrite->CreateTextFormat(family.c_str(), nullptr, static_cast<DWRITE_FONT_WEIGHT>(weight),
                                              DWRITE_FONT_STYLE_NORMAL, DWRITE_FONT_STRETCH_NORMAL, size, L"en-US",
                                              &fmt);
    formats_[key] = fmt;
    return fmt.Get();
}

ComPtr<IDWriteTextLayout> Ui::Layout(std::wstring_view s, const TextStyle& st, float w, float h) {
    ComPtr<IDWriteTextLayout> layout;
    auto* fmt = Format(st.size, st.weight, st.mono, st.icon);
    if (!fmt) return layout;
    Factories::Get().dwrite->CreateTextLayout(s.data(), static_cast<UINT32>(s.size()), fmt, std::max(w, 0.f),
                                              std::max(h, 0.f), &layout);
    if (!layout) return layout;
    layout->SetTextAlignment(st.h == HAlign::Left     ? DWRITE_TEXT_ALIGNMENT_LEADING
                             : st.h == HAlign::Center ? DWRITE_TEXT_ALIGNMENT_CENTER
                                                      : DWRITE_TEXT_ALIGNMENT_TRAILING);
    layout->SetParagraphAlignment(st.v == VAlign::Top      ? DWRITE_PARAGRAPH_ALIGNMENT_NEAR
                                  : st.v == VAlign::Center ? DWRITE_PARAGRAPH_ALIGNMENT_CENTER
                                                           : DWRITE_PARAGRAPH_ALIGNMENT_FAR);
    layout->SetWordWrapping(st.wrap ? DWRITE_WORD_WRAPPING_WRAP : DWRITE_WORD_WRAPPING_NO_WRAP);
    if (!st.wrap && !st.icon) {
        auto& sign = ellipses_[reinterpret_cast<uint64_t>(fmt)];
        if (!sign) Factories::Get().dwrite->CreateEllipsisTrimmingSign(fmt, &sign);
        DWRITE_TRIMMING trim{DWRITE_TRIMMING_GRANULARITY_CHARACTER, 0, 0};
        layout->SetTrimming(&trim, sign.Get());
    }
    return layout;
}

void Ui::Text(Rect r, std::wstring_view s, const TextStyle& st) {
    if (s.empty() || r.w <= 0 || st.color.a <= 0) return;
    auto layout = Layout(s, st, r.w, r.h);
    if (!layout) return;
    dc_->DrawTextLayout(D2D1::Point2F(r.x, r.y), layout.Get(), Brush(st.color), D2D1_DRAW_TEXT_OPTIONS_ENABLE_COLOR_FONT);
}

Point Ui::Measure(std::wstring_view s, const TextStyle& st, float maxWidth) {
    if (s.empty()) return {0, st.size * 1.33f};
    TextStyle m = st;
    m.v = VAlign::Top;
    m.h = HAlign::Left;
    auto layout = Layout(s, m, maxWidth >= FLT_MAX ? 100000.f : maxWidth, 100000.f);
    if (!layout) return {};
    DWRITE_TEXT_METRICS tm{};
    layout->GetMetrics(&tm);
    return {tm.widthIncludingTrailingWhitespace, tm.height};
}

void Ui::Icon(Rect r, wchar_t g, float size, Color c) {
    const wchar_t s[2] = {g, 0};
    TextStyle st;
    st.size = size;
    st.icon = true;
    st.color = c;
    st.h = HAlign::Center;
    st.v = VAlign::Center;
    Text(r, std::wstring_view(s, 1), st);
}

void Ui::Polyline(std::span<const D2D1_POINT_2F> pts, Color c, float width) {
    if (pts.size() < 2) return;
    ComPtr<ID2D1PathGeometry> geo;
    auto* f = Factories::Get().d2d.Get();
    if (FAILED(f->CreatePathGeometry(&geo))) return;
    ComPtr<ID2D1GeometrySink> sink;
    geo->Open(&sink);
    sink->BeginFigure(pts[0], D2D1_FIGURE_BEGIN_HOLLOW);
    sink->AddLines(pts.data() + 1, static_cast<UINT32>(pts.size() - 1));
    sink->EndFigure(D2D1_FIGURE_END_OPEN);
    sink->Close();
    static ComPtr<ID2D1StrokeStyle> round;
    if (!round) {
        const auto props = D2D1::StrokeStyleProperties(D2D1_CAP_STYLE_ROUND, D2D1_CAP_STYLE_ROUND,
                                                       D2D1_CAP_STYLE_ROUND, D2D1_LINE_JOIN_ROUND);
        f->CreateStrokeStyle(props, nullptr, 0, &round);
    }
    dc_->DrawGeometry(geo.Get(), Brush(c), width, round.Get());
}

void Ui::FillArea(std::span<const D2D1_POINT_2F> pts, float baseline, Color top, Color bottom) {
    if (pts.size() < 2) return;
    ComPtr<ID2D1PathGeometry> geo;
    if (FAILED(Factories::Get().d2d->CreatePathGeometry(&geo))) return;
    ComPtr<ID2D1GeometrySink> sink;
    geo->Open(&sink);
    sink->BeginFigure(D2D1::Point2F(pts.front().x, baseline), D2D1_FIGURE_BEGIN_FILLED);
    sink->AddLines(pts.data(), static_cast<UINT32>(pts.size()));
    sink->AddLine(D2D1::Point2F(pts.back().x, baseline));
    sink->EndFigure(D2D1_FIGURE_END_CLOSED);
    sink->Close();
    // `top` is the color farthest from the baseline, so an area hanging below it is shaded the same way.
    float minY = baseline, maxY = baseline;
    for (const auto& p : pts) {
        minY = std::min(minY, p.y);
        maxY = std::max(maxY, p.y);
    }
    const float farY = baseline - minY >= maxY - baseline ? minY : maxY;
    const D2D1_GRADIENT_STOP stops[2] = {{0.f, top}, {1.f, bottom}};
    ComPtr<ID2D1GradientStopCollection> coll;
    if (FAILED(dc_->CreateGradientStopCollection(stops, 2, &coll))) return;
    ComPtr<ID2D1LinearGradientBrush> brush;
    dc_->CreateLinearGradientBrush(
        D2D1::LinearGradientBrushProperties(D2D1::Point2F(0, farY), D2D1::Point2F(0, baseline)), coll.Get(), &brush);
    if (brush) dc_->FillGeometry(geo.Get(), brush.Get());
}

void Ui::Shadow(Rect r, float radius, float spread) {
    const Color base = theme_->shadow;
    const int steps = 6;
    for (int i = steps; i >= 1; --i) {
        const float t = static_cast<float>(i) / steps;
        const float grow = spread * t;
        Fill({r.x - grow, r.y - grow * 0.5f + spread * 0.35f, r.w + 2 * grow, r.h + 2 * grow}, WithAlpha(base, base.a * (1.f - t) * 0.35f),
             radius + grow);
    }
}

void Ui::Tooltip(Id id, Rect r, std::wstring text) {
    if (text.empty() || !Hover(r) || in_->down) return;
    if (tipId_ != id) {
        tipId_ = id;
        tipStart_ = time_;
    }
    tipSeen_ = true;
    if (time_ - tipStart_ < 0.55) {
        wantFrame_ = true;
        return;
    }
    const Point mouse = in_->mouse;
    Overlay([this, text = std::move(text), mouse] {
        TextStyle st = Body(12.f);
        st.wrap = true;
        const Point sz = Measure(text, st, 360.f);
        D2D1_SIZE_F vs = dc_->GetSize();
        Rect box{mouse.x + 12, mouse.y + 22, sz.x + 18, sz.y + 12};
        if (box.R() > vs.width - 4) box.x = vs.width - 4 - box.w;
        if (box.B() > vs.height - 4) box.y = mouse.y - 8 - box.h;
        Shadow(box, 4, 8);
        Fill(box, theme_->popup, 4);
        Stroke(box, theme_->popupStroke, 4);
        Text(box.Inset(9, 6), text, st);
    });
}

TextStyle Ui::Body(float size) const {
    TextStyle s;
    s.size = size;
    s.color = theme_->text;
    return s;
}

TextStyle Ui::Secondary(float size) const {
    TextStyle s;
    s.size = size;
    s.color = theme_->textSecondary;
    return s;
}

TextStyle Ui::Strong(float size) const {
    TextStyle s;
    s.size = size;
    s.weight = 600;
    s.color = theme_->text;
    return s;
}

TextStyle Ui::Title(float size) const {
    TextStyle s;
    s.size = size;
    s.weight = 600;
    s.color = theme_->text;
    return s;
}

void Ui::OpenMenu(Id owner, std::vector<MenuItem> items, Point at) {
    if (items.empty()) return;
    float textW = 0, shortW = 0, h = 8.f;
    const TextStyle st = Body(14.f);
    const TextStyle ss = Secondary(12.f);
    for (const auto& it : items) {
        if (it.separator) {
            h += 9.f;
            continue;
        }
        textW = std::max(textW, Measure(it.text, st).x);
        if (!it.shortcut.empty()) shortW = std::max(shortW, Measure(it.shortcut, ss).x);
        h += 34.f;
    }
    const float w = std::max(200.f, 12.f + 28.f + textW + (shortW > 0 ? 32.f + shortW : 0.f) + 20.f);
    const D2D1_SIZE_F vs = dc_->GetSize();
    Rect r{at.x + 2.f, at.y + 2.f, w, h};
    if (r.R() > vs.width - 6.f) r.x = std::max(6.f, at.x - w - 2.f);
    if (r.B() > vs.height - 6.f) r.y = std::max(6.f, vs.height - 6.f - h);
    menu_.owner = owner;
    menu_.open = true;
    menu_.rect = r;
    menu_.items = std::move(items);
    menu_.openedAt = time_;
    menuOpenedThisFrame_ = true;
    wantFrame_ = true;
}

int Ui::TakeMenuCommand(Id owner) {
    if (menu_.resultOwner != owner || menu_.resultCommand < 0) return -1;
    const int cmd = menu_.resultCommand;
    menu_.resultCommand = -1;
    menu_.resultOwner = 0;
    return cmd;
}

void Ui::DrawMenu() {
    inOverlay_ = inMenu_ = true;
    const auto& t = *theme_;
    const float appear = std::clamp(static_cast<float>((time_ - menu_.openedAt) / 0.12), 0.f, 1.f);
    if (appear < 1.f) wantFrame_ = true;
    Rect r = menu_.rect;
    r.y -= (1.f - appear) * 6.f;
    Shadow(r, 8.f, 14.f * appear);
    Fill(r, WithAlpha(t.popup, t.popup.a * (0.6f + 0.4f * appear)), 8.f);
    Stroke(r, t.popupStroke, 8.f);
    float y = r.y + 4.f;
    const TextStyle ss = [&] {
        TextStyle s = Secondary(12.f);
        s.h = HAlign::Right;
        s.color = t.textTertiary;
        return s;
    }();
    for (size_t i = 0; i < menu_.items.size(); ++i) {
        const auto& it = menu_.items[i];
        if (it.separator) {
            Line({r.x + 1.f, y + 4.5f}, {r.R() - 1.f, y + 4.5f}, t.divider);
            y += 9.f;
            continue;
        }
        const Rect ir{r.x + 4.f, y, r.w - 8.f, 34.f};
        y += 34.f;
        const Interaction in = it.enabled ? Interact(menu_.owner + 0x3E00 + i, ir) : Interaction{};
        if (in.hovered) Fill(ir.Inset(0, 1.f), in.held ? t.subtlePressed : t.subtleHover, 4.f);
        const Color fg = it.enabled ? t.text : t.textDisabled;
        if (it.icon) Icon({ir.x + 8.f, ir.y, 20.f, ir.h}, it.icon, 15.f, fg);
        TextStyle st = Body(14.f);
        st.color = fg;
        Text({ir.x + 40.f, ir.y, ir.w - 52.f, ir.h}, it.text, st);
        if (!it.shortcut.empty()) Text({ir.x, ir.y, ir.w - 12.f, ir.h}, it.shortcut, ss);
        if (in.clicked) {
            menu_.resultOwner = menu_.owner;
            menu_.resultCommand = it.command;
            menu_.open = false;
            wantFrame_ = true;
        }
    }
    inOverlay_ = inMenu_ = false;
}

std::wstring Ui::TableText(std::span<const TableColumn> cols, int rowCount,
                           const std::function<TableCell(int, int)>& cell) {
    std::wstring out;
    for (size_t c = 0; c < cols.size(); ++c) out += (c ? L"\t" : L"") + cols[c].title;
    out += L"\r\n";
    for (int r = 0; r < rowCount; ++r) {
        for (size_t c = 0; c < cols.size(); ++c) out += (c ? L"\t" : L"") + cell(r, static_cast<int>(c)).text;
        out += L"\r\n";
    }
    return out;
}

void Ui::CopyToClipboard(HWND hwnd, std::wstring_view text) {
    if (!::OpenClipboard(hwnd)) return;
    ::EmptyClipboard();
    const size_t bytes = (text.size() + 1) * sizeof(wchar_t);
    if (HGLOBAL mem = ::GlobalAlloc(GMEM_MOVEABLE, bytes)) {
        if (auto* p = static_cast<wchar_t*>(::GlobalLock(mem))) {
            std::copy(text.begin(), text.end(), p);
            p[text.size()] = 0;
            ::GlobalUnlock(mem);
            if (!::SetClipboardData(CF_UNICODETEXT, mem)) ::GlobalFree(mem);
        } else {
            ::GlobalFree(mem);
        }
    }
    ::CloseClipboard();
}

}  // namespace dgmod::ui
