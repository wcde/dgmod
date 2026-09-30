#pragma once

#include "ui/Theme.h"

#include <d2d1_1.h>
#include <dwrite.h>
#include "common/Win.h"

#include <cfloat>
#include <cstdint>
#include <functional>
#include <map>
#include <span>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

namespace dgmod::ui {



struct Point {
    float x = 0, y = 0;
};

struct Rect {
    float x = 0, y = 0, w = 0, h = 0;
    [[nodiscard]] float R() const { return x + w; }
    [[nodiscard]] float B() const { return y + h; }
    [[nodiscard]] float Cx() const { return x + w * 0.5f; }
    [[nodiscard]] float Cy() const { return y + h * 0.5f; }
    [[nodiscard]] bool Contains(Point p) const { return p.x >= x && p.x < x + w && p.y >= y && p.y < y + h; }
    [[nodiscard]] Rect Inset(float d) const { return {x + d, y + d, w - 2 * d, h - 2 * d}; }
    [[nodiscard]] Rect Inset(float dx, float dy) const { return {x + dx, y + dy, w - 2 * dx, h - 2 * dy}; }
    [[nodiscard]] Rect Intersect(const Rect& o) const;
    [[nodiscard]] bool Empty() const { return w <= 0 || h <= 0; }
    [[nodiscard]] D2D1_RECT_F D2D() const { return D2D1::RectF(x, y, x + w, y + h); }
};

// Layout helpers: cut a strip from a rect, shrinking the original.
Rect CutTop(Rect& r, float h, float gap = 0);
Rect CutBottom(Rect& r, float h, float gap = 0);
Rect CutLeft(Rect& r, float w, float gap = 0);
Rect CutRight(Rect& r, float w, float gap = 0);
std::vector<Rect> Columns(Rect r, int n, float gap);
std::vector<Rect> ColumnsW(Rect r, std::initializer_list<float> weights, float gap);

enum class HAlign : uint8_t { Left, Center, Right };
enum class VAlign : uint8_t { Top, Center, Bottom };

struct TextStyle {
    float size = 14.f;
    int weight = 400;
    Color color{};
    HAlign h = HAlign::Left;
    VAlign v = VAlign::Center;
    bool wrap = false;
    bool mono = false;
    bool icon = false;
};

struct Input {
    Point mouse{-1000, -1000};
    bool inside = false;
    bool down = false;
    bool pressed = false;
    bool released = false;
    bool doubleClick = false;
    bool rightReleased = false;
    float wheel = 0;
    bool ctrl = false, shift = false;
    std::vector<unsigned> keys;
    void EndFrame() {
        pressed = released = doubleClick = rightReleased = false;
        wheel = 0;
        keys.clear();
    }
    [[nodiscard]] bool KeyPressed(unsigned vk) const {
        for (unsigned k : keys)
            if (k == vk) return true;
        return false;
    }
};

using Id = uint64_t;

struct Interaction {
    bool hovered = false;
    bool held = false;
    bool clicked = false;
    bool doubleClicked = false;
    bool rightClicked = false;
};

struct ScrollState {
    float offset = 0;
    float target = 0;
    float content = 0;
    bool dragging = false;
    float dragStartMouse = 0;
    float dragStartOffset = 0;
};

enum class ButtonKind : uint8_t { Standard, Accent, Subtle };

struct TableColumn {
    std::wstring title;
    float width = 100;  // fixed width in DIPs; 0 = flexible
    float flex = 0;     // weight for flexible columns
    HAlign align = HAlign::Left;
    float minWidth = 0; // flexible columns: minimum width (0 = 110 DIP per flex unit)
    int hidePriority = 0;  // > 0: column may be hidden when the table is narrow (higher hides first)
};

struct TableCell {
    std::wstring text;
    const Color* color = nullptr;   // text color override
    float bar = -1;                 // 0..1 draws a magnitude bar behind the text
    const Color* barColor = nullptr;
    const wchar_t* icon = nullptr;  // optional leading icon glyph
    const Color* iconColor = nullptr;
};

struct TableState {
    int sortColumn = -1;
    bool sortDescending = true;
    int selected = -1;     // row index in display order
    int scrollToRow = -1;  // one-shot request: scroll so this row is visible
    ScrollState scroll;
};

struct TableResult {
    bool sortChanged = false;
    int activated = -1;  // row double-clicked or Enter pressed
    int context = -1;    // row right-clicked (already selected)
};

struct MenuItem {
    std::wstring text;
    wchar_t icon = 0;
    int command = 0;
    std::wstring shortcut;
    bool enabled = true;
    bool separator = false;
    static MenuItem Separator() {
        MenuItem m;
        m.separator = true;
        return m;
    }
};

struct ChartSeries {
    std::span<const float> values;
    Color color{};
    std::wstring label;
    bool fill = false;
    bool bars = false;
};

struct ChartThreshold {
    float value = 0;
    Color color{};
    std::wstring label;
};

struct ChartOptions {
    bool logScale = false;
    float minValue = 0;   // for log scale: lower bound (e.g. 1 µs)
    float maxValue = 0;   // 0 = auto
    std::function<std::wstring(float)> format;  // axis label formatter
    std::wstring xLeftLabel;
    std::wstring xRightLabel;
    int capacity = 0;     // fixed number of x slots (right aligned); 0 = values.size()
};

class Ui {
public:
    Ui();

