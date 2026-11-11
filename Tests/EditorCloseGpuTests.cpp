#include "ToolLayer/Editor/EditorApplication.h"
#include "ToolLayer/Editor/ImGuiEditor.h"
#include "FunctionLayer/Rendering/SandboxRenderer.h"
#include "ResourceLayer/Assets/AssetIO.h"
#include "ResourceLayer/Assets/Primitives.h"
#include <imgui/imgui_internal.h>
#include <fstream>
#include <iostream>
#include <stdexcept>

namespace fs=std::filesystem;
using namespace se;
namespace
{
void Check(bool value,const char* reason){if(!value)throw std::runtime_error(reason);}
std::string Read(const fs::path& path){std::ifstream input(path,std::ios::binary);return {std::istreambuf_iterator<char>(input),{}};}
class CloseRenderer final:public Renderer
{
public:
    SandboxRenderer gpu{SandboxScene::Scene};HWND window=nullptr;unsigned frames=0;bool valid=true;
    bool Initialize(const RenderSurface& surface,const Localization& text)override
    {window=surface.window;return gpu.Initialize(surface,text);}
    RenderResult Render(const RenderFrame& frame,const Localization& text)override
    {++frames;return gpu.Render(frame,text);}
    EditorRenderView QueryEditorView()const override{return gpu.QueryEditorView();}
    void Shutdown()noexcept override
    {const auto stats=gpu.Statistics();valid=valid&&stats.debugWarnings==0&&stats.debugErrors==0;gpu.Shutdown();}
};
void Test(const fs::path& root,bool save)
{
    fs::create_directories(root/"Assets");fs::create_directories(root/"Scenes");fs::create_directories(root/"UI");
    const auto scenePath=root/"Scenes/Main.scene.json",uiPath=root/"UI/Main.ui.json";
    AssetStore fixtureAssets(root/"Assets",root/"out/Cache");AssetId materialId;std::vector<AssetDiagnostic> diagnostics;
    const auto materialPath=root/"Assets/Materials/Default.material.json";
    Check(fixtureAssets.SaveMaterialSource(materialPath,MaterialDocument{},materialId,diagnostics),"save fixture material");
    EditorSession original;const auto entity=original.Create("saved entity");
    Check(entity&&original.SetMesh(entity,{BuiltinCubeAssetId,{materialId}})&&original.Save(scenePath),"save fixture scene");
    UiTree originalUi;originalUi.Create(UiKind::Label);{std::ofstream file(uiPath);file<<originalUi.SaveJson();}
    const auto sceneBefore=Read(scenePath),uiBefore=Read(uiPath),materialBefore=Read(materialPath);
    CloseRenderer renderer;EditorApplicationHooks hooks;unsigned tick=0;
    ImGuiEditor* frontend=nullptr;UiTree* ui=nullptr;
    bool createdMaterial=false,cancelQueued=false,cancelled=false,secondClose=false,decisionQueued=false;
    bool preserved=false,closing=false,allDirty=false;
    hooks.initialize=[&](EditorSession& editor,UiTree& tree){ui=&tree;return editor.Open(scenePath,true)&&editor.Create("unsaved entity")!=0;};
    hooks.configure=[&](ImGuiEditor& editor){
        frontend=&editor;editor.SetLayoutPath({});editor.SetAuxiliaryPanels(true);editor.SetDocumentPaths(scenePath,uiPath);
        Check(editor.OpenUi(*ui,uiPath,true)&&editor.EditUi(*ui,[](UiTree& tree){return tree.Create(UiKind::Button)!=0;}),"dirty UI fixture");
    };
    hooks.frame=[&](EditorSession& editor,double){
        ++tick;
        if(tick==8){allDirty=createdMaterial&&frontend->CurrentMaterialPreview().has_value()&&editor.Dirty()&&ui->Count()==2;PostMessageW(renderer.window,WM_CLOSE,0,0);}
        if(cancelled&&!secondClose&&tick>=12){
            preserved=editor.Dirty()&&editor.EditWorld().Size()==2&&ui->Count()==2&&Read(scenePath)==sceneBefore&&Read(uiPath)==uiBefore;
            secondClose=true;PostMessageW(renderer.window,WM_CLOSE,0,0);
        }
        return true;
    };
    hooks.drawn=[&](ImGuiContext* context,const EditorFrameResult&){
        auto* previous=ImGui::GetCurrentContext();ImGui::SetCurrentContext(context);
        auto* material=ImGui::FindWindowByID(ImHashStr("###EditorUi.MaterialEditor"));
        if((tick==2||tick==4)&&material&&material->Active){ImGui::FocusWindow(material);ImGui::ActivateItemByID(material->GetID(tick==2?"###EditorUi.OpenMaterial":"###material.doubleSided"));if(tick==4)createdMaterial=true;}
        auto* modal=ImGui::FindWindowByID(ImHashStr("###EditorUi.UnsavedDocuments"));
        if(cancelQueued&&modal&&!modal->Active)cancelled=true;
        if(modal&&modal->Active&&!modal->Hidden){
            ImGui::FocusWindow(modal);
            if(!secondClose){ImGui::ActivateItemByID(modal->GetID("###EditorUi.Cancel"));cancelQueued=true;}
            else{ImGui::ActivateItemByID(modal->GetID(save?"###EditorUi.SaveAndClose":"###EditorUi.DiscardChanges"));decisionQueued=true;}
        }
        ImGui::SetCurrentContext(previous);
    };
    hooks.closing=[&](EditorSession& editor,UiTree& tree){closing=true;Check(!editor.IsPlaying()&&tree.Count()==2,"clean shutdown keeps edit UI alive");};
    const auto result=RunEditorApp(renderer,"en-US",root,true,50,&hooks);
    Check(result==0,"dirty close returns zero");
    Check(allDirty&&cancelQueued&&cancelled&&secondClose&&decisionQueued&&preserved&&closing,"cancel then confirmed close completes");
    Check(renderer.frames<50&&renderer.valid&&!IsWindow(renderer.window),"GPU and native window finish without warnings");
    if(save){Check(Read(scenePath)!=sceneBefore&&Read(uiPath)!=uiBefore&&Read(materialPath)!=materialBefore,"save-close persists all three documents");}
    else{Check(Read(scenePath)==sceneBefore&&Read(uiPath)==uiBefore&&Read(materialPath)==materialBefore,"discard-close preserves all original bytes");}
    std::cout<<(save?"save":"discard")<<" close: result="<<result<<" frames="<<renderer.frames<<" cancel_preserved=1 gpu_valid=1\n";
}
}
int main(int argc,char** argv)
{
    try{const auto root=fs::absolute(argc>1?argv[1]:"out/validation/editor-close")/asset_io::NewId();Test(root/"discard",false);Test(root/"save",true);std::cout<<"EditorCloseGpuTests passed.\n";return 0;}
    catch(const std::exception& error){std::cerr<<"EditorCloseGpuTests failed: "<<error.what()<<'\n';return 1;}
}
