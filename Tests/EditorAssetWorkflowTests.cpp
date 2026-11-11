#include "ToolLayer/Editor/EditorApplication.h"
#include "ToolLayer/Editor/ImGuiEditor.h"
#include "ToolLayer/Editor/AssetImportService.h"
#include "ToolLayer/Editor/EditorAssetActions.h"
#include "ToolLayer/Tools/Project.h"
#include "FunctionLayer/Application/FrameData.h"
#include "FunctionLayer/Application/SandboxApp.h"
#include "FunctionLayer/Rendering/SandboxRenderer.h"
#include "ResourceLayer/Assets/AssetIO.h"
#include "ResourceLayer/Scenes/SceneSerializer.h"
#include <imgui/imgui_internal.h>
#include <nlohmann/json.hpp>
#include <ShlObj.h>
#include <shellapi.h>
#include <algorithm>
#include <cmath>
#include <cstring>
#include <fstream>
#include <functional>
#include <iostream>
#include <stdexcept>
#include <string_view>

namespace fs=std::filesystem;
using namespace se;
using Json=nlohmann::json;
namespace
{
void Check(bool value,const char* reason){if(!value)throw std::runtime_error(reason);}
std::string Read(const fs::path& path)
{std::ifstream input(path,std::ios::binary);Check(bool(input),"read workflow evidence");return {std::istreambuf_iterator<char>(input),{}};}
void Put(const fs::path& path,const std::string& text)
{fs::create_directories(path.parent_path());std::ofstream file(path,std::ios::binary|std::ios::trunc);file<<text;Check(bool(file),"write workflow evidence");}
void Copy(const fs::path& source,const fs::path& target)
{fs::create_directories(target.parent_path());Check(fs::copy_file(source,target),"copy isolated fixture without overwriting");}
fs::path Metadata(fs::path path){path+=".asset.json";return path;}
AssetId Id(const fs::path& path){return Json::parse(Read(Metadata(path))).at("id").get<std::string>();}
Json Files(const fs::path& root)
{
    Json result=Json::object();
    for(const auto& file:fs::recursive_directory_iterator(root))if(file.is_regular_file())
    {
        std::vector<std::uint8_t> bytes;Check(asset_io::Read(file.path(),bytes),"read directory evidence");
        result[asset_io::Utf8(file.path().lexically_relative(root))]={{"bytes",bytes.size()},{"sha256",asset_io::Hash(bytes)}};
    }
    return result;
}
void DropFiles(HWND window,const std::vector<fs::path>& sources)
{
    std::wstring list;for(const auto& source:sources){list+=source.wstring();list.push_back(L'\0');}list.push_back(L'\0');
    const auto bytes=sizeof(DROPFILES)+list.size()*sizeof(wchar_t);const auto memory=GlobalAlloc(GHND,bytes);
    Check(memory!=nullptr,"allocate native file-drop payload");auto* drop=static_cast<DROPFILES*>(GlobalLock(memory));
    if(!drop){GlobalFree(memory);throw std::runtime_error("lock native file-drop payload");}
    drop->pFiles=sizeof(DROPFILES);drop->fWide=TRUE;
    std::memcpy(reinterpret_cast<char*>(drop)+sizeof(DROPFILES),list.data(),list.size()*sizeof(wchar_t));GlobalUnlock(memory);
    // WindowsEditorInput 接管 HDROP，并在真实 WM_DROPFILES 路径中 DragFinish。
    SendMessageW(window,WM_DROPFILES,reinterpret_cast<WPARAM>(memory),0);
}
class WorkflowRenderer final:public Renderer
{
public:
    SandboxRenderer gpu{SandboxScene::Scene};HWND window=nullptr;UiRect viewport;
    unsigned frames=0,captures=0;bool valid=true,blankFirstThree=false;
    std::string pendingCapture;fs::path output;SandboxDrawStatistics draws;rhi::Statistics statistics;
    explicit WorkflowRenderer(fs::path directory):output(std::move(directory)){fs::create_directories(output);}
    bool Initialize(const RenderSurface& surface,const Localization& text)override
    {window=surface.window;return gpu.Initialize(surface,text);}
    RenderResult Render(const RenderFrame& frame,const Localization& text)override
    {
        Check(frame.data!=nullptr,"real renderer receives owned frame data");++frames;viewport=frame.data->sceneViewport;
        auto observed=frame;FrameData snapshot=*frame.data;
        if(blankFirstThree&&frames<=3)snapshot.meshes.clear();observed.data=&snapshot;
        if(!pendingCapture.empty()){gpu.SetCaptureOutput(output/pendingCapture,frame.updateIndex);pendingCapture.clear();++captures;}
        const auto result=gpu.Render(observed,text);draws=gpu.DrawStatistics();statistics=gpu.Statistics();return result;
    }
    EditorRenderView QueryEditorView()const override{return gpu.QueryEditorView();}
    void Shutdown()noexcept override
    {statistics=gpu.Statistics();valid=valid&&!statistics.debugWarnings&&!statistics.debugErrors;gpu.Shutdown();}
};
struct ContextScope
{
    ImGuiContext* previous=ImGui::GetCurrentContext();
    explicit ContextScope(ImGuiContext* context){ImGui::SetCurrentContext(context);}
    ~ContextScope(){ImGui::SetCurrentContext(previous);}
};
ImGuiWindow* Window(const char* name)
{auto* window=ImGui::FindWindowByID(ImHashStr(name));return window&&window->Active&&!window->Hidden?window:nullptr;}
struct UiDriver
{
    struct Step{std::string name;std::function<bool()> run;};
    std::vector<Step> steps;std::size_t current=0;ULONGLONG started=0;Json evidence=Json::array();fs::path output;
    explicit UiDriver(fs::path path):output(std::move(path)){}
    void Add(std::string name,std::function<bool()> run){steps.push_back({std::move(name),std::move(run)});}
    void Click(const char* window,const char* key)
    {
        Add(std::string("click ")+key,[window,key]{auto* found=Window(window);if(!found)return false;
            ImGui::FocusWindow(found);ImGui::ActivateItemByID(found->GetID((std::string("###")+key).c_str()));return true;});
    }
    void Text(const char* window,const char* key,std::function<std::string()> value)
    {
        Add(std::string("focus ")+key,[window,key]{auto* found=Window(window);if(!found)return false;
            ImGui::FocusWindow(found);ImGui::ActivateItemByID(found->GetID((std::string("###")+key).c_str()));
            ImGui::GetCurrentContext()->NavNextActivateFlags=ImGuiActivateFlags_PreferInput;return true;});
        Add(std::string("type ")+key,[window,key,value]{auto* found=Window(window);if(!found)return false;
            const auto id=found->GetID((std::string("###")+key).c_str());auto* state=ImGui::GetInputTextState(id);
            if(!state||ImGui::GetCurrentContext()->ActiveId!=id)return false;
            state->SelectAll();ImGui::GetIO().AddInputCharactersUTF8(value().c_str());return true;});
        Add(std::string("verify typed ")+key,[window,key,value]{auto* found=Window(window);if(!found)return false;
            auto* state=ImGui::GetInputTextState(found->GetID((std::string("###")+key).c_str()));
            if(!state||std::string_view(state->TextA.Data,static_cast<std::size_t>(state->TextLen))!=value())return false;
            ImGui::ClearActiveID();return true;});
    }
    void Select(std::function<AssetId()> id,const char* kind,const char* filename)
    {
        Add(std::string("select ")+filename,[id,kind,filename]{auto* found=Window("###EditorUi.Assets");if(!found)return false;
            const auto seed=ImHashStr(id().c_str(),0,found->ID);const auto label=std::string("[")+kind+"] "+filename;
            ImGui::FocusWindow(found);ImGui::ActivateItemByID(ImHashStr(label.c_str(),0,seed));return true;});
    }
    void Tick()
    {
        if(current==steps.size())return;const auto now=GetTickCount64();if(!started)started=now;
        try
        {
            if(now-started>15000)throw std::runtime_error("UI step did not complete within 15 seconds");
            if(!steps[current].run())return;
            evidence.push_back({{"step",current},{"name",steps[current].name},{"elapsed_ms",now-started}});
            Put(output/"ui-steps.json",evidence.dump(2));++current;started=0;
        }
        catch(const std::exception& failure)
        {Put(output/"ui-failure.json",Json{{"step",current},{"name",steps[current].name},{"error",failure.what()}}.dump(2));throw;}
    }
};
struct Ppm{unsigned width=0,height=0;std::vector<std::uint8_t> rgb;};
Ppm Image(const fs::path& path)
{
    std::ifstream file(path,std::ios::binary);Ppm image;std::string magic;unsigned maximum=0;
    file>>magic>>image.width>>image.height>>maximum;Check(magic=="P6"&&maximum==255&&image.width&&image.height,"workflow GPU PPM header");file.get();
    image.rgb.resize(static_cast<std::size_t>(image.width)*image.height*3);
    file.read(reinterpret_cast<char*>(image.rgb.data()),static_cast<std::streamsize>(image.rgb.size()));Check(bool(file),"complete workflow GPU image");return image;
}
void Test(const fs::path& root)
{
    const auto incoming=root/L"外部 模型",project=root/L"工作 项目",content=project/"Content";
    const auto source=incoming/"Hierarchy.gltf",model=content/"Hierarchy.gltf",image=content/"checker.png",unused=content/"delete-me.png";
    const auto moved=content/"Moved/Renamed.gltf",movedImage=content/"Moved/checker.png";
    const auto scenePath=project/"Scenes/Main.scene.json",materialPath=content/"Materials/Workflow.material.json";
    fs::create_directories(content);fs::create_directories(scenePath.parent_path());
    Copy("Resources/Templates/BlankScene/project.json",project/"project.json");
    Copy("Resources/Samples/Models/Hierarchy.gltf",source);Copy("Resources/Samples/Models/Hierarchy.bin",incoming/"Hierarchy.bin");
    Copy("Resources/Samples/Models/checker.png",incoming/"delete-me.png");Put(incoming/"unreferenced.txt","must never enter project content");
    World initial;const auto camera=initial.Create("Camera",1000);Transform cameraTransform;cameraTransform.position={1.5f,1,4};
    Check(initial.SetTransform(camera,cameraTransform)&&initial.SetCamera(camera,{Pi/3,.1f,1000,true}),"workflow camera");
    std::string error;
    if(!SaveScene(scenePath,initial,error))throw std::runtime_error("save initial workflow scene: "+error);
    const auto initialScene=Read(scenePath);AssetStore verify(content,root/"verification-cache");
    std::vector<AssetDiagnostic> diagnostics;std::vector<EditorAssetIssue> issues;
    AssetId modelId,imageId,unusedId,materialId;StableEntityId temporary=0,placed=0;std::string modelMetadata,imageMetadata;
    Json incomingBefore;bool materialEdited=false,closed=false;
    EditorSession* session=nullptr;ImGuiEditor* frontend=nullptr;
    WorkflowRenderer renderer(root/"workflow");UiDriver driver(root/"workflow");
    const auto idle=[&]{return frontend&&!frontend->AssetOperationBusy()&&!frontend->FileDialogActive();};
    const auto scan=[&]{diagnostics.clear();Check(verify.Scan(diagnostics),"fresh independent asset index");};
    const auto modal=[&](const char* name,const char* capture){return [&,name,capture]{if(!Window(name))return false;renderer.pendingCapture=capture;return true;};};
    driver.Add("native drop with missing texture",[&]{if(renderer.frames<2)return false;DropFiles(renderer.window,{source});return true;});
    driver.Add("missing dependency confirmation",modal("asset-import-confirm","01-missing-dependency.ppm"));
    driver.Click("asset-import-confirm","EditorUi.Import");
    driver.Add("invalid confirmation is disabled",[&]{Check(Window("asset-import-confirm")&&!frontend->AssetOperationBusy(),"missing dependency cannot commit through UI");
        const auto plan=AssetImportService::Plan({{source},content,{}});Check(!plan.valid&&!fs::exists(model)&&Files(content).empty(),"real importer rejects absent texture without writes");return true;});
    driver.Click("asset-import-confirm","EditorUi.Cancel");
    driver.Add("repair external texture",[&]{if(!idle())return false;Copy("Resources/Samples/Models/checker.png",incoming/"checker.png");incomingBefore=Files(incoming);
        const auto plan=AssetImportService::Plan({{source},content,{}});Check(plan.valid&&plan.files.size()==3,"real model has exactly gltf, buffer and texture dependency closure");
        DropFiles(renderer.window,{source});return true;});
    driver.Add("valid import before cancellation",modal("asset-import-confirm","02-cancel-import.ppm"));
    driver.Click("asset-import-confirm","EditorUi.Cancel");
    driver.Add("cancel preserves source and target",[&]{if(!idle())return false;Check(Files(content).empty()&&Files(incoming)==incomingBefore,"cancelled UI import writes nothing");
        DropFiles(renderer.window,{source,incoming/"delete-me.png",incoming/"unreferenced.txt"});return true;});
    driver.Add("native multi-file candidate",modal("asset-import-confirm","03-import-closure.ppm"));
    driver.Click("asset-import-confirm","EditorUi.Import");
    driver.Add("background import published and registered",[&]{if(!idle())return false;
        Check(fs::exists(Metadata(model))&&fs::exists(Metadata(image))&&fs::exists(Metadata(unused)),"UI import registers copied targets and image dependency");
        Check(fs::exists(content/"Hierarchy.bin")&&!fs::exists(Metadata(content/"Hierarchy.bin"))&&!fs::exists(content/"unreferenced.txt"),"only declared dependency closure is copied");
        Check(Files(incoming)==incomingBefore,"external files remain byte-identical after import");scan();
        modelId=Id(model);imageId=Id(image);unusedId=Id(unused);modelMetadata=Read(Metadata(model));imageMetadata=Read(Metadata(image));
        Check(verify.Resolve(modelId)==fs::weakly_canonical(model)&&verify.ListIds().size()==3,"fresh store resolves registered model and images");
        Check(Json::parse(Read(project/"project.json")).at("assets").size()==3,"successful UI import synchronizes project declarations");
        Put(root/"after-import.json",Files(content).dump(2));DropFiles(renderer.window,{source});return true;});
    driver.Add("same-name conflict confirmation",modal("asset-import-confirm","04-import-conflict.ppm"));
    driver.Click("asset-import-confirm","EditorUi.Import");
    driver.Add("conflicting import stays uncommitted",[&]{Check(Window("asset-import-confirm")&&!frontend->AssetOperationBusy(),"conflicting import confirmation remains disabled");
        const auto plan=AssetImportService::Plan({{source},content,{}});Check(!plan.valid&&!plan.conflicts.empty()&&Read(Metadata(model))==modelMetadata,"same-name conflict preserves registered identity");return true;});
    driver.Click("asset-import-confirm","EditorUi.Cancel");driver.Add("wait after conflict",idle);
    driver.Select([&]{return unusedId;},"Images","delete-me.png");driver.Click("###EditorUi.Assets","Editor.AssetDelete");
    driver.Add("delete cancellation candidate",modal("asset-action-confirm","05-delete-cancel.ppm"));driver.Click("asset-action-confirm","EditorUi.Cancel");
    driver.Add("cancel deletion retains source and sidecar",[&]{if(!idle())return false;Check(fs::exists(unused)&&Id(unused)==unusedId,"cancelled delete retains identity");return true;});
    driver.Click("###EditorUi.Assets","Editor.AssetDelete");driver.Add("unreferenced delete candidate",modal("asset-action-confirm","06-delete-confirm.ppm"));
    driver.Click("asset-action-confirm","Editor.AssetConfirm");
    driver.Add("unreferenced delete commits manifest",[&]{if(!idle())return false;Check(!fs::exists(unused)&&!fs::exists(Metadata(unused)),"UI delete removes only selected file and sidecar");
        scan();Check(verify.ListIds().size()==2&&Json::parse(Read(project/"project.json")).at("assets").size()==2,"delete removes project declaration and refreshes index");return true;});
    driver.Add("unsaved reference fixture",[&]{temporary=session->Create("unsaved referenced model");Check(temporary&&session->SetMesh(temporary,{modelId,{}}),"create through editor command model");return true;});
    driver.Select([&]{return modelId;},"Models","Hierarchy.gltf");driver.Click("###EditorUi.Assets","Editor.AssetDelete");
    driver.Add("referenced deletion refused in dialog",modal("asset-action-confirm","07-referenced-delete.ppm"));driver.Click("asset-action-confirm","Editor.AssetConfirm");
    driver.Add("reference refusal preserves unsaved world",[&]{Check(Window("asset-action-confirm")&&!frontend->AssetOperationBusy()&&fs::exists(model),"referenced confirmation stays disabled");
        std::string memory;Check(SerializeScene(session->EditWorld(),memory,error),"serialize actual unsaved editor world");
        const auto plan=EditorAssetActions::Plan({EditorAssetAction::Delete,project,content,model,{},{{EditorAssetDocumentKind::Scene,scenePath,memory}}},verify);
        Check(!plan.valid&&std::any_of(plan.references.begin(),plan.references.end(),[&](const auto& reference){return reference.inMemory&&reference.entity==temporary;}),"reference report identifies exact unsaved entity");
        Check(Read(scenePath)==initialScene&&Read(Metadata(model))==modelMetadata,"refused delete does not save or remove user data");return true;});
    driver.Click("asset-action-confirm","EditorUi.Cancel");
    driver.Add("remove temporary reference",[&]{if(!idle())return false;Check(session->Delete(temporary)&&!session->Dirty(),"removing unsaved test entity restores original scene");return true;});
    driver.Text("###EditorUi.Assets","Editor.AssetTarget",[]{return "Moved/Renamed.gltf";});driver.Click("###EditorUi.Assets","Editor.AssetMove");
    driver.Add("model dependency move confirmation",modal("asset-action-confirm","08-move-closure.ppm"));driver.Click("asset-action-confirm","Editor.AssetConfirm");
    driver.Add("move preserves IDs and dependency bytes",[&]{if(!idle())return false;
        Check(!fs::exists(model)&&!fs::exists(image)&&!fs::exists(content/"Hierarchy.bin")&&fs::exists(content/"Moved/Hierarchy.bin"),"model move preserves relative dependency layout");
        Check(Read(Metadata(moved))==modelMetadata&&Read(Metadata(movedImage))==imageMetadata&&Id(moved)==modelId&&Id(movedImage)==imageId,"source and dependency sidecars retain exact identity bytes");
        scan();const auto decoded=verify.LoadModel(modelId,diagnostics);Check(decoded&&!decoded->meshes.empty()&&std::none_of(decoded->materials.begin(),decoded->materials.end(),[](const auto& material){return material.needsFallbackTexture;}),"moved dependency closure imports without a fallback");
        MaterialDocument material;material.name="Workflow material";material.baseColor={.08f,.85f,.2f,1};material.roughness=.4f;material.metallic=.1f;material.baseColorTexture=imageId;
        Check(verify.SaveMaterialSource(materialPath,material,materialId,diagnostics)&&EditorAssetActions::SyncProjectAssets(project,verify,issues),"material uses imported image and project declarations");
        placed=session->Create("Imported and moved model");Check(placed&&session->SetMesh(placed,{modelId,{}})&&session->Select(placed),"editor domain creates selected model with stable asset identity");
        Put(root/"after-move.json",Files(content).dump(2));return true;});
    driver.Click("###EditorUi.Assets","EditorUi.Rescan");
    driver.Text("###EditorUi.MaterialEditor","EditorUi.MaterialPath",[&]{return asset_io::Utf8(materialPath);});driver.Click("###EditorUi.MaterialEditor","EditorUi.OpenMaterial");
    driver.Click("###EditorUi.MaterialEditor","material.doubleSided");
    driver.Add("actual material editor has unsaved preview",[&]{const auto preview=frontend->CurrentMaterialPreview();if(!preview)return false;
        Check(preview->id==materialId&&preview->document.baseColorTexture==imageId&&preview->document.doubleSided,"material UI edits the imported-texture document");materialEdited=true;return true;});
    driver.Click("###EditorUi.MaterialEditor","EditorUi.SaveMaterial");driver.Click("###EditorUi.MaterialEditor","EditorUi.AssignMaterial");
    driver.Add("save-all through shared command route",[&]{const auto* mesh=session->EditWorld().GetMesh(session->EditWorld().FindByStableId(placed));
        if(!mesh||mesh->materials.empty()||mesh->materials[0]!=materialId)return false;
        Check(frontend->ExecuteCommand("editor.save-all").state==EditorCommandResult::Succeeded&&!session->Dirty()&&!frontend->CurrentMaterialPreview(),"shared save command persists actual assigned scene and material");
        scan();const auto material=verify.LoadMaterial(materialId,diagnostics);Check(material&&material->doubleSided&&material->baseColorTexture==imageId,"fresh material matches edited UI state");
        const auto deletion=EditorAssetActions::Plan({EditorAssetAction::Delete,project,content,moved,{},{}},verify);
        Check(!deletion.valid&&!deletion.references.empty(),"saved scene references also block deletion");
        const auto imageDeletion=EditorAssetActions::Plan({EditorAssetAction::Delete,project,content,movedImage,{},{}},verify);
        Check(!imageDeletion.valid&&!imageDeletion.references.empty(),"saved material keeps imported image alive");
        Check(EditorAssetActions::ValidatePlayAssets(session->EditWorld(),nullptr,verify,{},issues),"complete saved world passes real Play asset validation");
        ProjectValidation validation;std::vector<ProjectDiagnostic> projectDiagnostics;
        Check(ValidateProject(project/"project.json",validation,projectDiagnostics),"saved workflow is a valid project with resolvable export dependencies");
        renderer.pendingCapture="09-material-assigned.ppm";return true;});
    driver.Add("normal workflow close",[&]{frontend->RequestClose();return true;});

    EditorLayout layout;layout.SetVisible(EditorPanel::Hierarchy,false);layout.SetVisible(EditorPanel::Inspector,false);
    layout.SetVisible(EditorPanel::Console,false);layout.SetVisible(EditorPanel::MaterialEditor,true);layout.SetVisible(EditorPanel::UiDesigner,false);
    Check(layout.CaptureGeometry(EditorPanel::Assets,0,32,760,680,1280,720)&&layout.CaptureGeometry(EditorPanel::MaterialEditor,760,32,520,680,1280,720),"workflow panel geometry");
    const auto layoutPath=root/"workflow-layout.json";Check(layout.Save(layoutPath,error),"save isolated workflow layout");
    EditorApplicationHooks hooks;
    hooks.initialize=[&](EditorSession& editor,UiTree&){session=&editor;return editor.Open(scenePath,true);};
    hooks.configure=[&](ImGuiEditor& editor){frontend=&editor;editor.SetLayoutPath(layoutPath);editor.SetUiScale(1,1);editor.SetDocumentPaths(scenePath,{});};
    hooks.drawn=[&](ImGuiContext* context,const EditorFrameResult&){ContextScope scope(context);driver.Tick();};
    hooks.closing=[&](EditorSession& editor,UiTree&){closed=true;Check(!editor.Dirty()&&!editor.IsPlaying(),"workflow closes without unsaved or runtime state");};
    const auto result=RunEditorApp(renderer,"en-US",project,true,1200,&hooks);
    if(driver.current!=driver.steps.size())Put(root/"workflow/ui-incomplete.json",Json{{"step",driver.current},{"name",driver.steps[driver.current].name},
        {"total_steps",driver.steps.size()},{"frames",renderer.frames},{"application_result",result}}.dump(2));
    Check(result==0&&closed&&driver.current==driver.steps.size()&&renderer.frames<1200&&renderer.valid&&materialEdited,"actual UI/background/GPU workflow completes every phase");
    Check(renderer.captures==9&&renderer.gpu.CaptureSucceeded(),"every workflow boundary has an actual GPU capture");
    Check(Files(incoming)==incomingBefore,"all successful and refused operations preserve external source tree");

    World reopened;scan();Check(LoadScene(scenePath,reopened,error),"fresh scene reopens final workflow");
    const auto* mesh=reopened.GetMesh(reopened.FindByStableId(placed));Check(mesh&&mesh->meshAsset==modelId&&mesh->materials==std::vector<AssetId>{materialId},"saved scene retains moved model and assigned material IDs");
    EditorLayout playbackLayout;
    for(std::size_t index=0;index<EditorPanelCount;++index)playbackLayout.SetVisible(static_cast<EditorPanel>(index),index==static_cast<std::size_t>(EditorPanel::Viewport));
    Check(playbackLayout.CaptureGeometry(EditorPanel::Viewport,0,32,1280,688,1280,720),"unobscured viewport for pixel comparison");
    const auto playbackLayoutPath=root/"playback-layout.json";Check(playbackLayout.Save(playbackLayoutPath,error),"save isolated playback layout");
    WorkflowRenderer gui(root/"reopened-editor");EditorApplicationHooks playback;playback.startPlaying=true;
    playback.initialize=[&](EditorSession& editor,UiTree&){return editor.Open(scenePath,true);};
    playback.configure=[&](ImGuiEditor& editor){editor.SetLayoutPath(playbackLayoutPath);editor.SetUiScale(1,1);editor.SetAuxiliaryPanels(false);};
    playback.drawn=[&](ImGuiContext*,const EditorFrameResult&){if(gui.frames==2)gui.pendingCapture="scene.ppm";};
    Check(RunEditorApp(gui,"en-US",project,true,3,&playback)==0&&gui.valid&&gui.gpu.CaptureSucceeded()&&gui.captures==1,"reopened real GUI renders imported and moved assets");
    Check(gui.viewport.width>0&&gui.viewport.height>0&&gui.draws.draws>0,"reopened viewport contains actual geometry");
    WorkflowRenderer player(root/"player");player.blankFirstThree=true;ApplicationServices services;services.maximumFrames=6;
    services.initialWidth=static_cast<unsigned>(gui.viewport.width);services.initialHeight=static_cast<unsigned>(gui.viewport.height);
    services.extract=[&](FrameData& frame){BuildFrameData(reopened,nullptr,nullptr,frame);frame.assets=&verify;
        if(player.frames==2)player.pendingCapture="empty.ppm";else if(player.frames==5)player.pendingCapture="scene.ppm";};
    Check(RunSandboxApp("en-US",player,false,&services)==0&&player.valid&&player.gpu.CaptureSucceeded()&&player.captures==2,"independent Player uses freshly reopened assets");
    const auto editorImage=Image(gui.output/"scene.ppm"),playerImage=Image(player.output/"scene.ppm"),emptyImage=Image(player.output/"empty.ppm");
    Check(playerImage.width==emptyImage.width&&playerImage.height==emptyImage.height,"blank oracle dimensions");
    const auto left=static_cast<unsigned>(gui.viewport.x),top=static_cast<unsigned>(gui.viewport.y);
    Check(left+playerImage.width<=editorImage.width&&top+playerImage.height<=editorImage.height,"actual viewport crop lies inside GUI capture");
    unsigned maximum=0;std::size_t visiblePixels=0;
    for(unsigned y=0;y<playerImage.height;++y)for(unsigned x=0;x<playerImage.width;++x)
    {
        const auto index=(static_cast<std::size_t>(y)*playerImage.width+x)*3;bool different=false;
        for(unsigned channel=0;channel<3;++channel)
        {
            const auto guiIndex=((static_cast<std::size_t>(y)+top)*editorImage.width+x+left)*3+channel;
            maximum=(std::max)(maximum,static_cast<unsigned>(std::abs(int(editorImage.rgb[guiIndex])-int(playerImage.rgb[index+channel]))));
            different=different||playerImage.rgb[index+channel]!=emptyImage.rgb[index+channel];
        }
        visiblePixels+=different?1u:0u;
    }
    Check(maximum<=1&&visiblePixels>100,"every GUI/Player RGB pixel agrees and imported model produces visible pixels");
    Put(root/"verification.json",Json{{"success",true},{"native_drop",true},{"ui_driven_import_cancel_conflict",true},{"ui_driven_move_delete",true},
        {"dependency_closure",true},{"sidecar_bytes_preserved",true},{"unsaved_reference_rejected",true},{"material_ui_saved",materialEdited},
        {"model_id",modelId},{"image_id",imageId},{"material_id",materialId},{"scene_entity",placed},{"gui_player_max_rgb_difference",maximum},
        {"visible_model_pixels",visiblePixels},{"workflow_frames",renderer.frames},{"ui_steps",driver.steps.size()},
        {"gpu_warnings",renderer.statistics.debugWarnings+gui.statistics.debugWarnings+player.statistics.debugWarnings},
        {"gpu_errors",renderer.statistics.debugErrors+gui.statistics.debugErrors+player.statistics.debugErrors},
        {"manual_input_verified",false},{"native_file_picker_verified",false}}.dump(2));
}
}
int main(int argc,char** argv)
{
    fs::path output;
    try
    {
        Check(argc==2,"usage: EditorAssetWorkflowTests <output-root>");const auto id=asset_io::NewId();Check(!id.empty(),"unique workflow identity");
        output=fs::absolute(argv[1])/id;Check(fs::create_directories(output),"create isolated workflow evidence directory");
        std::cout<<"EditorAssetWorkflowTests evidence: "<<asset_io::Utf8(output)<<std::endl;Test(output);
        std::cout<<"EditorAssetWorkflowTests passed (native drop, actual async dialogs, dependency move, material UI, save/reopen and GPU/Player pixels).\n";return 0;
    }
    catch(const std::exception& failure)
    {if(!output.empty())Put(output/"failure.txt",failure.what());std::cerr<<"EditorAssetWorkflowTests failed: "<<failure.what()<<'\n';return 1;}
}