    void Begin(ID2D1DeviceContext* dc, const Theme& theme, Input& input, double timeSec, HWND hwnd = nullptr);
    void End();

    [[nodiscard]] const Theme& T() const { return *theme_; }
    [[nodiscard]] ID2D1DeviceContext* Dc() const { return dc_; }
    [[nodiscard]] Input& In() { return *in_; }
    [[nodiscard]] double Time() const { return time_; }
    [[nodiscard]] float Dt() const { return dt_; }
    [[nodiscard]] HWND Hwnd() const { return hwnd_; }

    // Identity
    [[nodiscard]] Id MakeId(std::wstring_view s) const;
    [[nodiscard]] Id MakeId(uint64_t n) const;
    void PushId(std::wstring_view s);
    void PushId(uint64_t n);
    void PopId();

    // Clipping
    void PushClip(Rect r);
    void PopClip();
    [[nodiscard]] Rect Clip() const { return clips_.empty() ? Rect{-1e6f, -1e6f, 2e6f, 2e6f} : clips_.back(); }

    // Interaction
    [[nodiscard]] bool Hover(Rect r) const;
    Interaction Interact(Id id, Rect r);
    float Anim(Id id, float target, float speed = 14.f);
    void RequestFrame() { wantFrame_ = true; }
    [[nodiscard]] bool WantsFrame() const { return wantFrame_; }
    void SetCursor(LPCWSTR cursor) { cursor_ = cursor; }
    [[nodiscard]] LPCWSTR Cursor() const { return cursor_; }

    // Drawing primitives
    ID2D1SolidColorBrush* Brush(Color c);
    void Fill(Rect r, Color c, float radius = 0);
    void Stroke(Rect r, Color c, float radius = 0, float width = 1);
    void Line(Point a, Point b, Color c, float width = 1);
    void Circle(Point center, float radius, Color c, bool fill = true, float width = 1);
    void Text(Rect r, std::wstring_view s, const TextStyle& st);
    Point Measure(std::wstring_view s, const TextStyle& st, float maxWidth = FLT_MAX);
    void Icon(Rect r, wchar_t glyph, float size, Color c);
    void Polyline(std::span<const D2D1_POINT_2F> pts, Color c, float width = 1.5f);
    void FillArea(std::span<const D2D1_POINT_2F> pts, float baseline, Color top, Color bottom);
    void Shadow(Rect r, float radius, float spread);

    // Deferred topmost drawing (popups, tooltips)
    void Overlay(std::function<void()> fn) { overlays_.push_back(std::move(fn)); }
    void Tooltip(Id id, Rect r, std::wstring text);

