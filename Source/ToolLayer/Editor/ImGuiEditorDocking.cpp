#include "ImGuiEditor.h"
#include "EditorDockLayout.h"
#include "EditorPanelRegistry.h"
#include "CoreLayer/Log.h"
#include <imgui/imgui_internal.h>
#include <algorithm>
#include <cstring>

namespace se
{
namespace
{
struct ContextScope
{
    ImGuiContext* previous=ImGui::GetCurrentContext();
    explicit ContextScope(ImGuiContext* next){ImGui::SetCurrentContext(next);}
    ~ContextScope(){ImGui::SetCurrentContext(previous);}
};
constexpr const char* DockSpaceName="SelfEngineDockSpace";
const char* TitleKey(EditorPanel panel)
{
    switch(panel)
    {
    case EditorPanel::Hierarchy:return "EditorUi.Hierarchy";
    case EditorPanel::Inspector:return "EditorUi.Inspector";
    case EditorPanel::Assets:return "EditorUi.Assets";
    case EditorPanel::MaterialEditor:return "EditorUi.MaterialEditor";
    case EditorPanel::Viewport:return "EditorUi.Viewport";
    case EditorPanel::UiDesigner:return "EditorUi.UiDesigner";
    case EditorPanel::ProjectSettings:return "EditorUi.ProjectSettings";
    case EditorPanel::BuildExport:return "EditorUi.BuildExport";
    case EditorPanel::Console:return "EditorUi.Console";
    }
    return "EditorUi.Console";
}
std::string WindowName(EditorPanel panel,const Localization& text)
{
    const auto* desc=FindEditorPanel(panel);
    return desc?EditorPanelWindowName(*desc,text.GetText(TitleKey(panel))):std::string{};
}
}
void ImGuiEditor::SetDockLayoutPath(std::filesystem::path path)
{
    dockLayoutPath_=std::move(path);
    if(dockLayoutPath_.empty()){dockNeedsBuild_=true;return;}
    std::string text,error;
    if(!EditorDockLayout::Load(dockLayoutPath_,text,error))
    {
        LogConsole(1,"Editor.DockLayoutInvalid",error);
        dockNeedsBuild_=true;
        return;
    }
    if(text.empty()){dockNeedsBuild_=true;return;}
    ContextScope context(context_);
    ImGui::LoadIniSettingsFromMemory(text.data(),text.size());
    dockNeedsBuild_=ImGui::DockBuilderGetNode(ImHashStr(DockSpaceName))==nullptr;
}
void ImGuiEditor::BuildDefaultDockLayout(const Localization& text)
{
    const auto display=ImGui::GetIO().DisplaySize;
    const auto root=ImHashStr(DockSpaceName);
    ImGui::DockBuilderRemoveNodeDockedWindows(root,true);
    ImGui::DockBuilderRemoveNode(root);
    ImGui::DockBuilderAddNode(root,ImGuiDockNodeFlags_DockSpace);
    ImGui::DockBuilderSetNodeSize(root,{std::max(1.0f,display.x),std::max(1.0f,display.y)});

    const bool leftVisible=showHierarchy_||showAssets_;
    const bool rightVisible=showInspector_||showConsole_||showMaterial_||showUi_||showProjectSettings_||showBuild_;
    ImGuiID left=0,right=0,center=root;
    if(leftVisible)
    {
        float ratio=0.20f;
        if(!rightVisible&&!showViewport_)ratio=1.0f;
        ImGui::DockBuilderSplitNode(center,ImGuiDir_Left,ratio,&left,&center);
    }
    if(rightVisible)
    {
        const float available=leftVisible?0.80f:1.0f;
        const float ratio=std::clamp(0.24f/available,0.10f,0.90f);
        ImGui::DockBuilderSplitNode(center,ImGuiDir_Right,ratio,&right,&center);
    }
    ImGuiID hierarchy=left,assets=left,inspector=right,console=right;
    if(leftVisible&&(showHierarchy_||showAssets_))
        ImGui::DockBuilderSplitNode(left,ImGuiDir_Down,0.34f,&assets,&hierarchy);
    if(rightVisible&&(showInspector_||showConsole_))
        ImGui::DockBuilderSplitNode(right,ImGuiDir_Down,0.30f,&console,&inspector);

    const auto dock=[&](EditorPanel panel,ImGuiID node){
        if(!node)return;
        const auto name=WindowName(panel,text);
        if(!name.empty())ImGui::DockBuilderDockWindow(name.c_str(),node);
    };
    if(showHierarchy_)dock(EditorPanel::Hierarchy,hierarchy);
    if(showAssets_)dock(EditorPanel::Assets,assets);
    if(showInspector_)dock(EditorPanel::Inspector,inspector);
    if(showConsole_)dock(EditorPanel::Console,console);
    if(showMaterial_)dock(EditorPanel::MaterialEditor,right);
    if(showUi_)dock(EditorPanel::UiDesigner,right);
    if(showProjectSettings_)dock(EditorPanel::ProjectSettings,right);
    if(showBuild_)dock(EditorPanel::BuildExport,right);
    if(showViewport_)dock(EditorPanel::Viewport,center);
    ImGui::DockBuilderFinish(root);
    dockNeedsBuild_=false;
}
void ImGuiEditor::DrawDockSpace(const Localization& text)
{
    auto& io=ImGui::GetIO();
    const auto* toolbar=ImGui::FindWindowByID(ImHashStr("###EditorUi.Toolbar"));
    const float top=toolbar?toolbar->Pos.y+toolbar->Size.y:ImGui::GetFrameHeight();
    const float width=std::max(1.0f,io.DisplaySize.x);
    const float height=std::max(1.0f,io.DisplaySize.y-top);
    ImGui::SetNextWindowPos({0,top},ImGuiCond_Always);
    ImGui::SetNextWindowSize({width,height},ImGuiCond_Always);
    ImGui::SetNextWindowBgAlpha(0.0f);
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding,{0,0});
    ImGui::PushStyleVar(ImGuiStyleVar_WindowBorderSize,0.0f);
    ImGui::Begin("###EditorUi.DockHost",nullptr,
        ImGuiWindowFlags_NoTitleBar|ImGuiWindowFlags_NoCollapse|ImGuiWindowFlags_NoResize|ImGuiWindowFlags_NoMove
        |ImGuiWindowFlags_NoSavedSettings|ImGuiWindowFlags_NoDocking|ImGuiWindowFlags_NoBringToFrontOnFocus|ImGuiWindowFlags_NoNavFocus);
    ImGui::DockSpace(ImHashStr(DockSpaceName),{0,0},ImGuiDockNodeFlags_None);
    ImGui::End();
    ImGui::PopStyleVar(2);
    if(dockNeedsBuild_)BuildDefaultDockLayout(text);

    if(io.WantSaveIniSettings)
    {
        if(!dockDirty_){dockDirty_=true;dockDirtyAt_=ImGui::GetTime();}
        if(dockLayoutPath_.empty())
        {
            std::size_t size=0;ImGui::SaveIniSettingsToMemory(&size);io.WantSaveIniSettings=false;dockDirty_=false;
        }
        else if(!ImGui::IsMouseDown(ImGuiMouseButton_Left)&&ImGui::GetTime()-dockDirtyAt_>=1.0)SaveDockLayout();
    }
}
void ImGuiEditor::SaveDockLayout()
{
    if(dockLayoutPath_.empty())return;
    ContextScope context(context_);
    std::size_t size=0;
    const char* data=ImGui::SaveIniSettingsToMemory(&size);
    if(!data||!size){LogConsole(1,"Editor.DockLayoutSaveFailed");return;}
    std::string error;
    if(!EditorDockLayout::Save(dockLayoutPath_,std::string_view(data,size),error))LogConsole(1,error);
    dockDirty_=false;
}
void ImGuiEditor::ResetDockLayout()
{
    dockNeedsBuild_=true;dockDirty_=true;dockDirtyAt_=ImGui::GetTime();
}
}
