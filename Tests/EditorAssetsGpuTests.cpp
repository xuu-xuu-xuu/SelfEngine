#include "ToolLayer/Editor/EditorApplication.h"
#include "ToolLayer/Editor/EditorSession.h"
#include "FunctionLayer/Application/FrameData.h"
#include "FunctionLayer/Application/SandboxApp.h"
#include "FunctionLayer/Rendering/SandboxRenderer.h"
#include "ResourceLayer/Scenes/SceneSerializer.h"
#include "ResourceLayer/Assets/AssetIO.h"
#include <algorithm>
#include <cmath>
#include <fstream>
#include <iostream>
#include <stdexcept>

using namespace se;
namespace fs=std::filesystem;
namespace
{
void Check(bool value,const char* message){if(!value)throw std::runtime_error(message);}
std::string Read(const fs::path& path){std::ifstream file(path,std::ios::binary);return {std::istreambuf_iterator<char>(file),{}};}
struct Ppm {unsigned width=0,height=0;std::vector<unsigned char> pixels;};
Ppm Image(const fs::path& path)
{
    std::ifstream file(path,std::ios::binary);Ppm image;std::string kind;unsigned maximum=0;
    file>>kind>>image.width>>image.height>>maximum;Check(kind=="P6"&&maximum==255&&image.width&&image.height,"PPM header");file.get();
    image.pixels.resize(size_t(image.width)*image.height*3);file.read(reinterpret_cast<char*>(image.pixels.data()),static_cast<std::streamsize>(image.pixels.size()));Check(bool(file),"complete PPM");return image;
}
std::size_t Different(const Ppm& a,const Ppm& b)
{Check(a.width==b.width&&a.height==b.height,"equal image dimensions");std::size_t count=0;for(size_t i=0;i<a.pixels.size();i+=3)if(a.pixels[i]!=b.pixels[i]||a.pixels[i+1]!=b.pixels[i+1]||a.pixels[i+2]!=b.pixels[i+2])++count;return count;}
class Observe final:public Renderer
{
public:
    SandboxRenderer gpu{SandboxScene::Scene};fs::path output;MaterialPreview preview;
    AssetId sharedModel;std::weak_ptr<const StaticModel> firstModel;bool sharedModelSeen=false;
    bool transitions=false,valid=true;unsigned frames=0;UiRect viewport;SandboxDrawStatistics draws;
    std::vector<rhi::Statistics> statistics;
    explicit Observe(fs::path path):output(std::move(path)){}
    bool Initialize(const RenderSurface& surface,const Localization& text)override{return gpu.Initialize(surface,text);}
    RenderResult Render(const RenderFrame& frame,const Localization& text)override
    {
        Check(frame.data!=nullptr,"owned scene snapshot");FrameData snapshot=*frame.data;++frames;viewport=snapshot.sceneViewport;
        if(transitions&&frames<=3)for(auto& mesh:snapshot.meshes)mesh.mesh.materials.clear();
        if(transitions&&frames>=7&&frames<=9)snapshot.materialPreview=preview;
        if((transitions&&frames%3==0)||(!transitions&&frames==3))gpu.SetCaptureOutput(output/(std::to_string(frames)+".ppm"),frame.updateIndex);
        auto observed=frame;observed.data=&snapshot;const auto result=gpu.Render(observed,text);statistics.push_back(gpu.Statistics());draws=gpu.DrawStatistics();
        if(!sharedModel.empty())
        {
            std::vector<AssetDiagnostic> diagnostics;const auto source=snapshot.assets->LoadModel(sharedModel,diagnostics);
            Check(source!=nullptr,"shared immutable model source");
            if(sharedModelSeen)Check(firstModel.lock()==source,"Play/Stop shares the same immutable CPU model revision");
            else{firstModel=source;sharedModelSeen=true;}
        }
        return result;
    }
    EditorRenderView QueryEditorView()const override{return gpu.QueryEditorView();}
    void Shutdown()noexcept override{const auto stats=gpu.Statistics();valid=valid&&stats.debugWarnings==0&&stats.debugErrors==0;gpu.Shutdown();}
};
void Run(Observe& renderer,const World& world,AssetStore& assets,unsigned frames,unsigned width=320,unsigned height=240)
{
    fs::create_directories(renderer.output);ApplicationServices services;services.maximumFrames=frames;services.initialWidth=width;services.initialHeight=height;
    services.extract=[&](FrameData& snapshot){BuildFrameData(world,nullptr,nullptr,snapshot);snapshot.assets=&assets;};
    Check(RunSandboxApp("en-US",renderer,false,&services)==0&&renderer.valid&&renderer.frames==frames&&renderer.gpu.CaptureSucceeded(),"GPU run without debug diagnostics");
}
void Test(const fs::path& root)
{
    const auto project=root/"project",sources=project/"Assets";fs::create_directories(sources);
    fs::copy_file("Resources/Samples/Models/Hierarchy.fbx",sources/"Hierarchy.fbx");
    // 故意不复制 FBX 引用的 checker.png；修复贴图使用另一个资产名。
    fs::copy_file("Resources/Samples/Models/checker.png",sources/"repair.png");
    AssetStore assets(sources,root/"cache");AssetId modelId,imageId,materialId;std::vector<AssetDiagnostic> diagnostics;
    Check(assets.RegisterSource("Hierarchy.fbx",{},modelId,diagnostics),"register missing-texture FBX");const auto model=assets.LoadModel(modelId,diagnostics);
    Check(model&&!model->meshes.empty()&&std::any_of(diagnostics.begin(),diagnostics.end(),[](const auto& d){return d.key=="Assets.MissingTexture";}),"FBX remains usable with actual missing-texture diagnosis");
    Check(std::any_of(model->materials.begin(),model->materials.end(),[](const auto& m){return m.needsFallbackTexture;}),"missing texture marks fallback material");
    Check(assets.RegisterSource("repair.png",{},imageId,diagnostics),"register repair image");
    MaterialDocument red;red.name="FBX repair";red.baseColor={.95f,.08f,.05f,1};red.roughness=.7f;red.metallic=.2f;red.doubleSided=true;red.baseColorTexture=imageId;
    const auto materialPath=sources/"Materials/repair.material.json";
    Check(assets.SaveMaterialSource(materialPath,red,materialId,diagnostics),"save repair material with stable ID");const auto sourceBytes=Read(materialPath);
    World world;const auto entity=world.Create("repaired FBX",101),camera=world.Create("camera",102);
    Check(world.SetMesh(entity,{modelId,std::vector<AssetId>(std::max(size_t(1),model->materials.size()),materialId)}),"assign engine material to model slots");
    StaticModelLayout layout;Check(BuildStaticModelLayout(*model,layout,diagnostics),"model layout for known camera");
    const auto center=(layout.minimum+layout.maximum)*.5f;const float radius=Length(layout.maximum-layout.minimum)*.5f;Transform transform;transform.position=center+Vec3{0,0,std::max(2.f,radius*3)};
    Check(world.SetTransform(camera,transform)&&world.SetCamera(camera,{Pi/3,.1f,1000,true}),"fit scene camera");
    MaterialDocument green=red;green.baseColor={.05f,.9f,.15f,1};green.metallic=.65f;green.roughness=.25f;
    Observe changes(root/"changes");changes.transitions=true;changes.preview={materialId,green};Run(changes,world,assets,12);
    const auto fallback=Image(changes.output/"3.ppm"),saved=Image(changes.output/"6.ppm"),preview=Image(changes.output/"9.ppm"),reverted=Image(changes.output/"12.ppm");
    const auto repairedPixels=Different(fallback,saved),previewPixels=Different(saved,preview);
    std::ofstream counts(root/"resource-counts.csv");counts<<"frame,textures,uploads,global_waits,resource_creations,pso_creations,shader_compilations\n";
    for(size_t i=0;i<changes.statistics.size();++i){const auto& s=changes.statistics[i];counts<<i+1<<','<<s.textures<<','<<s.uploadSubmissions<<','<<s.globalWaits<<','<<s.memoryResourceCreations<<','<<s.pipelineCreations<<','<<s.shaderProgramCompilations<<'\n';}counts.close();
    Check(repairedPixels>100&&previewPixels>100&&Different(saved,reverted)==0,"real FBX fallback, PBR repair, unsaved preview and exact revert");
    Check(Read(materialPath)==sourceBytes,"unsaved preview never modifies source material");
    const auto& before=changes.statistics.at(5);for(size_t i=6;i<changes.statistics.size();++i)
    {const auto& s=changes.statistics[i];Check(s.textures<=before.textures&&s.memoryResourceCreations==before.memoryResourceCreations&&s.uploadSubmissions==before.uploadSubmissions&&s.pipelineCreations==before.pipelineCreations&&s.shaderProgramCompilations==before.shaderProgramCompilations,"numeric material previews reuse GPU textures, uploads, programs and PSOs");}
    Check(changes.statistics[6].globalWaits==changes.statistics[7].globalWaits&&changes.statistics[9].globalWaits==changes.statistics[10].globalWaits,"preview revision changes add no global wait between capture frames");
    AssetId savedId;Check(assets.SaveMaterialSource(materialPath,green,savedId,diagnostics)&&savedId==materialId,"save modified material preserves ID");
    const auto scenePath=project/"repair.scene.json";std::string error;Check(SaveScene(scenePath,world,error),"save repaired scene");
    World reopened;AssetStore fresh(sources,root/"fresh-cache");Check(fresh.Scan(diagnostics)&&LoadScene(scenePath,reopened,error),"fresh store and scene reopen");
    const auto document=fresh.LoadMaterial(materialId,diagnostics);Check(document&&document->baseColor==green.baseColor&&document->baseColorTexture==imageId,"saved material and texture reference reload");
    Observe persisted(root/"persisted");Run(persisted,reopened,fresh,3);Check(Different(preview,Image(persisted.output/"3.ppm"))==0,"saved and reopened material matches unsaved GPU preview exactly");
    Observe editor(root/"editor");fs::create_directories(editor.output);EditorApplicationHooks hooks;hooks.startPlaying=true;
    hooks.initialize=[&](EditorSession& session,UiTree&){return session.Open(scenePath,true);};
    Check(RunEditorApp(editor,"en-US",project,true,3,&hooks)==0&&editor.valid&&editor.gpu.CaptureSucceeded(),"real GUI presents repaired PBR scene in Play");
    const auto rect=editor.viewport;Check(rect.width>0&&rect.height>0,"actual editor scene viewport");
    Observe player(root/"player");Run(player,reopened,fresh,3,static_cast<unsigned>(rect.width),static_cast<unsigned>(rect.height));
    const auto gui=Image(editor.output/"3.ppm"),standalone=Image(player.output/"3.ppm");unsigned maximum=0;
    for(unsigned y=0;y<standalone.height;++y)for(unsigned x=0;x<standalone.width;++x)for(unsigned c=0;c<3;++c)
    {const auto a=gui.pixels[((size_t(y)+static_cast<unsigned>(rect.y))*gui.width+x+static_cast<unsigned>(rect.x))*3+c],b=standalone.pixels[(size_t(y)*standalone.width+x)*3+c];maximum=std::max(maximum,static_cast<unsigned>(std::abs(int(a)-b)));}
    Check(maximum<=1,"every repaired PBR scene pixel agrees between editor and independent Player");
    World missing;const auto placeholder=missing.Create("missing model",201),missingCamera=missing.Create("camera",202);Transform view;view.position={0,0,3};
    Check(missing.SetMesh(placeholder,{asset_io::NewId(),{}})&&missing.SetTransform(missingCamera,view)&&missing.SetCamera(missingCamera,{Pi/3,.1f,1000,true}),"missing model fixture");
    Observe missingRenderer(root/"missing-model");Run(missingRenderer,missing,fresh,3);Check(missingRenderer.draws.draws==1,"missing model retains visible placeholder draw");
    std::ofstream report(root/"verification.txt");report<<"fallback_to_repaired_pixels="<<repairedPixels<<"\npreview_changed_pixels="<<previewPixels<<"\neditor_player_max_rgb_difference="<<maximum<<"\npreview_writes=0\nextra_numeric_texture_uploads=0\ndebug_warnings=0\ndebug_errors=0\n";
    const auto cyclesRoot=root/"play-cycles";fs::create_directories(cyclesRoot);Observe cycles(cyclesRoot);cycles.sharedModel=modelId;
    EditorApplicationHooks lifecycle;EditorSession* session=nullptr;std::string editBefore;unsigned tick=0,starts=0,stops=0;EntityHandle oldRuntime;UiId label=0;bool valid=true;
    lifecycle.initialize=[&](EditorSession& editorSession,UiTree& ui){session=&editorSession;label=ui.Create(UiKind::Label);return ui.SetText(label,"edit UI")&&editorSession.Open(scenePath,true)&&editorSession.Select(101)&&SerializeScene(editorSession.EditWorld(),editBefore,error);};
    lifecycle.playStarted=[&](World& runtime,UiTree& ui){
        ++starts;valid=valid&&(&runtime!=&session->EditWorld())&&!runtime.IsAlive(oldRuntime)&&ui.Count()==1;
        const auto original=runtime.FindByStableId(101);const auto mesh=*runtime.GetMesh(original);const auto transform=*runtime.GetTransform(original);
        valid=valid&&runtime.Destroy(original);oldRuntime=runtime.Create("runtime replacement");auto moved=transform;moved.position.x+=.2f;
        valid=valid&&runtime.SetMesh(oldRuntime,mesh)&&runtime.SetTransform(oldRuntime,moved)&&ui.SetText(label,"runtime UI");ui.Create(UiKind::TextInput);return valid;
    };
    lifecycle.playStopping=[&](World& runtime,UiTree& ui){++stops;valid=valid&&session->PlayWorld()==&runtime&&runtime.IsAlive(oldRuntime)&&!runtime.FindByStableId(101)&&ui.Count()==2&&ui.Get(label)->text=="runtime UI";};
    lifecycle.frame=[&](EditorSession& editorSession,double){
        ++tick;if(tick>4){const unsigned phase=(tick-5)%4;if(phase==0)valid=valid&&editorSession.StartPlay();else if(phase==2)editorSession.StopPlay();}
        std::string current;valid=valid&&SerializeScene(editorSession.EditWorld(),current,error)&&current==editBefore&&editorSession.Selected()==101&&editorSession.UndoCount()==0&&!editorSession.Dirty();return valid;
    };
    lifecycle.closing=[&](EditorSession& editorSession,UiTree& ui){valid=valid&&!editorSession.IsPlaying()&&ui.Count()==1&&ui.Get(label)->text=="edit UI";};
    Check(RunEditorApp(cycles,"en-US",project,true,132,&lifecycle)==0&&valid&&cycles.valid&&starts==32&&stops==32,"32 real GUI/GPU Play/Stop cycles preserve edit state and UI");
    const auto warm=cycles.statistics.at(15);std::ofstream cycleCounts(cyclesRoot/"resource-counts.csv");cycleCounts<<"frame,textures,buffers,uploads,resource_creations,pso_creations,global_waits\n";
    for(size_t i=0;i<cycles.statistics.size();++i)
    {
        const auto& s=cycles.statistics[i];cycleCounts<<i+1<<','<<s.textures<<','<<s.buffers<<','<<s.uploadSubmissions<<','<<s.memoryResourceCreations<<','<<s.pipelineCreations<<','<<s.globalWaits<<'\n';
        if(i>=16)Check(s.textures<=warm.textures&&s.buffers<=warm.buffers&&s.uploadSubmissions==warm.uploadSubmissions&&s.memoryResourceCreations==warm.memoryResourceCreations&&s.pipelineCreations==warm.pipelineCreations&&s.globalWaits==warm.globalWaits,"repeated Play/Stop does not grow or reupload immutable GPU resources");
    }
    report<<"play_stop_cycles="<<starts<<"\nimmutable_cpu_model_shared=1\ngpu_growth_after_warmup=0\n";
}
}
int main(int argc,char** argv)
{
    try{const auto root=fs::absolute(argc>1?argv[1]:"out/validation/editor-assets-gpu")/asset_io::NewId();fs::create_directories(root);Test(root);std::cout<<"EditorAssetsGpuTests passed (FBX diagnosis/repair, PBR preview/revert/reopen, texture reuse, GUI/Player and placeholder).\n";return 0;}
    catch(const std::exception& error){std::cerr<<"EditorAssetsGpuTests failed: "<<error.what()<<'\n';return 1;}
}
