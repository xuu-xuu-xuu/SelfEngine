#pragma once

#include <Windows.h>

namespace se
{
// 只在加载窗口线程绘制。phase 按弧度循环，percent 仅来自启动通道。
void DrawLoadingRibbon(HDC dc, const RECT& bounds, int percent, float phase);
}
