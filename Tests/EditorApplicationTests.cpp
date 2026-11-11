#include "ToolLayer/Editor/EditorApplication.h"
#include "ToolLayer/Editor/EditorSession.h"
#include "ToolLayer/Editor/EditorSessionStore.h"
#include "FunctionLayer/Application/FrameData.h"
#include "ResourceLayer/Assets/AssetIO.h"
#include "ResourceLayer/Assets/AssetStore.h"
#include "FunctionLayer/Rendering/Renderer.h"
#include <imgui/imgui.h>
#include <imgui/imgui_internal.h>
#include <filesystem>
#include <fstream>
#include <iostream>

#define CHECK(condition) do { if (!(condition)) { std::cerr << "FAIL line " << __LINE__ << ": " << #condition << '\n'; return 1; } } while (false)
class RecordingRenderer final:public Renderer
{
public:
    bool initializeSuccess=true,failRender=false,valid=true,shutdown=false;
    int frames=0;
    HWND window=nullptr;
    const ImDrawData* borrowed=nullptr;
    std::filesystem::path expectedAssets;
    bool Initialize(const RenderSurface& surface,const Localization&) override
    {window=surface.window;valid=valid&&IsWindow(window);return initializeSuccess;}
    RenderResult Render(const RenderFrame& frame,const Localization&) override
    {
        ++frames;
        valid=valid&&frame.data&&frame.data->editorDrawData&&frame.data->assets;
        if(frame.data&&frame.data->assets&&!expectedAssets.empty())valid=valid&&frame.data->assets->SourceRoot()==expectedAssets;
        if(frame.data&&frame.data->editorDrawData)
        {borrowed=frame.data->editorDrawData;valid=valid&&borrowed->Valid&&borrowed->TotalVtxCount>0
            &&frame.data->editorFontPixels&&frame.data->editorFontWidth>0&&frame.data->editorFontHeight>0&&frame.data->editorFontToken==1;}
        return failRender?RenderResult::Failed:RenderResult::Success;
    }
    void Shutdown() noexcept override
    {shutdown=true;valid=valid&&IsWindow(window);if(borrowed)valid=valid&&borrowed->Valid&&borrowed->TotalVtxCount>0;}
};
int main(int argc,char** argv)
{
    const auto project=std::filesystem::absolute(argc>1?argv[1]:".cache/validation/editor-host")/se::asset_io::NewId();
    std::filesystem::create_directories(project/"Assets");
    {std::ofstream file(project/"Assets"/"keep.txt");file<<"unchanged";}
    RecordingRenderer renderer;
    CHECK(se::RunEditorApp(renderer,"en-US",project,true,3)==0);
    CHECK(renderer.frames==3&&renderer.valid&&renderer.shutdown&&!IsWindow(renderer.window));
    const auto sessionDirectory=se::EditorSessionStore::ProjectDirectory(project);
    CHECK(std::filesystem::exists(sessionDirectory/"session.json"));
    CHECK(!std::filesystem::exists(sessionDirectory/"process.json"));
    std::ifstream file(project/"Assets"/"keep.txt");std::string value;file>>value;CHECK(value=="unchanged");
    CHECK(std::distance(std::filesystem::directory_iterator(project/"Assets"),std::filesystem::directory_iterator{})==1);
    RecordingRenderer failedInit;failedInit.initializeSuccess=false;
    CHECK(se::RunEditorApp(failedInit,"en-US",project,true,1)==1&&failedInit.shutdown&&failedInit.valid&&failedInit.frames==0);
    RecordingRenderer failedFrame;failedFrame.failRender=true;
    CHECK(se::RunEditorApp(failedFrame,"en-US",project,true,1)==1&&failedFrame.frames==1&&failedFrame.shutdown&&failedFrame.valid);
    RecordingRenderer closeRenderer;se::EditorApplicationHooks closeHooks;
    closeHooks.editorUi="imgui"; // 本夹具检查 ImDrawData 和 ImGui 模态；toolkit 默认入口由托管/窗口回归覆盖。
    bool cancelQueued=false,cancelClosed=false,secondClose=false,discardQueued=false,cancelPreserved=false;unsigned tick=0;
    closeHooks.initialize=[](se::EditorSession& editor,se::UiTree& ui){
        // 关闭保护必须同时覆盖场景与 UI 文档的脏状态；仅场景脏会漏掉 UI 分支。
        return editor.Create("unsaved close fixture")!=0&&ui.Create(se::UiKind::Button,0)!=0;
    };
    closeHooks.frame=[&](se::EditorSession& editor,double){
        ++tick;if(tick==2)PostMessageW(closeRenderer.window,WM_CLOSE,0,0);
        if(tick>=5&&cancelClosed&&!secondClose){cancelPreserved=editor.Dirty()&&editor.EditWorld().Size()==1;secondClose=true;PostMessageW(closeRenderer.window,WM_CLOSE,0,0);}return true;
    };
    closeHooks.drawn=[&](ImGuiContext* context,const se::EditorFrameResult&){
        auto* previous=ImGui::GetCurrentContext();ImGui::SetCurrentContext(context);
        auto* modal=ImGui::FindWindowByID(ImHashStr("###EditorUi.UnsavedDocuments"));
        if(cancelQueued&&modal&&!modal->Active)cancelClosed=true;
        if(modal&&modal->Active&&!modal->Hidden)
        {
            if(!secondClose){ImGui::FocusWindow(modal);ImGui::ActivateItemByID(modal->GetID("###EditorUi.Cancel"));cancelQueued=true;}
            else{ImGui::FocusWindow(modal);ImGui::ActivateItemByID(modal->GetID("###EditorUi.DiscardChanges"));discardQueued=true;}
        }
        ImGui::SetCurrentContext(previous);
    };
    CHECK(se::RunEditorApp(closeRenderer,"en-US",project,true,30,&closeHooks)==0);
    std::cout<<"close guard: cancel="<<cancelQueued<<" closed="<<cancelClosed<<" second="<<secondClose<<" discard="<<discardQueued<<" preserved="<<cancelPreserved<<" frames="<<closeRenderer.frames<<'\n';
    CHECK(cancelQueued&&secondClose&&discardQueued&&cancelPreserved&&closeRenderer.frames<30&&closeRenderer.valid&&closeRenderer.shutdown);
    CHECK(!std::filesystem::exists(project/"Resources/Scenes/Untitled.scene.json"));
    const auto custom=project/"custom-content-project";
    std::filesystem::create_directories(custom/"Art");
    std::filesystem::create_directories(custom/"Content"); // 旧启发式会错误选中此目录。
    {std::ofstream manifest(custom/"project.json");manifest<<R"({"format":"SelfEngine.Project","version":1,"name":"Custom","contentRoot":"Art","entryScene":"Main.scene.json","scenes":["Main.scene.json"],"ui":[],"assets":[]})";}
    RecordingRenderer customRenderer;customRenderer.expectedAssets=std::filesystem::weakly_canonical(custom/"Art");
    CHECK(se::RunEditorApp(customRenderer,"en-US",custom,true,2)==0&&customRenderer.valid&&customRenderer.frames==2);
    {std::ofstream manifest(custom/"project.json");manifest<<"invalid";}
    RecordingRenderer invalidManifest;
    CHECK(se::RunEditorApp(invalidManifest,"en-US",custom,true,2)==1&&invalidManifest.frames==0);
    std::cout<<"EditorApplicationTests passed (finite hidden host, CPU draw data, shutdown lifetime, read-only startup, failures).\n";
    return 0;
}
