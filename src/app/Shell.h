#pragma once

#include "app/Model.h"
#include "ui/Ui.h"

namespace dgmod::app {

enum class CaptionButton : int { None = 0, Minimize, Maximize, Close };

struct ShellState {
    CaptionButton hover = CaptionButton::None;
    CaptionButton pressed = CaptionButton::None;
    bool maximized = false;
    bool active = true;
    bool drawCaptionButtons = true;
};

// Window chrome: title bar, navigation pane and the page host.
class Shell {
public:
    static constexpr float kTitleH = 48.f;
    static constexpr float kPaneW = 248.f;
    static constexpr float kCaptionW = 46.f;
    static constexpr float kCaptionH = 32.f;

    void Draw(ui::Ui& ui, ui::Rect client, AppModel& m, const ShellState& s);
    // Hit test in DIPs relative to the client area.
    static CaptionButton CaptionHit(float x, float y, float clientW);
    static bool InDragRegion(float x, float y, float clientW);
};

}  // namespace dgmod::app
