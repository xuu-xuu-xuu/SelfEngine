#pragma once

#include "imgui/imgui.h"

namespace se
{
// Unity Dark 风格基准。它只修改 ImGuiStyle，不接触布局、文档或业务状态。
void ApplyEditorTheme(ImGuiStyle& style);
}