    // Text helpers with theme defaults
    TextStyle Body(float size = 14.f) const;
    TextStyle Secondary(float size = 12.f) const;
    TextStyle Strong(float size = 14.f) const;
    TextStyle Title(float size = 28.f) const;

    // Widgets
    bool Button(Id id, Rect r, std::wstring_view label, ButtonKind kind = ButtonKind::Standard, wchar_t icon = 0,
                bool enabled = true);
    bool IconButton(Id id, Rect r, wchar_t icon, std::wstring_view tooltip = {}, bool enabled = true);
    bool Toggle(Id id, Rect r, bool& value, std::wstring_view label = {}, bool enabled = true);
    bool Segmented(Id id, Rect r, std::span<const std::wstring> items, int& selected, bool enabled = true);
    bool Combo(Id id, Rect r, std::span<const std::wstring> items, int& selected, bool enabled = true);
    // Horizontal Fluent slider; `step` > 0 snaps the value. Returns true while the value changes.
    bool Slider(Id id, Rect r, float& value, float minValue, float maxValue, float step = 0.f, bool enabled = true);
    void Card(Rect r, float radius = 8.f);
    void Badge(Rect r, std::wstring_view text, Color fg, Color bg);
    float BadgeWidth(std::wstring_view text, float size = 12.f);
    void SeverityIcon(Rect r, Severity s, float size = 16.f);
    // Returns the rect reserved for an optional action button on the right.
    Rect InfoBar(Rect r, Severity s, std::wstring_view title, std::wstring_view message, float actionWidth = 0);
    float InfoBarHeight(float width, std::wstring_view title, std::wstring_view message, float actionWidth = 0);
    void ProgressRing(Point center, float radius, Color c);
    void Meter(Rect r, float value, Color c);

    // Scrolling
    float BeginScroll(Id id, Rect view, ScrollState& s);  // returns content origin y
    void EndScroll(Id id, Rect view, ScrollState& s, float contentHeight);

    // Table: rows are given in display order. Double click / Enter activates a row, right click requests a menu.
    TableResult Table(Id id, Rect r, std::span<const TableColumn> cols, int rowCount, TableState& state,
                      const std::function<TableCell(int row, int col)>& cell, float rowHeight = 32.f);
    // Tab-separated text of the whole table (header + rows), for "copy table".
    static std::wstring TableText(std::span<const TableColumn> cols, int rowCount,
                                  const std::function<TableCell(int row, int col)>& cell);

    // Context menu, drawn above everything (including modal dialogs). The chosen command is returned by
    // TakeMenuCommand(owner) on the following frame.
    void OpenMenu(Id owner, std::vector<MenuItem> items, Point at);
    int TakeMenuCommand(Id owner);
    [[nodiscard]] bool MenuOpen() const { return menu_.open; }
    [[nodiscard]] bool MenuOpenedThisFrame() const { return menuOpenedThisFrame_; }

    // While blocked (a modal dialog is open), widgets drawn outside overlays ignore input.
    void SetBlocked(bool blocked) { blocked_ = blocked; }
    [[nodiscard]] bool Blocked() const { return blocked_; }

    // Charts
    void Chart(Rect r, std::span<const ChartSeries> series, std::span<const ChartThreshold> thresholds,
               const ChartOptions& opt);
    void HistogramChart(Rect r, std::span<const uint64_t> buckets, float (*bucketLow)(int), Color c,
                        float warnUs, float critUs);

    static void CopyToClipboard(HWND hwnd, std::wstring_view text);

private:
    IDWriteTextFormat* Format(float size, int weight, bool mono, bool icon);
    ComPtr<IDWriteTextLayout> Layout(std::wstring_view s, const TextStyle& st, float w, float h);

