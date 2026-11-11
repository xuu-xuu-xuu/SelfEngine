#pragma once

#include "EditorLayout.h"
#include <array>
#include <string>
#include <string_view>

namespace se
{
// 面板稳定身份与默认显隐的唯一来源。窗口 ID 不随语言变化。
struct EditorPanelDesc
{
    EditorPanel id=EditorPanel::Hierarchy;
    const char* stableKey="hierarchy";
    const char* windowId="###EditorUi.Hierarchy";
    bool defaultVisible=true;
};
inline constexpr std::size_t EditorPanelDescCount=EditorPanelCount;
const std::array<EditorPanelDesc,EditorPanelDescCount>& EditorPanels();
const EditorPanelDesc* FindEditorPanel(EditorPanel panel);
const EditorPanelDesc* FindEditorPanel(std::string_view stableKey);
// 返回“本地化标题 + 稳定窗口 ID”，供 ImGui::Begin 使用。
std::string EditorPanelWindowName(const EditorPanelDesc& panel,std::string_view localizedTitle);
}
