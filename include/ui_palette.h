#pragma once

#include <windows.h>
#include <objbase.h>
#include <propidl.h>
#include <gdiplus.h>
#include <algorithm>

namespace quickdns {

struct ColorPalette {
    COLORREF bg;
    COLORREF cardBg;
    COLORREF cardBorder;
    COLORREF textPrimary;
    COLORREF textSecondary;
    COLORREF textMuted;
    COLORREF accentBlue;
    COLORREF accentGreen;
    COLORREF accentAmber;
    COLORREF accentRed;
    COLORREF separator;
    COLORREF btnPrimaryBg;
    COLORREF btnPrimaryHover;
    COLORREF btnPrimaryPress;
    COLORREF btnPrimaryBorder;
    COLORREF btnPrimaryText;
    COLORREF btnBenchBg;
    COLORREF btnBenchHover;
    COLORREF btnBenchPress;
    COLORREF btnBenchBorder;
    COLORREF btnBenchText;
    COLORREF btnUtilBg;
    COLORREF btnUtilHover;
    COLORREF btnUtilPress;
    COLORREF btnUtilBorder;
    COLORREF btnUtilText;
    COLORREF graphBg;
    COLORREF graphBorder;
    COLORREF graphGrid;
    COLORREF graphLine;
    COLORREF graphText;
};

inline ColorPalette GetPalette(bool isDark) {
    if (isDark) {
        return ColorPalette{
            RGB(22, 25, 31),     // bg (refined dark charcoal)
            RGB(30, 34, 43),     // cardBg
            RGB(48, 55, 68),     // cardBorder
            RGB(241, 245, 249),  // textPrimary
            RGB(156, 163, 175),  // textSecondary
            RGB(110, 118, 132),  // textMuted
            RGB(92, 130, 165),   // accentBlue (muted steel)
            RGB(72, 150, 112),   // accentGreen (calm sage)
            RGB(185, 142, 60),   // accentAmber (muted warm amber)
            RGB(192, 82, 82),    // accentRed (muted brick)
            RGB(48, 55, 68),     // separator
            RGB(44, 76, 112),    // btnPrimaryBg (calm slate navy)
            RGB(54, 92, 134),    // btnPrimaryHover
            RGB(36, 62, 92),     // btnPrimaryPress
            RGB(62, 102, 146),   // btnPrimaryBorder
            RGB(255, 255, 255),  // btnPrimaryText
            RGB(50, 64, 94),     // btnBenchBg (subdued steel indigo)
            RGB(62, 79, 115),    // btnBenchHover
            RGB(40, 52, 78),     // btnBenchPress
            RGB(72, 92, 132),    // btnBenchBorder
            RGB(255, 255, 255),  // btnBenchText
            RGB(34, 38, 48),     // btnUtilBg (neutral dark slate)
            RGB(46, 52, 65),     // btnUtilHover
            RGB(28, 31, 40),     // btnUtilPress
            RGB(58, 66, 82),     // btnUtilBorder
            RGB(226, 232, 240),  // btnUtilText
            RGB(25, 29, 37),     // graphBg
            RGB(48, 55, 68),     // graphBorder
            RGB(38, 44, 56),     // graphGrid
            RGB(88, 132, 174),   // graphLine (neutral steel blue)
            RGB(140, 148, 162)   // graphText
        };
    } else {
        return ColorPalette{
            RGB(246, 248, 251),  // bg (soft clean neutral)
            RGB(255, 255, 255),  // cardBg
            RGB(218, 224, 233),  // cardBorder
            RGB(30, 41, 59),     // textPrimary
            RGB(85, 95, 110),    // textSecondary
            RGB(120, 130, 145),  // textMuted
            RGB(52, 98, 150),    // accentBlue
            RGB(46, 128, 78),    // accentGreen
            RGB(165, 108, 24),   // accentAmber
            RGB(180, 52, 52),    // accentRed
            RGB(222, 226, 233),  // separator
            RGB(48, 88, 132),    // btnPrimaryBg (neutral slate navy)
            RGB(40, 74, 112),    // btnPrimaryHover
            RGB(32, 60, 92),     // btnPrimaryPress
            RGB(40, 74, 112),    // btnPrimaryBorder
            RGB(255, 255, 255),  // btnPrimaryText
            RGB(62, 80, 114),    // btnBenchBg (neutral steel)
            RGB(52, 67, 96),     // btnBenchHover
            RGB(42, 54, 78),     // btnBenchPress
            RGB(52, 67, 96),     // btnBenchBorder
            RGB(255, 255, 255),  // btnBenchText
            RGB(243, 245, 249),  // btnUtilBg (light neutral)
            RGB(232, 236, 242),  // btnUtilHover
            RGB(220, 225, 233),  // btnUtilPress
            RGB(206, 213, 224),  // btnUtilBorder
            RGB(44, 52, 65),     // btnUtilText
            RGB(250, 251, 253),  // graphBg
            RGB(218, 224, 233),  // graphBorder
            RGB(235, 239, 245),  // graphGrid
            RGB(62, 108, 158),   // graphLine
            RGB(110, 120, 135)   // graphText
        };
    }
}

inline COLORREF LerpColor(COLORREF c1, COLORREF c2, float t) {
    if (t <= 0.0f) return c1;
    if (t >= 1.0f) return c2;
    int r = static_cast<int>(GetRValue(c1) + (GetRValue(c2) - GetRValue(c1)) * t);
    int g = static_cast<int>(GetGValue(c1) + (GetGValue(c2) - GetGValue(c1)) * t);
    int b = static_cast<int>(GetBValue(c1) + (GetBValue(c2) - GetBValue(c1)) * t);
    return RGB(std::clamp(r, 0, 255), std::clamp(g, 0, 255), std::clamp(b, 0, 255));
}

inline void AddRoundedRect(Gdiplus::GraphicsPath& path, const Gdiplus::RectF& rect, float radius) {
    float diameter = radius * 2.0f;
    path.AddArc(rect.X, rect.Y, diameter, diameter, 180.0f, 90.0f);
    path.AddArc(rect.GetRight() - diameter, rect.Y, diameter, diameter, 270.0f, 90.0f);
    path.AddArc(rect.GetRight() - diameter, rect.GetBottom() - diameter, diameter, diameter, 0.0f, 90.0f);
    path.AddArc(rect.X, rect.GetBottom() - diameter, diameter, diameter, 90.0f, 90.0f);
    path.CloseFigure();
}

} // namespace quickdns
