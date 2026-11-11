#include "ToolLayer/Editor/ImGuiEditor.h"
#include "ResourceLayer/Assets/AssetIO.h"
#include "ResourceLayer/Scenes/SceneSerializer.h"
#include <imgui/imgui_internal.h>
#include <nlohmann/json.hpp>
#include <Windows.h>
#include <algorithm>
#include <iostream>
#include <stdexcept>

namespace fs=std::filesystem;
using namespace se;
namespace
{
void Check(bool value,const char* message){if(!value)throw std::runtime_error(message);}
void Put(const fs::path& path,const std::string& text){Check(asset_io::WriteAtomic(path,{reinterpret_cast<const std::uint8_t*>(text.data()),text.size()}),"write fixture");}
std::string Read(const fs::path& path){std::vector<std::uint8_t> bytes;Check(asset_io::Read(path,bytes),"read fixture");return {bytes.begin(),bytes.end()};}
struct ReadLock
{
    HANDLE handle;
    explicit ReadLock(const fs::path& path):handle(CreateFileW(path.c_str(),GENERIC_READ,FILE_SHARE_READ,nullptr,OPEN_EXISTING,FILE_ATTRIBUTE_NORMAL,nullptr)){Check(handle!=INVALID_HANDLE_VALUE,"lock fixture");}
    ~ReadLock(){CloseHandle(handle);}
};
}
int wmain(int argc,wchar_t** argv)
{
    try
    {
        Check(argc==2,"usage: EditorSettingsUiTests <output>");
        const auto root=fs::absolute(argv[1])/asset_io::NewId();fs::create_directories(root/"Content");
        fs::create_directories(root/"Scenes");World base;std::string error;
        const auto scene=root/"Scenes/Main.scene.json",project=root/"project.json",build=root/"build.json";
        Check(SaveScene(scene,base,error),"save scene");
        auto projectJson=nlohmann::json::parse(R"({"format":"SelfEngine.Project","version":1,"name":"Original","contentRoot":"Content","entryScene":"Scenes/Main.scene.json","scenes":["Scenes/Main.scene.json"],"ui":[],"assets":[],"extension":{"keep":true}})");
        projectJson["name"]=std::string(200,'N');Put(project,projectJson.dump());
        const std::string originalBuild=R"({"format":"SelfEngine.Build","version":1,"scriptsProject":"","scriptType":"","bindings":"","entryUi":"","extension":{"keep":true}})";
        Put(build,originalBuild);UiTree savedUi;savedUi.Create(UiKind::Label);Put(root/"UI/Main.ui.json",savedUi.SaveJson());
        EditorSession editor;Check(editor.Open(scene,true),"open clean scene");AssetStore assets(root/"Content",root/"out/Cache");
        ImGuiEditor frontend;frontend.SetProjectRoot(root);frontend.SetDocumentPaths(scene,{});
        EditorLayout layout;layout.SetVisible(EditorPanel::ProjectSettings,true);layout.SetVisible(EditorPanel::BuildExport,true);
        const auto layoutPath=root/"out/layout.json";Check(layout.Save(layoutPath,error),"settings layout");frontend.SetLayoutPath(layoutPath);
        const unsigned char* pixels=nullptr;int width=0,height=0;frontend.FontAtlasRgba(pixels,width,height);frontend.SetFontTexture(1);
        Localization text;EditorInputFrame input;EditorViewport viewport;
        const auto frame=[&]{frontend.Draw(editor,assets,text,input,viewport,nullptr);};frame();frame();
        Check(frontend.CaptureRecoveryDocuments(editor,nullptr).empty(),"opening a field longer than its display buffer never truncates or dirties the document");
        const auto projectBeforeFocus=Read(project),buildBeforeFocus=Read(build);
        for(const auto& [active,windowName]:std::vector<std::pair<std::string,const char*>>{
            {"project.json","###EditorUi.ProjectSettings"},{"build.json","###EditorUi.BuildExport"}})
        {
            auto* previous=ImGui::GetCurrentContext();ImGui::SetCurrentContext(frontend.Context());
            auto* window=ImGui::FindWindowByID(ImHashStr(windowName));Check(window&&window->Active,"settings focus source exists");
            ImGui::FocusWindow(window);ImGui::SetCurrentContext(previous);frame();
            Check(frontend.SessionActiveDocument(editor)==active,"settings focus is captured as its document path");
            const auto documents=frontend.SessionDocuments(editor);
            Check(std::find(documents.begin(),documents.end(),"project.json")!=documents.end()
                &&std::find(documents.begin(),documents.end(),"build.json")!=documents.end(),"opened settings join the session document list");
            ImGuiEditor restored;restored.SetProjectRoot(root);restored.SetDocumentPaths(scene,{});restored.SetLayoutPath(layoutPath);UiTree restoredUi;
            restored.RestoreSessionDocuments(documents,assets,restoredUi,active);
            const unsigned char* restoredPixels=nullptr;int restoredWidth=0,restoredHeight=0;
            restored.FontAtlasRgba(restoredPixels,restoredWidth,restoredHeight);restored.SetFontTexture(1);
            restored.Draw(editor,assets,text,input,viewport,&restoredUi);restored.Draw(editor,assets,text,input,viewport,&restoredUi);
            Check(restored.SessionActiveDocument(editor)==active&&restored.CaptureRecoveryDocuments(editor,&restoredUi).empty(),"restored settings context stays clean");
            ImGui::SetCurrentContext(restored.Context());const auto focused=ImGui::GetCurrentContext()->NavWindow;
            Check(focused&&focused->RootWindow->ID==ImHashStr(windowName),"restored settings panel receives actual keyboard focus");
            ImGui::SetCurrentContext(previous);
        }
        Check(Read(project)==projectBeforeFocus&&Read(build)==buildBeforeFocus,"settings focus restoration never rewrites source data");
        const auto click=[&](const char* name,const char* key){
            auto* previous=ImGui::GetCurrentContext();ImGui::SetCurrentContext(frontend.Context());
            auto* window=ImGui::FindWindowByID(ImHashStr(name));Check(window&&window->Active,"target UI window exists");
            ImGui::FocusWindow(window);ImGui::ActivateItemByID(window->GetID((std::string("###")+key).c_str()));
            ImGui::SetCurrentContext(previous);frame();
        };
        const auto type=[&](const char* name,const char* key,const char* value){
            auto* previous=ImGui::GetCurrentContext();ImGui::SetCurrentContext(frontend.Context());
            auto* window=ImGui::FindWindowByID(ImHashStr(name));Check(window&&window->Active,"settings field window exists");
            const auto id=window->GetID((std::string("###")+key).c_str());ImGui::FocusWindow(window);ImGui::ActivateItemByID(id);
            ImGui::GetCurrentContext()->NavNextActivateFlags=ImGuiActivateFlags_PreferInput;ImGui::SetCurrentContext(previous);frame();
            ImGui::SetCurrentContext(frontend.Context());auto* state=ImGui::GetInputTextState(id);Check(state&&ImGui::GetCurrentContext()->ActiveId==id,"actual settings InputText is focused");
            state->SelectAll();ImGui::GetIO().AddInputCharactersUTF8(value);ImGui::SetCurrentContext(previous);frame();
            ImGui::SetCurrentContext(frontend.Context());state=ImGui::GetInputTextState(id);Check(state&&std::string_view(state->TextA.Data,state->TextLen)==value,"settings text entered");
            ImGui::ClearActiveID();ImGui::SetCurrentContext(previous);frame();
        };
        type("###EditorUi.ProjectSettings","EditorUi.Name","Changed");
        type("###EditorUi.BuildExport","EditorUi.EntryUi","UI/Main.ui.json");
        auto recovery=frontend.CaptureRecoveryDocuments(editor,nullptr);Check(recovery.size()==2,"both settings buffers are recoverable dirty documents");
        frontend.RequestClose();frame();Check(!frontend.CloseApproved()&&frontend.FileDialogActive(),"dirty settings block immediate close");
        {
            ReadLock locked(build);click("###EditorUi.UnsavedDocuments","EditorUi.SaveAndClose");
            Check(!frontend.CloseApproved(),"failed build settings save cannot approve close");
            Check(nlohmann::json::parse(Read(project))["name"]=="Changed"&&Read(build)==originalBuild,"partial save matches real files");
            recovery=frontend.CaptureRecoveryDocuments(editor,nullptr);Check(recovery.size()==1&&recovery.front().kind=="build-settings","only failed settings remain dirty");
            click("###EditorUi.UnsavedDocuments","EditorUi.Cancel");
        }
        frame();Check(frontend.ExecuteCommand("editor.save-all").state==EditorCommandResult::Succeeded,"retry saves unlocked settings");
        Check(nlohmann::json::parse(Read(build))["entryUi"]=="UI/Main.ui.json"&&nlohmann::json::parse(Read(build))["extension"]["keep"]==true,"save preserves settings and unknown extensions");
        Check(frontend.CaptureRecoveryDocuments(editor,nullptr).empty(),"saved settings are clean");
        type("###EditorUi.ProjectSettings","EditorUi.Name","Draft name");
        click("###EditorUi.ProjectSettings","EditorUi.Reload");frame();Check(frontend.FileDialogActive(),"reload asks before discarding draft");
        click("settings-reload","EditorUi.Cancel");Check(frontend.CaptureRecoveryDocuments(editor,nullptr).size()==1,"cancel reload keeps draft");
        auto external=nlohmann::json::parse(Read(project));external["name"]="External";Put(project,external.dump());const auto externalBytes=Read(project);
        Check(frontend.ExecuteCommand("editor.save-all").state==EditorCommandResult::Failed,"external settings conflict rejects save-all");frame();
        click("###EditorUi.ExternalChangeTitle","EditorUi.SaveAsNew");Check(Read(project)==externalBytes,"save copy never overwrites external settings");
        bool copyFound=false;for(const auto& file:fs::directory_iterator(root))if(file.path().filename().string().starts_with("project-external-"))
        {copyFound=nlohmann::json::parse(Read(file.path()))["name"]=="Draft name";}
        Check(copyFound&&!frontend.CaptureRecoveryDocuments(editor,nullptr).empty(),"alternate copy preserves draft without claiming the project was saved");
        std::cout<<"EditorSettingsUiTests passed (actual InputText, dirty close protection, partial failure, retry, reload cancel, external conflict and copy).\n";return 0;
    }
    catch(const std::exception& error){std::cerr<<"EditorSettingsUiTests failed: "<<error.what()<<'\n';return 1;}
}
