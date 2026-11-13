#include "ToolLayer/Editor/EditorApplication.h"
#include "ToolLayer/Editor/EditorSession.h"
#include "ToolLayer/Editor/EditorSessionStore.h"
#include "ToolLayer/Editor/EditorUi/EditorShell.h"
#include <nlohmann/json.hpp>
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
void ToolkitImportFlow(const std::filesystem::path& parent,const std::filesystem::path& external)
{
    namespace fs=std::filesystem;
    using se::ui::editor::EditorCommandId;
    const auto require=[](bool ok,const char* message){if(!ok)throw std::runtime_error(message);};
    const auto root=parent/"toolkit-import",content=root/"Art";
    const auto incoming=parent/se::asset_io::FromUtf8("外部模型"),source=incoming/external.filename();
    fs::create_directories(content);fs::create_directories(incoming);
    fs::copy_file(external,source);
    // 参考 FBX 有一张外部贴图；用户额外提供的 FBX 以探针得到的真实依赖为准。
    se::StaticModel original;std::vector<se::AssetDiagnostic> originalDiagnostics;
    require(se::ImportStaticModel(external,{},original,originalDiagnostics),"original FBX parses");
    for(const auto& dependency:original.dependencies)
    {
        const auto relative=se::asset_io::FromUtf8(dependency);
        fs::create_directories((incoming/relative).parent_path());
        fs::copy_file(external.parent_path()/relative,incoming/relative);
    }
    {std::ofstream manifest(root/"project.json");manifest<<R"({"format":"SelfEngine.Project","version":1,"name":"Import","contentRoot":"Art","entryScene":"Main.scene.json","scenes":["Main.scene.json"],"ui":[],"assets":[]})";}
    const auto bad=incoming/"broken.fbx";{std::ofstream file(bad);file<<"invalid FBX";}
    class ToolkitRenderer final:public Renderer
    {
    public:
        HWND window=nullptr;bool valid=true;int frames=0;
        bool Initialize(const RenderSurface& surface,const Localization&) override {window=surface.window;return true;}
        RenderResult Render(const RenderFrame& frame,const Localization&) override
        {++frames;valid=valid&&frame.data&&frame.data->assets&&frame.data->editorGuiEnabled;return RenderResult::Success;}
        void Shutdown() noexcept override {}
    } renderer;
    se::EditorApplicationHooks hooks;hooks.editorUi="toolkit";
    fs::path chosen=source;unsigned picks=0;int stage=0;bool selected=false,playingRejected=false;
    hooks.chooseAssetImportSource=[&](){++picks;return chosen;};
    hooks.toolkitDrawn=[&](se::ui::editor::EditorShell& shell,se::EditorSession& editor){
        const auto status=std::string(shell.Document().Text(shell.StatusBar()));
        const auto import=[&](){return shell.Commands().Execute({EditorCommandId::Import,{}});};
        if(stage==0)
        {
            require(import(),"external FBX import starts");
            require(!import()&&picks==1,"busy import does not reopen picker");
            require(!shell.Commands().Execute({EditorCommandId::Play,{}}),"Play waits for import transaction");
            stage=1;
        }
        else if(stage==1&&status.find("Assets imported.")!=std::string::npos)
        {
            selected=shell.Document().Text(shell.AssetsPreview()).find(se::asset_io::Utf8(source.filename()))!=std::string_view::npos;
            require(selected,"successful import auto-selects primary FBX");
            require(fs::exists(content/source.filename()),"external FBX copied into configured Art root");
            require(import(),"repeated external import starts validation");stage=2;
        }
        else if(stage==2&&status.find("Target file exists")!=std::string::npos)
        {chosen=bad;require(import(),"invalid FBX starts validation");stage=3;}
        else if(stage==3&&status.find("source is invalid")!=std::string::npos)
        {
            require(!fs::exists(content/bad.filename()),"invalid FBX leaves no copied file");
            const auto before=picks;require(editor.StartPlay(),"empty runtime can start");
            playingRejected=!import()&&picks==before;require(playingRejected,"Play rejects import before picker");
            editor.StopPlay();stage=4;PostMessageW(renderer.window,WM_CLOSE,0,0);
        }
    };
    require(se::RunEditorApp(renderer,"en-US",root,true,600,&hooks)==0,"toolkit import host exits normally");
    require(renderer.valid&&stage==4&&selected&&playingRejected,"toolkit import workflow completes");
    se::AssetStore assets(content,root/"out/Cache/Assets");std::vector<se::AssetDiagnostic> diagnostics;
    require(assets.Scan(diagnostics),"imported store reopens");bool modelFound=false;
    for(const auto& id:assets.ListIds())if(assets.Resolve(id).filename()==source.filename())
    {
        const auto model=assets.LoadModel(id,diagnostics);modelFound=model&&model->meshes.size()==original.meshes.size();
    }
    require(modelFound,"registered FBX loads after editor closes");
    std::ifstream manifest(root/"project.json");nlohmann::json project;manifest>>project;
    require(!project["assets"].empty(),"project.json declares imported assets");
    std::vector<std::uint8_t> before,after;
    require(se::asset_io::Read(source,before)&&se::asset_io::Read(content/source.filename(),after)&&before==after,"FBX bytes preserved after conflict");
    require(!fs::exists(fs::path(source.wstring()+L".asset.json")),"external source remains untouched");
    std::cout<<"Toolkit import passed: "<<se::asset_io::Utf8(source.filename())<<", "<<original.meshes.size()<<" meshes, "<<original.dependencies.size()<<" dependencies.\n";
}
int wmain(int argc,wchar_t** argv)
{
    const auto project=std::filesystem::absolute(argc>1?argv[1]:L".cache/validation/editor-host")/se::asset_io::NewId();
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
    try
    {
        ToolkitImportFlow(project,std::filesystem::absolute(argc>2?std::filesystem::path(argv[2]):std::filesystem::path("Resources/Samples/Models/Hierarchy.fbx")));
    }
    catch(const std::exception& failure){std::cerr<<"Toolkit import failed: "<<failure.what()<<'\n';return 1;}
    std::cout<<"EditorApplicationTests passed (finite hidden host, CPU draw data, shutdown lifetime, read-only startup, failures).\n";
    return 0;
}
