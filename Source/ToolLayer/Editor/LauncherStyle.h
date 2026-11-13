#pragma once

#include "EditorWindowStyle.h"

namespace se::launcher_style
{
// 原生控件保留键盘、辅助功能和通知行为，父窗口负责着色。
LRESULT ControlColors(UINT message, HDC dc, bool muted);
bool DrawControl(const DRAWITEMSTRUCT& item);
void ThemeControl(HWND control);
void Fill(HDC dc, RECT rect, flare::Color color);
}
