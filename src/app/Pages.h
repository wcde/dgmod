#pragma once

#include "app/Model.h"
#include "ui/Ui.h"

#include <string>
#include <string_view>

namespace dgmod::app {

inline constexpr wchar_t kProductTitle[] = L"dgmod";

}  // namespace dgmod::app

namespace dgmod::app::pages {

using ui::Rect;
using ui::Ui;

inline constexpr float kPadX = 32.f;
inline constexpr float kPadTop = 20.f;
inline constexpr float kGap = 12.f;

void Bridge(Ui& ui, Rect area, AppModel& m);
void Filter(Ui& ui, Rect area, AppModel& m);
void Tone(Ui& ui, Rect area, AppModel& m);
void Plugins(Ui& ui, Rect area, AppModel& m);
void Devices(Ui& ui, Rect area, AppModel& m);
void Log(Ui& ui, Rect area, AppModel& m);

// Shared building blocks.
// Draws the page title/subtitle and returns the rect on the right available for header actions.
Rect PageHeader(Ui& ui, Rect& r, std::wstring_view title, std::wstring_view subtitle);
void SectionTitle(Ui& ui, Rect& r, std::wstring_view title);
void StatTile(Ui& ui, Rect r, std::wstring_view label, std::wstring_view value, std::wstring_view sub,
              ui::Color valueColor, wchar_t icon = 0);
void EmptyState(Ui& ui, Rect r, wchar_t icon, std::wstring_view title, std::wstring_view text);
// InfoBar with an optional action button; returns true when the action was clicked. Advances `r`.
bool Notice(Ui& ui, Rect& r, ui::Severity severity, std::wstring_view title, std::wstring_view message,
            std::wstring_view action = {}, bool actionEnabled = true);
// Label on the left, control area on the right (returned). Advances `r`.
Rect SettingRow(Ui& ui, Rect& r, std::wstring_view label, std::wstring_view description, float controlWidth,
                float height = 64.f);

// Formatting.
std::wstring RateText(uint32_t hz);        // "44.1 kHz", "192 kHz"
std::wstring FormatText(const StreamFormat& f);  // "24-bit · 192 kHz"
std::wstring DbfsText(float linear);       // "-3.2 dBFS", "−∞"
std::wstring CountText(uint64_t v);        // "1 234 567"
std::wstring TimeText(uint64_t fileTime);  // "12:34:56.789"

}  // namespace dgmod::app::pages
