#include "ToolLayer/Editor/ImGuiEditor.h"
#include "ResourceLayer/Assets/AssetIO.h"
#include "ResourceLayer/Scenes/SceneSerializer.h"
#include <filesystem>
#include <iostream>
#include <stdexcept>
#include <imgui/imgui_internal.h>

namespace fs=std::filesystem;
using namespace se;
namespace
{
void Check(bool value,const char* message){if(!value)throw std::runtime_error(message);}
std::string Read(const fs::path& path){std::vector<std::uint8_t> bytes;Check(asset_io::Read(path,bytes),"read fixture");return {bytes.begin(),bytes.end()};}
void Put(const fs::path& path,const std::string& text){Check(asset_io::WriteAtomic(path,{reinterpret_cast<const std::uint8_t*>(text.data()),text.size()}),"write fixture");}
std::string Hash(const std::string& text){return asset_io::Hash({reinterpret_cast<const std::uint8_t*>(text.data()),text.size()});}
}
int wmain(int argc,wchar_t** argv)
{
    try
    {
        Check(argc==2,"usage: EditorRecoveryTests <output>");
        const auto root=fs::absolute(fs::path(argv[1]))/asset_io::NewId(),content=root/"Content";
        fs::create_directories(content);AssetStore assets(content,root/"out"/"Cache");
        World original;const auto entity=original.Create("saved");Check(bool(entity),"create scene");
        std::string error;const auto scenePath=content/"Scenes"/"Main.scene.json";
        fs::create_directories(scenePath.parent_path());
        Check(SaveScene(scenePath,original,error),"save base scene");
        UiTree originalUi;originalUi.Create(UiKind::Label);const auto uiPath=content/"UI"/"Main.ui.json";Put(uiPath,originalUi.SaveJson());
        MaterialDocument baseMaterial;AssetId materialId;std::vector<AssetDiagnostic> diagnostics;
        const auto materialPath=content/"Materials"/"Main.material.json";
        Check(assets.SaveMaterialSource(materialPath,baseMaterial,materialId,diagnostics),"save base material");
        const auto sceneBefore=Read(scenePath),uiBefore=Read(uiPath),materialBefore=Read(materialPath);
        World edited=original;Check(edited.Rename(edited.FindByStableId(original.Get(entity)->stableId),"recovered"),"edit recovery scene");
        std::string sceneText;Check(SerializeScene(edited,sceneText,error),"serialize recovery scene");
        UiTree editedUi=originalUi;editedUi.Create(UiKind::Button);
        MaterialDocument editedMaterial=baseMaterial;editedMaterial.doubleSided=true;std::string materialText;
        Check(SerializeMaterialDocument(editedMaterial,materialText,diagnostics),"serialize recovery material");
        RecoveryGroup group{"group-test","2026-11-04T00:00:00Z",{
            {"scene","scene","Content/Scenes/Main.scene.json",Hash(sceneBefore),sceneText,1},
            {"ui","ui","Content/UI/Main.ui.json",Hash(uiBefore),editedUi.SaveJson(),1},
            {"material","material","Content/Materials/Main.material.json",Hash(materialBefore),materialText,1}}};
        EditorSession session;Check(session.Open(scenePath,true),"open base scene");UiTree ui;
        ImGuiEditor frontend;frontend.SetProjectRoot(root);frontend.SetLayoutPath({});
        Check(frontend.OpenUi(ui,uiPath,true),"open base UI");
        Check(!frontend.RecoverySourceChanged(group),"matching bases accepted");
        Check(frontend.RestoreRecovery(group,session,assets,ui),"restore all editing documents");
        Check(session.Dirty()&&ui.Count()==2&&frontend.CurrentMaterialPreview().has_value(),"three recovered documents remain dirty");
        Check(Read(scenePath)==sceneBefore&&Read(uiPath)==uiBefore&&Read(materialPath)==materialBefore,"restore never overwrites originals");
        const auto sessionDocuments=frontend.SessionDocuments(session);
        Check(sessionDocuments==std::vector<std::string>{"Content/Scenes/Main.scene.json","Content/Materials/Main.material.json","Content/UI/Main.ui.json"},"all active documents use project-relative session paths");
        {ImGuiEditor restored;restored.SetProjectRoot(root);UiTree restoredUi;restored.RestoreSessionDocuments(sessionDocuments,assets,restoredUi);
        Check(restoredUi.SaveJson()==originalUi.SaveJson()&&restored.SessionDocuments(session)==sessionDocuments,"saved UI and material reopen through session restoration");}
        for(const auto& [active,window]:std::vector<std::pair<std::string,const char*>>{
            {"Content/Scenes/Main.scene.json","###EditorUi.Viewport"},
            {"Content/Materials/Main.material.json","###EditorUi.MaterialEditor"},
            {"Content/UI/Main.ui.json","###EditorUi.UiDesigner"}})
        {
            ImGuiEditor restored;restored.SetProjectRoot(root);restored.SetLayoutPath({});UiTree restoredUi;
            // 兼容旧会话的绝对活动路径，写回时仍统一为项目相对路径。
            const auto restoreActive=active.ends_with(".ui.json")?asset_io::Utf8(root/asset_io::FromUtf8(active)):active;
            restored.RestoreSessionDocuments(sessionDocuments,assets,restoredUi,restoreActive);
            const unsigned char* pixels=nullptr;int width=0,height=0;restored.FontAtlasRgba(pixels,width,height);restored.SetFontTexture(1);
            Localization text;EditorInputFrame input;
            const auto draw=[&]{restored.Draw(session,assets,text,input,{},&restoredUi);};draw();draw();
            const auto actualActive=restored.SessionActiveDocument(session);
            if(actualActive!=active)std::cerr<<"active mismatch expected="<<active<<" actual="<<actualActive<<'\n';
            Check(actualActive==active,"restored document context uses its actual relative path");
            auto* previous=ImGui::GetCurrentContext();ImGui::SetCurrentContext(restored.Context());
            const auto focused=ImGui::GetCurrentContext()->NavWindow;
            Check(focused&&focused->RootWindow->ID==ImHashStr(window),"restored document receives actual ImGui focus after its window exists");
            auto* hierarchy=ImGui::FindWindowByID(ImHashStr("###EditorUi.Hierarchy"));Check(hierarchy&&hierarchy->Active,"scene focus target");
            ImGui::FocusWindow(hierarchy);ImGui::SetCurrentContext(previous);draw();
            Check(restored.SessionActiveDocument(session)=="Content/Scenes/Main.scene.json","later user focus changes the active document");
            ImGui::SetCurrentContext(restored.Context());
            Check(ImGui::GetCurrentContext()->NavWindow->RootWindow->ID==hierarchy->ID,"session restoration focuses only once");
            ImGui::SetCurrentContext(previous);
            restored.RestoreSessionDocuments(sessionDocuments,assets,restoredUi,"Content/Materials/Missing.material.json");draw();
            ImGui::SetCurrentContext(restored.Context());
            Check(ImGui::GetCurrentContext()->NavWindow->RootWindow->ID==hierarchy->ID,"invalid active path does not steal existing focus");
            ImGui::SetCurrentContext(previous);
        }
        Check(Read(scenePath)==sceneBefore&&Read(uiPath)==uiBefore&&Read(materialPath)==materialBefore,"restoring active focus never saves editing documents");
        const auto captured=frontend.CaptureRecoveryDocuments(session,&ui);Check(captured.size()==3,"capture complete dirty editing group");
        for(const auto& document:captured)Check(!document.sourceDigest.empty(),"saved source identity retained");
        Put(scenePath,sceneBefore+"\n");Check(frontend.RecoverySourceChanged(group),"external source change detected by digest");
        EditorSession second;Check(second.Open(scenePath,true),"open externally changed source");UiTree secondUi;
        ImGuiEditor secondFrontend;secondFrontend.SetProjectRoot(root);secondFrontend.SetLayoutPath({});
        Check(secondFrontend.OpenUi(secondUi,uiPath,true),"open second base UI");
        Check(!secondFrontend.RestoreRecovery(group,second,assets,secondUi)&&!second.Dirty(),"changed source blocks in-place recovery atomically");
        const auto loadedDigest=second.SourceDigest();Put(scenePath,sceneBefore+"\n\n");
        Check(secondFrontend.CaptureRecoveryDocuments(second,&secondUi).empty(),"clean document has no recovery changes");
        Check(second.Create("after external edit")!=0,"edit after external source change");
        const auto afterExternal=secondFrontend.CaptureRecoveryDocuments(second,&secondUi);
        Check(afterExternal.size()==1&&afterExternal.front().sourceDigest==loadedDigest,"clean frames do not replace the loaded source baseline");
        fs::path copy;Check(secondFrontend.SaveRecoveryCopy(group,copy),"save distinct recovery copy");
        Check(Read(copy/"Content/Scenes/Main.scene.json")==sceneText&&Read(scenePath)==sceneBefore+"\n\n","save copy preserves changed source");
        const auto recovery=EditorSessionStore::ProjectDirectory(root);
        Check(EditorSessionStore::WriteRecoveryGroup(recovery,root,captured,error),"publish captured group");
        frontend.ConfigureRecovery(recovery,false);frontend.FinishRecovery(true);
        Check(EditorSessionStore::ListRecoveryGroups(recovery,root,error).empty(),"normal confirmed discard removes recovery revisions");
        const auto pruneDirectory=recovery/"prune-regression";
        Check(EditorSessionStore::WriteRecoveryGroup(pruneDirectory,root,captured,error),"publish older mixed document revision");
        Check(session.Create("latest unsaved scene")!=0,"newer scene revision");
        const auto latest=frontend.CaptureRecoveryDocuments(session,&ui);
        Check(EditorSessionStore::WriteRecoveryGroup(pruneDirectory,root,latest,error),"publish newer mixed document revision");
        frontend.ConfigureRecovery(pruneDirectory,false);Check(frontend.SaveUi(ui,uiPath),"save only UI from mixed group");
        const unsigned char* prunePixels=nullptr;int pruneWidth=0,pruneHeight=0;
        frontend.FontAtlasRgba(prunePixels,pruneWidth,pruneHeight);frontend.SetFontTexture(1);
        Localization pruneText;EditorInputFrame pruneInput;frontend.Draw(session,assets,pruneText,pruneInput,{},&ui);
        const auto pruned=EditorSessionStore::ListRecoveryGroups(pruneDirectory,root,error);
        Check(pruned.size()==1&&pruned.front().documents.size()==2,"retired document removed without duplicate history republication");
        Check(pruned.front().documents.front().content==latest.front().content,"pruning keeps the newest scene during the debounce window");
        frontend.FinishRecovery(true);
        // 暂缓旧恢复组不关闭当前会话的保护；保存当前文档只撤销当前修订的恢复资格。
        Check(EditorSessionStore::WriteRecoveryGroup(recovery,root,captured,error),"publish deferred fixture");
        const auto oldId=EditorSessionStore::ListRecoveryGroups(recovery,root,error).front().id;
        ImGuiEditor deferred;deferred.SetProjectRoot(root);deferred.ConfigureRecovery(recovery,true);
        EditorSession current;Check(current.Open(scenePath,true),"open deferred base");UiTree currentUi;
        const unsigned char* pixels=nullptr;int width=0,height=0;deferred.FontAtlasRgba(pixels,width,height);deferred.SetFontTexture(1);
        Localization text;EditorInputFrame input;EditorViewport view;
        deferred.Draw(current,assets,text,input,view,&currentUi);
        auto* previous=ImGui::GetCurrentContext();ImGui::SetCurrentContext(deferred.Context());
        auto* modal=ImGui::FindWindowByID(ImHashStr("editor-recovery"));Check(modal,"recovery choices shown");
        ImGui::ActivateItemByID(modal->GetID("###Editor.RecoveryDefer"));ImGui::SetCurrentContext(previous);
        deferred.Draw(current,assets,text,input,view,&currentUi);Check(!deferred.FileDialogActive(),"defer returns to editing");
        Check(current.Create("new work after defer")!=0,"create new edit");deferred.Draw(current,assets,text,input,view,&currentUi);
        auto newer=EditorSessionStore::ListRecoveryGroups(recovery,root,error);Check(newer.size()==2,"new edits still receive an automatic complete recovery group");
        Check(current.Save(scenePath),"save current scene");deferred.Draw(current,assets,text,input,view,&currentUi);
        newer=EditorSessionStore::ListRecoveryGroups(recovery,root,error);Check(newer.size()==1&&newer.front().id==oldId,"saving current work retires its snapshots and preserves explicitly deferred old group");
        deferred.FinishRecovery(true);newer=EditorSessionStore::ListRecoveryGroups(recovery,root,error);
        Check(newer.size()==1&&newer.front().id==oldId,"normal close preserves the deferred choice");
        std::cout<<"EditorRecoveryTests passed (complete editing group, dirty restore, source conflict, save copy and discard).\n";return 0;
    }
    catch(const std::exception& error){std::cerr<<error.what()<<'\n';return 1;}
}
