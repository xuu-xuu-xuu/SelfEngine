#include "FunctionLayer/Application/SandboxApp.h"
#include "FunctionLayer/Application/FrameData.h"
#include "FunctionLayer/Rendering/SandboxRenderer.h"
#include "ToolLayer/Editor/EditorApplication.h"
#include "ToolLayer/Editor/EditorSession.h"
#include "ResourceLayer/Assets/AssetIO.h"
#include "Physics/PhysicsDemo.h"
#include "FirstPerson/FirstPersonDemo.h"
#include <fstream>
#include <iostream>
#include <stdexcept>

using namespace se;
namespace fs=std::filesystem;
namespace
{
void Check(bool value,const char* message){if(!value)throw std::runtime_error(message);}
struct Ppm{unsigned width=0,height=0;std::vector<unsigned char> rgb;};
Ppm Image(const fs::path& path){std::ifstream file(path,std::ios::binary);Ppm value;std::string kind;unsigned maximum=0;file>>kind>>value.width>>value.height>>maximum;Check(kind=="P6"&&maximum==255,"PPM header");file.get();value.rgb.resize(size_t(value.width)*value.height*3);file.read(reinterpret_cast<char*>(value.rgb.data()),static_cast<std::streamsize>(value.rgb.size()));Check(bool(file),"complete PPM");return value;}
std::size_t Different(const Ppm& a,const Ppm& b){Check(a.width==b.width&&a.height==b.height,"equal dimensions");std::size_t count=0;for(size_t i=0;i<a.rgb.size();i+=3)if(a.rgb[i]!=b.rgb[i]||a.rgb[i+1]!=b.rgb[i+1]||a.rgb[i+2]!=b.rgb[i+2])++count;return count;}
class Observe:public Renderer
{
public:
    SandboxRenderer gpu{SandboxScene::Scene};fs::path output;bool sequence=false,valid=true;unsigned frames=0;UiRect viewport;
    std::vector<rhi::Statistics> statistics;std::vector<SandboxDrawStatistics> draws;
    explicit Observe(fs::path path):output(std::move(path)){fs::create_directories(output);}
    bool Initialize(const RenderSurface& surface,const Localization& text)override{return gpu.Initialize(surface,text);}
    RenderResult Render(const RenderFrame& frame,const Localization& text)override
    {++frames;viewport=frame.data->sceneViewport;if((sequence&&frames%3==0)||(!sequence&&frames==3))gpu.SetCaptureOutput(output/(std::to_string(frames)+".ppm"),frame.updateIndex);const auto result=gpu.Render(frame,text);statistics.push_back(gpu.Statistics());draws.push_back(gpu.DrawStatistics());return result;}
    EditorRenderView QueryEditorView()const override{return gpu.QueryEditorView();}
    void Shutdown()noexcept override{const auto stats=gpu.Statistics();valid=valid&&!stats.debugWarnings&&!stats.debugErrors;gpu.Shutdown();}
};
void Camera(World& world){const auto camera=world.Create("camera",101);Check(world.SetTransform(camera,FreeCameraTransform({{0,1,5},0,-.15f}))&&world.SetCamera(camera,{Pi/3,.1f,100,true}),"test camera");}
std::pair<size_t,size_t> ContactDrawing(const fs::path& root)
{
    World world;Camera(world);PhysicsWorld physics;physics.gravity={};BodyDesc base;base.mass=0;Check(bool(physics.CreateBody(base)),"static contact box");
    BodyDesc top;top.shape.center={0,.9f,0};Check(bool(physics.CreateBody(top))&&physics.Step(),"four point physics contact");
    Check(physics.Contacts().size()==1&&physics.Contacts()[0].manifold.points.size()==4,"actual support contact set");
    Observe renderer(root/"contacts");renderer.sequence=true;unsigned frame=0;ApplicationServices services;services.maximumFrames=6;services.initialWidth=640;services.initialHeight=480;
    services.update=[&](double){++frame;return true;};services.extract=[&](FrameData& data){BuildFrameData(world,nullptr,&physics,data);if(frame>3)data.debugLines.resize(24);};
    Check(RunSandboxApp("en-US",renderer,false,&services)==0&&renderer.valid&&renderer.gpu.CaptureSucceeded(),"real GPU contact points and normals");
    Check(renderer.draws.at(2).debugLines==40&&renderer.draws.at(5).debugLines==24,"four contact crosses and normals are consumed");
    const auto marked=Image(renderer.output/"3.ppm"),plain=Image(renderer.output/"6.ppm");size_t red=0,cyan=0;
    for(size_t i=0;i<marked.rgb.size();i+=3)
    {
        const auto r=marked.rgb[i],g=marked.rgb[i+1],b=marked.rgb[i+2];
        if(r==plain.rgb[i]&&g==plain.rgb[i+1]&&b==plain.rgb[i+2])continue;
        if(r>180&&g<80&&b<80)++red;if(r<80&&g>180&&b>180)++cyan;
    }
    Check(red>20&&cyan>20,"actual red normal and cyan point pixels visible");
    return {red,cyan};
}
size_t StackDrawing(const fs::path& root)
{
    PhysicsDemo demo("en-US");auto services=demo.Services();const auto update=services.update;unsigned frame=0;size_t sleeping=0;
    services.maximumFrames=6;services.initialWidth=960;services.initialHeight=640;services.fixedDeltaSeconds=1.0/60;
    services.update=[&](double){++frame;if(frame==4){sleeping=demo.Physics().Statistics().sleeping;Check(sleeping>=16&&demo.PushBox(16,{2,0,0}),"sample stack sleeps and accepts real impulse");}
        for(unsigned i=0;i<(frame<=3?600u:200u);++i)if(!update(1.0/60))return false;return true;};
    Observe renderer(root/"stack");renderer.sequence=true;Check(RunSandboxApp("en-US",renderer,false,&services)==0&&renderer.valid&&renderer.gpu.CaptureSucceeded(),"actual 30-second stable sample and 10-second pushed sample");
    const auto pixels=Different(Image(renderer.output/"3.ppm"),Image(renderer.output/"6.ppm"));
    Check(pixels>100&&renderer.draws.at(2).debugLines>300&&demo.Physics().Statistics().sleeping>=16,"sample geometry moves after wake and settles again");return pixels;
}
std::pair<size_t,size_t> FirstPersonDrawing(const fs::path& root)
{
    FirstPersonDemo demo("en-US");auto services=demo.Services();const auto update=services.update;const auto input=services.input;unsigned frame=0;float stoppedZ=0,jumpY=0;
    services.maximumFrames=9;services.initialWidth=640;services.initialHeight=480;services.fixedDeltaSeconds=1.0/60;
    services.update=[&](double){++frame;InputState fake;fake.hasFocus=true;fake.keysDown['W']=frame>=4&&frame<=6;if(frame==7)fake.keyEvents.push_back({KeyEventType::Pressed,32});input(fake,false,false);
        const unsigned steps=frame<=3?20:frame<=6?40:10;for(unsigned i=0;i<steps;++i)if(!update(1.0/60))return false;
        Check(Length(demo.View().position-demo.Character().position-Vec3{0,.65f,0})<.00001f,"first person camera follows collision-controlled capsule");
        if(frame==6){stoppedZ=demo.Character().position.z;Check(stoppedZ>=-2.201f&&stoppedZ< -2.18f&&demo.Character().grounded&&demo.Target().has_value(),"sample WASD blocked by target and ray sees actual geometry");}
        if(frame==9){jumpY=demo.Character().position.y;Check(jumpY>2&&!demo.Character().grounded,"sample space edge produces physical jump");}return true;};
    Observe renderer(root/"first-person");renderer.sequence=true;Check(RunSandboxApp("en-US",renderer,false,&services)==0&&renderer.valid&&renderer.gpu.CaptureSucceeded(),"actual first person GPU input and capsule camera");
    const auto start=Image(renderer.output/"3.ppm"),blocked=Image(renderer.output/"6.ppm"),jump=Image(renderer.output/"9.ppm");const auto walkPixels=Different(start,blocked),jumpPixels=Different(blocked,jump);
    Check(walkPixels>100&&jumpPixels>100,"walking and jumping visibly alter camera projection");return {walkPixels,jumpPixels};
}
void Test(const fs::path& root)
{
    World world;Camera(world);PhysicsWorld physics;physics.gravity={};BodyDesc description;description.shape.halfExtents={1,.5f,.5f};description.shape.rotation=AxisAngle({0,1,0},.3f);const auto body=physics.CreateBody(description);Check(bool(body),"debug box");
    Observe renderer(root/"sequence");renderer.sequence=true;unsigned frame=0;ApplicationServices services;services.maximumFrames=12;services.initialWidth=320;services.initialHeight=240;
    services.update=[&](double){++frame;if(frame==7)Check(physics.Teleport(body,{1.5f,0,0},AxisAngle({0,1,0},.7f)),"move box");if(frame==10)Check(physics.DestroyBody(body),"remove box");return true;};
    services.extract=[&](FrameData& data){BuildFrameData(world,nullptr,frame>=4?&physics:nullptr,data);};
    Check(RunSandboxApp("en-US",renderer,false,&services)==0&&renderer.valid&&renderer.gpu.CaptureSucceeded(),"actual GPU debug sequence");
    const auto empty=Image(renderer.output/"3.ppm"),box=Image(renderer.output/"6.ppm"),moved=Image(renderer.output/"9.ppm"),removed=Image(renderer.output/"12.ppm");const auto visible=Different(empty,box),motion=Different(box,moved);
    Check(visible>100&&motion>100&&Different(empty,removed)==0,"wireframe visible, moving and fully removed without stale geometry");
    Check(renderer.draws.at(5).debugLines==12&&renderer.draws.at(5).draws==12&&renderer.draws.at(11).debugLines==0,"one existing cube draw per physical segment");
    const auto warm=renderer.statistics.at(2);for(size_t i=3;i<renderer.statistics.size();++i){const auto& s=renderer.statistics[i];Check(s.uploadSubmissions==warm.uploadSubmissions&&s.textures==warm.textures&&s.pipelineCreations==warm.pipelineCreations&&s.memoryResourceCreations==warm.memoryResourceCreations,"debug shape changes reuse geometry/textures/PSO without new GPU resources");}
    const auto project=root/"project";fs::create_directories(project/"Assets");PhysicsWorld guiPhysics;guiPhysics.gravity={};Check(bool(guiPhysics.CreateBody(description)),"GUI debug box");Observe editor(root/"editor");EditorApplicationHooks hooks;hooks.startPlaying=true;
    hooks.initialize=[](EditorSession& session,UiTree&){return session.Apply("test.camera",[](World& scene){Camera(scene);return true;});};
    hooks.extract=[&](World& scene,UiTree& ui,FrameData& data){BuildFrameData(scene,&ui,&guiPhysics,data);};
    Check(RunEditorApp(editor,"en-US",project,true,3,&hooks)==0&&editor.valid&&editor.gpu.CaptureSucceeded(),"actual GUI debug view");const auto rect=editor.viewport;
    Observe player(root/"player");ApplicationServices playerServices;playerServices.maximumFrames=3;playerServices.initialWidth=static_cast<unsigned>(rect.width);playerServices.initialHeight=static_cast<unsigned>(rect.height);playerServices.extract=[&](FrameData& data){BuildFrameData(world,nullptr,&guiPhysics,data);};
    Check(RunSandboxApp("en-US",player,false,&playerServices)==0&&player.valid&&player.gpu.CaptureSucceeded(),"independent debug Player");const auto gui=Image(editor.output/"3.ppm"),standalone=Image(player.output/"3.ppm");unsigned maximum=0;
    for(unsigned y=0;y<standalone.height;++y)for(unsigned x=0;x<standalone.width;++x)for(unsigned channel=0;channel<3;++channel)
    {const auto a=gui.rgb[((size_t(y)+static_cast<unsigned>(rect.y))*gui.width+x+static_cast<unsigned>(rect.x))*3+channel],b=standalone.rgb[(size_t(y)*standalone.width+x)*3+channel];maximum=std::max(maximum,static_cast<unsigned>(std::abs(int(a)-b)));}
    Check(maximum<=1&&editor.draws.back().debugLines==12,"every debug scene pixel matches GUI and independent Player");
    const auto [normalPixels,pointPixels]=ContactDrawing(root);
    const auto stackPixels=StackDrawing(root);
    const auto [walkPixels,jumpPixels]=FirstPersonDrawing(root);
    std::ofstream proof(root/"verification.json");proof<<"{\"schema\":1,\"success\":true,\"visible_pixels\":"<<visible<<",\"motion_pixels\":"<<motion<<",\"removed_pixels\":0,\"editor_player_max_rgb_difference\":"<<maximum<<",\"contact_normal_pixels\":"<<normalPixels<<",\"contact_point_pixels\":"<<pointPixels<<",\"stack_changed_pixels\":"<<stackPixels<<",\"stack_baseline_seconds\":30,\"stack_wake_seconds\":10,\"first_person_walk_pixels\":"<<walkPixels<<",\"first_person_jump_pixels\":"<<jumpPixels<<",\"new_texture_uploads\":0,\"debug_warnings\":0,\"debug_errors\":0}\n";
}
}
int main(int argc,char** argv){try{const auto root=fs::absolute(argc>1?argv[1]:"out/validation/physics-debug-gpu")/asset_io::NewId();fs::create_directories(root);Test(root);std::cout<<"PhysicsDebugGpuTests passed (wireframe move/remove, resource reuse, GUI and independent Player).\n";return 0;}catch(const std::exception& error){std::cerr<<"PhysicsDebugGpuTests failed: "<<error.what()<<'\n';return 1;}}
