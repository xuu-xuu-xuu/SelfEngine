#pragma once
#include "CoreLayer/GuiGeometry.h"
struct ImDrawData;
namespace se
{
// 完整复制到通用数据，返回后不再借用 ImGui 缓冲；只支持 ResetRenderState 回调。
bool CopyImGuiGeometry(const ImDrawData& source,GuiGeometry& output);
}
