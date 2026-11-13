#pragma once

#include "FlarePalette.h"
#include <Windows.h>
#include <dwmapi.h>

namespace se::flare
{
constexpr COLORREF Native(Color color) { return RGB(color.r,color.g,color.b); }

inline void StyleTitleBar(HWND window)
{
    const BOOL dark = TRUE;
    const auto background = Native(Background);
    const auto text = Native(Heading);
    DwmSetWindowAttribute(window, DWMWA_USE_IMMERSIVE_DARK_MODE, &dark, sizeof(dark));
    DwmSetWindowAttribute(window, DWMWA_CAPTION_COLOR, &background, sizeof(background));
    DwmSetWindowAttribute(window, DWMWA_TEXT_COLOR, &text, sizeof(text));
}
}
