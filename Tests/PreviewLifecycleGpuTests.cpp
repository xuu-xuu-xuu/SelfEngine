#include "ToolLayer/Editor/EditorApplication.h"
#include "ToolLayer/Editor/EditorSession.h"
#include "FunctionLayer/GameUI/UiTree.h"
#include "FunctionLayer/Physics/PhysicsWorld.h"
#include "FunctionLayer/Rendering/SandboxRenderer.h"
#include "FunctionLayer/Application/FrameData.h"
#include "ResourceLayer/Assets/Primitives.h"
#include <nlohmann/json.hpp>
#include <algorithm>
#include <cstdlib>
#include <fstream>
#include <iostream>
#include <stdexcept>
using namespace se;
namespace fs=std::filesystem;
namespace
{
void Check(bool value,const char* message){if(!value)throw std::runtime_error(message);}
struct Observe:Renderer
{
    SandboxRenderer gpu{SandboxScene::Scene};fs::path output;unsigned frames=0;bool valid=true;std::vector<rhi::Statistics> samples;
    explicit Observe(fs::path path):output(std::move(path)){}
    bool Initialize(const RenderSurface& surface,const Localization& text)override{return gpu.Initialize(surface,text);}
    RenderResult Render(const RenderFrame& frame,const Localization& text)override
    {
        ++frames;Check(frame.data&&frame.data->editorGuiEnabled,"actual preview editor frame");
        if(frames==77||frames==84)gpu.SetCaptureOutput(output/(std::to_string(frames)+".ppm"),frame.updateIndex);
        const auto result=gpu.Render(frame,text);
        if(frames%12==0){Check(bool(gpu.WaitIdle()),"preview fence drain");samples.push_back(gpu.Statistics());}
        return result;
    }
    EditorRenderView QueryEditorView()const override{return gpu.QueryEditorView();}
    void Shutdown()noexcept override{const auto stats=gpu.Statistics();valid=valid&&!stats.debugWarnings&&!stats.debugErrors;gpu.Shutdown();}
};
void Test(const fs::path& root)
{
    const auto project=root/"project";fs::create_directories(project/"Assets");Observe renderer(root);EditorApplicationHooks hooks;
    PhysicsWorld physics;physics.gravity={};BodyHandle body;EntityHandle runtime;UiId button=0;unsigned frames=0,starts=0,stops=0;UiTree* design=nullptr;std::string editUi;
    hooks.initialize=[&](EditorSession& editor,UiTree& tree){
        design=&tree;const auto label=tree.Create(UiKind::Label);Check(tree.SetText(label,"Edit UI"),"edit UI label");editUi=tree.SaveJson();
        return editor.Apply("Editor.PreviewTest",[](World& world){const auto camera=world.Create("camera",1),cube=world.Create("edit cube",2);Transform view;view.position={0,1,5};return world.SetTransform(camera,view)&&world.SetCamera(camera,{Pi/3,.1f,100,true})&&world.SetMesh(cube,{BuiltinCubeAssetId,{}});});
    };
    hooks.playStarted=[&](World& world,UiTree& tree){
        ++starts;runtime=world.Create("runtime cube",100);Transform transform;transform.position={1,1,0};Check(world.SetTransform(runtime,transform)&&world.SetMesh(runtime,{BuiltinCubeAssetId,{}}),"runtime mesh");
        BodyDesc description;description.entityStableId=100;description.shape.center=transform.position;body=physics.CreateBody(description);Check(bool(body),"runtime body");
        button=tree.Create(UiKind::Button);auto style=tree.Get(button)->style;style.offset={12,44};style.size={190,32};Check(tree.SetStyle(button,style)&&tree.SetText(button,"Runtime menu"),"runtime UI label");tree.FadeTo(button,.5f,.1);return true;
    };
    hooks.update=[&](World& world,UiTree& tree,double){Check(physics.Step(1.0f/60),"runtime physics");auto transform=*world.GetTransform(runtime);transform.position=physics.GetBody(body)->description.shape.center;Check(world.SetTransform(runtime,transform),"physics pose extraction");tree.Update(1.0/60);return true;};
    hooks.playStopping=[&](World& world,UiTree& tree){++stops;Check(world.IsAlive(runtime)&&tree.Get(button)&&physics.DestroyBody(body)&&tree.Remove(button),"cleanup while runtime world/UI remain alive");Check(physics.Bodies().empty(),"runtime body count returns to zero");};
    hooks.frame=[&](EditorSession& editor,double){
        const auto phase=frames++%12;
        if(phase==0)Check(editor.StartPlay(),"repeat preview start");
        if(phase==8)editor.StopPlay();
        const auto position=editor.EditWorld().GetTransform(editor.EditWorld().FindByStableId(2))->position;
        Check(editor.EditWorld().Entities().size()==2&&position.x==0&&position.y==0&&position.z==0&&design->SaveJson()==editUi,"preview never changes edit scene or UI");return true;
    };
    Check(RunEditorApp(renderer,"en-US",project,true,288,&hooks)==0&&renderer.valid&&renderer.frames==288&&starts==24&&stops==24&&renderer.samples.size()==24,"actual same-device preview sequence");
    const auto& baseline=renderer.samples[4];nlohmann::json samples=nlohmann::json::array();
    for(size_t i=0;i<renderer.samples.size();++i){const auto& s=renderer.samples[i];Check(!s.debugWarnings&&!s.debugErrors&&!s.retiring&&!s.inFlightBatches&&!s.retiringDescriptors,"GPU frame references retire after completion");if(i>=4)Check(s.buffers==baseline.buffers&&s.textures==baseline.textures&&s.pipelines==baseline.pipelines&&s.persistentDescriptors==baseline.persistentDescriptors&&s.trackedResources==baseline.trackedResources,"warm preview resources do not grow across cycles");samples.push_back({{"cycle",i+1},{"buffers",s.buffers},{"textures",s.textures},{"pipelines",s.pipelines},{"descriptors",s.persistentDescriptors},{"trackedResources",s.trackedResources},{"retiring",s.retiring},{"inFlight",s.inFlightBatches}});}
    std::ofstream report(root/"verification.json");report<<nlohmann::json{{"success",true},{"frames",288},{"cycles",24},{"warmStableCycles",20},{"editUnchanged",true},{"runtimeCleanup",true},{"fenceDrained",true},{"zeroDebugMessages",true},{"samples",samples},{"manualInputVerified",false}}.dump(2);Check(bool(report),"preview GPU report");
}
}
int main(int argc,char** argv)
{
#ifdef _MSC_VER
    _set_error_mode(_OUT_TO_STDERR);_set_abort_behavior(0,_CALL_REPORTFAULT);
#endif
    try{const auto root=fs::absolute(argc>1?argv[1]:"out/validation/preview-lifecycle-gpu");fs::create_directories(root);Test(root);std::cout<<"PreviewLifecycleGpuTests passed (24 same-device previews, isolated edit data, fence retirement and warm resource plateau).\n";return 0;}catch(const std::exception& error){std::cerr<<error.what()<<'\n';return 1;}
}
