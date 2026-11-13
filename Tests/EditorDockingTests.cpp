#include "ToolLayer/Editor/ImGuiEditor.h"
#include "ToolLayer/Editor/EditorDockLayout.h"
#include "ToolLayer/Editor/EditorTheme.h"
#include "ToolLayer/Editor/FlarePalette.h"
#include "ResourceLayer/Assets/AssetIO.h"
#include <imgui/imgui_internal.h>
#include <cmath>
#include <fstream>
#include <iostream>
#include <string>
#include <vector>

#define CHECK(condition) do { if(!(condition)){std::cerr<<"FAIL line "<<__LINE__<<": "<<#condition<<'\n';return 1;} } while(false)
namespace fs=std::filesystem;
namespace
{
struct WindowState{ImGuiID dock=0;ImVec2 pos{},size{};};
WindowState Window(const char* id)
{
    const auto* window=ImGui::FindWindowByID(ImHashStr(id));
    return window?WindowState{window->DockId,window->Pos,window->Size}:WindowState{};
}
}
int main(int argc,char** argv)
{
    const auto directory=fs::path(argc>1?argv[1]:"out/validation/editor-docking")/se::asset_io::NewId();
    fs::create_directories(directory);
    const auto ini=directory/"editor-dock.ini";
    se::EditorSession session;se::AssetStore assets(directory,directory/"cache");Localization text;
    se::EditorInputFrame input;input.displaySize={1280,800};input.deltaSeconds=1.0f;
    std::vector<ImGuiID> firstDocks;WindowState firstViewport{};
    {
        se::ImGuiEditor editor;editor.SetDockLayoutPath(ini);
        const unsigned char* pixels=nullptr;int width=0,height=0;editor.FontAtlasRgba(pixels,width,height);editor.SetFontTexture(1);
        for(int frame=0;frame<6;++frame)editor.Draw(session,assets,text,input,{});
        auto* previous=ImGui::GetCurrentContext();ImGui::SetCurrentContext(editor.Context());
        const auto hierarchy=Window("###EditorUi.Hierarchy"),assetsWindow=Window("###EditorUi.Assets");
        const auto inspector=Window("###EditorUi.Inspector"),console=Window("###EditorUi.Console");
        const auto viewport=Window("###EditorUi.Viewport"),toolbar=Window("###EditorUi.Toolbar");
        CHECK(hierarchy.dock&&assetsWindow.dock&&inspector.dock&&console.dock&&viewport.dock);
        CHECK(toolbar.size.y>0&&viewport.pos.y>=toolbar.pos.y+toolbar.size.y-1);
        CHECK(hierarchy.pos.x<hierarchy.pos.x+hierarchy.size.x&&hierarchy.pos.x+hierarchy.size.x<=viewport.pos.x+1);
        CHECK(viewport.pos.x+viewport.size.x<=inspector.pos.x+1);
        CHECK(inspector.pos.y+inspector.size.y<=console.pos.y+1);
        CHECK(std::abs(hierarchy.size.x/1280.0f-0.20f)<0.06f);
        CHECK(std::abs(inspector.size.x/1280.0f-0.24f)<0.06f);
        CHECK(assetsWindow.pos.y>hierarchy.pos.y);
        firstDocks={hierarchy.dock,assetsWindow.dock,inspector.dock,console.dock,viewport.dock};
        firstViewport=viewport;
        ImGui::SetCurrentContext(previous);
    }
    CHECK(fs::exists(ini));
    std::string iniText;{std::ifstream file(ini,std::ios::binary);iniText.assign(std::istreambuf_iterator<char>(file),{});}
    CHECK(!iniText.empty()&&iniText.find("[Docking][Data]")!=std::string::npos&&iniText.size()<=se::EditorDockLayout::MaximumBytes);
    {
        se::ImGuiEditor reopened;reopened.SetDockLayoutPath(ini);
        const unsigned char* pixels=nullptr;int width=0,height=0;reopened.FontAtlasRgba(pixels,width,height);reopened.SetFontTexture(1);
        for(int frame=0;frame<6;++frame)reopened.Draw(session,assets,text,input,{});
        auto* previous=ImGui::GetCurrentContext();ImGui::SetCurrentContext(reopened.Context());
        const auto hierarchy=Window("###EditorUi.Hierarchy"),assetsWindow=Window("###EditorUi.Assets");
        const auto inspector=Window("###EditorUi.Inspector"),console=Window("###EditorUi.Console"),viewport=Window("###EditorUi.Viewport");
        const std::vector<ImGuiID> restoredDocks{hierarchy.dock,assetsWindow.dock,inspector.dock,console.dock,viewport.dock};
        CHECK(firstDocks==restoredDocks);
        CHECK(std::abs(viewport.size.x-firstViewport.size.x)<2.0f&&std::abs(viewport.size.y-firstViewport.size.y)<2.0f);
        ImGui::SetCurrentContext(previous);
    }
    {
        const auto broken=directory/"broken.ini";{std::ofstream file(broken,std::ios::binary);file<<"not a valid docking layout";}
        se::ImGuiEditor fallback;fallback.SetDockLayoutPath(broken);
        const unsigned char* pixels=nullptr;int width=0,height=0;fallback.FontAtlasRgba(pixels,width,height);fallback.SetFontTexture(1);
        for(int frame=0;frame<5;++frame)fallback.Draw(session,assets,text,input,{});
        auto* previous=ImGui::GetCurrentContext();ImGui::SetCurrentContext(fallback.Context());
        CHECK(Window("###EditorUi.Viewport").dock);
        ImGui::SetCurrentContext(previous);
        std::ifstream file(broken,std::ios::binary);std::string brokenText;brokenText.assign(std::istreambuf_iterator<char>(file),{});
        CHECK(brokenText=="not a valid docking layout");
    }
    std::string backup;{std::ifstream file(fs::path(se::EditorDockLayout::BackupPath(directory/"broken.ini")),std::ios::binary);backup.assign(std::istreambuf_iterator<char>(file),{});}
    CHECK(backup=="not a valid docking layout");
    ImGuiStyle style;se::ApplyEditorTheme(style);
    CHECK(style.Colors[ImGuiCol_WindowBg].x==se::flare::Rgba<ImVec4>(se::flare::Background).x);
    CHECK(style.Colors[ImGuiCol_TabSelectedOverline].z==se::flare::Rgba<ImVec4>(se::flare::Silver).z);
    CHECK(style.WindowPadding.x==6&&style.FramePadding.y==3);
    std::string error;CHECK(se::EditorDockLayout::Validate("[Docking][Data]\n",error));
    CHECK(!se::EditorDockLayout::Validate("garbage",error)&&error=="Editor.DockLayoutInvalid");
    std::cout<<"EditorDockingTests passed (3x2 default, ini persistence, corrupt fallback, Unity theme).\n";
    return 0;
}
