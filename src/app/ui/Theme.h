#pragma once

#include <cstdint>

namespace dgmod::ui {
enum class Severity : uint8_t { Ok, Info, Warning, Critical };
}

#include <d2d1_1.h>

#include <cstdint>

namespace dgmod::ui {

using Color = D2D1_COLOR_F;

constexpr Color Rgb(uint32_t rgb, float a = 1.f) {
    return Color{static_cast<float>((rgb >> 16) & 0xFF) / 255.f, static_cast<float>((rgb >> 8) & 0xFF) / 255.f,
                 static_cast<float>(rgb & 0xFF) / 255.f, a};
}
constexpr Color WithAlpha(Color c, float a) { return Color{c.r, c.g, c.b, a}; }
constexpr Color Mix(Color a, Color b, float t) {
    return Color{a.r + (b.r - a.r) * t, a.g + (b.g - a.g) * t, a.b + (b.b - a.b) * t, a.a + (b.a - a.a) * t};
}

// Windows 11 (Fluent) inspired dark palette on an opaque graphite background.
struct Theme {
    bool dark = true;
    bool mica = true;  // backdrop is provided by DWM; otherwise windowBg is painted

    Color accent{};         // accent fill for buttons, selection
    Color accentHover{};
    Color accentPressed{};
    Color onAccent{};       // text on accent fill
    Color accentText{};     // accent used as text/icon color

    Color windowBg{};       // solid fallback when Mica is unavailable
    Color layer{};          // content layer over Mica
    Color layerStroke{};
    Color card{};
    Color cardHover{};
    Color cardStroke{};
    Color control{};
    Color controlHover{};
    Color controlPressed{};
    Color controlStroke{};
    Color controlStrokeBottom{};
    Color subtleHover{};
    Color subtlePressed{};
    Color divider{};
    Color neutralBadge{};
    Color popup{};
    Color popupStroke{};
    Color shadow{};

    Color text{};
    Color textSecondary{};
    Color textTertiary{};
    Color textDisabled{};

    Color success{}, successBg{};
    Color caution{}, cautionBg{};
    Color critical{}, criticalBg{};
    Color info{}, infoBg{};

    Color chartGrid{};
    Color series[6]{};

    [[nodiscard]] Color SeverityColor(Severity s) const;
    [[nodiscard]] Color SeverityBg(Severity s) const;

    static Theme Load();
};

}  // namespace dgmod::ui
