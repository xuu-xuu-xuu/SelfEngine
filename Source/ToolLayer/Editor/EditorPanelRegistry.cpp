#include "EditorPanelRegistry.h"

namespace se
{
namespace
{
constexpr std::array<EditorPanelDesc,EditorPanelDescCount> Panels{{
    {EditorPanel::Hierarchy,"hierarchy","###EditorUi.Hierarchy",true},
    {EditorPanel::Inspector,"inspector","###EditorUi.Inspector",true},
    {EditorPanel::Assets,"assets","###EditorUi.Assets",true},
    {EditorPanel::MaterialEditor,"material","###EditorUi.MaterialEditor",false},
    {EditorPanel::Viewport,"viewport","###EditorUi.Viewport",true},
    {EditorPanel::UiDesigner,"ui-designer","###EditorUi.UiDesigner",false},
    {EditorPanel::ProjectSettings,"project-settings","###EditorUi.ProjectSettings",false},
    {EditorPanel::BuildExport,"build-export","###EditorUi.BuildExport",false},
    {EditorPanel::Console,"console","###EditorUi.Console",true},
}};
}
const std::array<EditorPanelDesc,EditorPanelDescCount>& EditorPanels(){return Panels;}
const EditorPanelDesc* FindEditorPanel(EditorPanel panel)
{
    const auto index=static_cast<std::size_t>(panel);
    return index<Panels.size()&&Panels[index].id==panel?&Panels[index]:nullptr;
}
const EditorPanelDesc* FindEditorPanel(std::string_view stableKey)
{
    for(const auto& panel:Panels)if(stableKey==panel.stableKey)return &panel;
    return nullptr;
}
std::string EditorPanelWindowName(const EditorPanelDesc& panel,std::string_view localizedTitle)
{
    std::string name(localizedTitle);
    name+=panel.windowId;
    return name;
}
}