    ID2D1DeviceContext* dc_ = nullptr;
    const Theme* theme_ = nullptr;
    Input* in_ = nullptr;
    HWND hwnd_ = nullptr;
    double time_ = 0, lastTime_ = 0;
    float dt_ = 0;
    bool wantFrame_ = false;
    LPCWSTR cursor_ = nullptr;

    std::vector<Id> idStack_;
    std::vector<Rect> clips_;
    std::vector<std::function<void()>> overlays_;
    bool inOverlay_ = false;
    bool blocked_ = false;

    struct Menu {
        Id owner = 0;
        bool open = false;
        Rect rect;
        std::vector<MenuItem> items;
        double openedAt = 0;
        int resultCommand = -1;
        Id resultOwner = 0;
    } menu_;
    bool menuOpenedThisFrame_ = false;
    bool inMenu_ = false;
    void DrawMenu();

    Id hot_ = 0;
    Id active_ = 0;
    std::unordered_map<Id, float> anims_;
    std::unordered_map<Id, bool> animsTouched_;

    // popup (combo dropdown)
    struct Popup {
        Id owner = 0;
        Rect rect;
        bool open = false;
        int resultIndex = -1;
        Id resultOwner = 0;
    } popup_;
    bool swallowClick_ = false;

    // tooltip
    Id tipId_ = 0;
    double tipStart_ = 0;
    bool tipSeen_ = false;

    ComPtr<ID2D1SolidColorBrush> brush_;
    std::map<uint64_t, ComPtr<IDWriteTextFormat>> formats_;
    std::map<uint64_t, ComPtr<IDWriteInlineObject>> ellipses_;
    ID2D1DeviceContext* brushOwner_ = nullptr;
};

// Segoe Fluent Icons glyphs used across the app.
namespace glyph {
inline constexpr wchar_t Home = 0xE80F;
inline constexpr wchar_t Diagnostic = 0xE9D9;
inline constexpr wchar_t Component = 0xE950;
inline constexpr wchar_t Puzzle = 0xEA86;
inline constexpr wchar_t List = 0xE8FD;
inline constexpr wchar_t Speakers = 0xE7F5;
inline constexpr wchar_t Microphone = 0xE720;
inline constexpr wchar_t Audio = 0xE8D6;
inline constexpr wchar_t Equalizer = 0xE9E9;
inline constexpr wchar_t System = 0xE770;
inline constexpr wchar_t Report = 0xE9F9;
inline constexpr wchar_t Play = 0xE768;
inline constexpr wchar_t Stop = 0xE71A;
inline constexpr wchar_t Switch = 0xE8AB;
inline constexpr wchar_t Refresh = 0xE72C;
inline constexpr wchar_t Save = 0xE74E;
inline constexpr wchar_t Copy = 0xE8C8;
inline constexpr wchar_t Shield = 0xEA18;
inline constexpr wchar_t Info = 0xE946;
inline constexpr wchar_t Warning = 0xE7BA;
inline constexpr wchar_t Error = 0xEA39;
inline constexpr wchar_t Check = 0xE73E;
inline constexpr wchar_t CheckCircle = 0xEC61;
inline constexpr wchar_t ChevronDown = 0xE70D;
inline constexpr wchar_t ChevronUp = 0xE70E;
inline constexpr wchar_t Delete = 0xE74D;
inline constexpr wchar_t Minimize = 0xE921;
inline constexpr wchar_t Maximize = 0xE922;
inline constexpr wchar_t Restore = 0xE923;
inline constexpr wchar_t Close = 0xE8BB;
inline constexpr wchar_t Clock = 0xE823;
inline constexpr wchar_t Processing = 0xE9F5;
inline constexpr wchar_t Memory = 0xEEA0;
inline constexpr wchar_t Lightning = 0xE945;
inline constexpr wchar_t Globe = 0xE774;
inline constexpr wchar_t Document = 0xE8A5;
inline constexpr wchar_t Link = 0xE71B;
inline constexpr wchar_t Monitor = 0xE7F4;
inline constexpr wchar_t Download = 0xE896;
}  // namespace glyph

}  // namespace dgmod::ui
