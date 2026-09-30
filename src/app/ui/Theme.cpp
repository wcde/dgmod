#include "ui/Theme.h"

#include "common/Win.h"

namespace dgmod::ui {
namespace {

struct AccentPalette {
    Color c[7];  // Light3, Light2, Light1, Base, Dark1, Dark2, Dark3
};

AccentPalette ReadAccentPalette() {
    AccentPalette p{{Rgb(0x99EBFF), Rgb(0x4CC2FF), Rgb(0x0091F8), Rgb(0x0078D4), Rgb(0x005FB8), Rgb(0x004C87),
                     Rgb(0x001A68)}};
    BYTE data[32]{};
    DWORD size = sizeof(data);
    if (::RegGetValueW(HKEY_CURRENT_USER, L"Software\\Microsoft\\Windows\\CurrentVersion\\Explorer\\Accent",
                       L"AccentPalette", RRF_RT_REG_BINARY, nullptr, data, &size) == ERROR_SUCCESS &&
        size >= 28) {
        for (int i = 0; i < 7; ++i)
            p.c[i] = Color{data[i * 4] / 255.f, data[i * 4 + 1] / 255.f, data[i * 4 + 2] / 255.f, 1.f};
    }
    return p;
}

}  // namespace

Theme Theme::Load() {
    // Always dark, no Mica: an opaque neutral graphite background with the Fluent translucent control tokens on top.
    Theme t;
    t.dark = true;
    t.mica = false;
    const auto pal = ReadAccentPalette();

    t.accent = pal.c[1];
    t.accentHover = WithAlpha(pal.c[1], 0.9f);
    t.accentPressed = WithAlpha(pal.c[1], 0.8f);
    t.onAccent = Rgb(0x000000);
    t.accentText = pal.c[0];

    t.windowBg = Rgb(0x1C1C1C);
    t.layer = Rgb(0x232323);
    t.layerStroke = Rgb(0x000000, 0.22f);
    t.card = Rgb(0xFFFFFF, 0.0512f);
    t.cardHover = Rgb(0xFFFFFF, 0.0837f);
    t.cardStroke = Rgb(0x000000, 0.18f);
    t.control = Rgb(0xFFFFFF, 0.0605f);
    t.controlHover = Rgb(0xFFFFFF, 0.0837f);
    t.controlPressed = Rgb(0xFFFFFF, 0.0326f);
    t.controlStroke = Rgb(0xFFFFFF, 0.0698f);
    t.controlStrokeBottom = Rgb(0xFFFFFF, 0.093f);
    t.subtleHover = Rgb(0xFFFFFF, 0.0605f);
    t.subtlePressed = Rgb(0xFFFFFF, 0.0419f);
    t.divider = Rgb(0xFFFFFF, 0.0837f);
    t.neutralBadge = Rgb(0xFFFFFF, 0.08f);
    t.popup = Rgb(0x2C2C2C);
    t.popupStroke = Rgb(0x000000, 0.20f);
    t.shadow = Rgb(0x000000, 0.30f);

    t.text = Rgb(0xFFFFFF);
    t.textSecondary = Rgb(0xFFFFFF, 0.786f);
    t.textTertiary = Rgb(0xFFFFFF, 0.544f);
    t.textDisabled = Rgb(0xFFFFFF, 0.363f);

    t.success = Rgb(0x6CCB5F);
    t.successBg = Rgb(0x393D1B);
    t.caution = Rgb(0xFCE100);
    t.cautionBg = Rgb(0x433519);
    t.critical = Rgb(0xFF99A4);
    t.criticalBg = Rgb(0x442726);
    t.info = pal.c[0];
    t.infoBg = Rgb(0xFFFFFF, 0.0326f);

    t.chartGrid = Rgb(0xFFFFFF, 0.07f);
    t.series[0] = pal.c[1];
    t.series[1] = Rgb(0xFFA657);
    t.series[2] = Rgb(0xC5A3FF);
    t.series[3] = Rgb(0x6CCB5F);
    t.series[4] = Rgb(0xFF7AB2);
    t.series[5] = Rgb(0xFCE100);
    return t;
}

Color Theme::SeverityColor(Severity s) const {
    switch (s) {
        case Severity::Ok: return success;
        case Severity::Info: return info;
        case Severity::Warning: return caution;
        case Severity::Critical: return critical;
    }
    return text;
}

Color Theme::SeverityBg(Severity s) const {
    switch (s) {
        case Severity::Ok: return successBg;
        case Severity::Info: return infoBg;
        case Severity::Warning: return cautionBg;
        case Severity::Critical: return criticalBg;
    }
    return card;
}

}  // namespace dgmod::ui
