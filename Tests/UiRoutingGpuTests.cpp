#include "FunctionLayer/Application/SandboxApp.h"
#include "FunctionLayer/Application/FrameData.h"
#include "FunctionLayer/Application/FreeCameraInput.h"
#include "FunctionLayer/Rendering/SandboxRenderer.h"
#include "ResourceLayer/Assets/Primitives.h"
#include <fstream>
#include <iostream>
#include <stdexcept>
using namespace se;
namespace fs=std::filesystem;
namespace
{
void Check(bool value,const char* message){if(!value)throw std::runtime_error(message);}
UiId Node(UiTree& tree,UiKind kind,UiRect r,std::string text={},UiId parent=0)
{const auto id=tree.Create(kind,parent);UiStyle style;style.offset={r.x,r.y};style.size={r.width,r.height};Check(tree.SetStyle(id,style)&&tree.SetText(id,text),"UI fixture");return id;}
void Post(HWND window,UINT message,WPARAM wp,short x=0,short y=0){Check(PostMessageW(window,message,wp,MAKELPARAM(x,y))!=FALSE,"post native input");}
struct Observe:Renderer
{
    SandboxRenderer gpu{SandboxScene::Scene};fs::path output;UiTree& ui;UiId input;unsigned frame=0;bool valid=true,deletedCaptureReleased=false;
    Observe(fs::path root,UiTree& tree,UiId id):output(std::move(root)),ui(tree),input(id){fs::create_directories(output);}
    bool Initialize(const RenderSurface& s,const Localization& text)override
    {if(!gpu.Initialize(s,text))return false;Post(s.window,WM_SETFOCUS,0);return true;}
    RenderResult Render(const RenderFrame& f,const Localization& text)override
    {
        ++frame;if(frame==1||frame==2||frame==6)gpu.SetCaptureOutput(output/(std::to_string(frame)+".ppm"),f.updateIndex);
        const auto result=gpu.Render(f,text);
        if(frame==1)
        {Post(f.surface.window,WM_MOUSEMOVE,0,40,40);Post(f.surface.window,WM_LBUTTONDOWN,MK_LBUTTON,40,40);Post(f.surface.window,WM_LBUTTONUP,0,40,40);Post(f.surface.window,WM_KEYDOWN,'W');Post(f.surface.window,WM_RBUTTONDOWN,MK_RBUTTON,40,40);}
        if(frame==2){Post(f.surface.window,WM_LBUTTONDOWN,MK_LBUTTON,40,110);Post(f.surface.window,WM_CHAR,'A');}
        if(frame==3){deletedCaptureReleased=GetCapture()!=f.surface.window&&!ui.Get(input)&&!ui.Captured();Post(f.surface.window,WM_LBUTTONUP,0,600,400);}
        if(frame==4){Post(f.surface.window,WM_MOUSEMOVE,0,600,400);Post(f.surface.window,WM_LBUTTONDOWN,MK_LBUTTON,600,400);Post(f.surface.window,WM_LBUTTONUP,0,600,400);}
        if(frame==5){Post(f.surface.window,WM_KEYUP,'W');Post(f.surface.window,WM_RBUTTONUP,0,600,400);}
        return result;
    }
    EditorRenderView QueryEditorView()const override{return gpu.QueryEditorView();}
    void Shutdown()noexcept override{const auto s=gpu.Statistics();valid=valid&&!s.debugWarnings&&!s.debugErrors;gpu.Shutdown();}
};
std::vector<unsigned char> Ppm(const fs::path& path)
{std::ifstream file(path,std::ios::binary);std::string kind;unsigned width=0,height=0,max=0;file>>kind>>width>>height>>max;Check(kind=="P6"&&width==640&&height==480&&max==255,"routing PPM");file.get();std::vector<unsigned char> bytes(size_t(width)*height*3);file.read(reinterpret_cast<char*>(bytes.data()),static_cast<std::streamsize>(bytes.size()));Check(bool(file),"complete routing PPM");return bytes;}
void Test(const fs::path& root)
{
    UiTree ui;const auto panel=Node(ui,UiKind::Panel,{10,10,300,220});ui.SetColor(panel,{.04f,.08f,.12f,1});ui.SetBlocksPointer(panel,true);
    const auto bottom=Node(ui,UiKind::Button,{20,20,160,40},"Bottom",panel),top=Node(ui,UiKind::Button,{20,20,160,40},"Top button",panel);
    const auto input=Node(ui,UiKind::TextInput,{20,80,220,50},"Text input",panel),status=Node(ui,UiKind::Label,{20,160,260,40},"Clicks: 0",panel);
    World world;const auto camera=world.Create("camera",1),cube=world.Create("cube",2);FreeCameraState view{{0,0,5},0,0};
    Check(world.SetTransform(camera,FreeCameraTransform(view))&&world.SetCamera(camera,{Pi/3,.1f,100,true})&&world.SetMesh(cube,{BuiltinCubeAssetId,{}}),"actual scene camera and cube");
    PhysicsWorld physics;BodyDesc target;target.mass=0;target.entityStableId=2;Check(bool(physics.CreateBody(target)),"actual scene ray target");
    Observe renderer(root,ui,input);FreeCameraInputRouter router;unsigned frame=0,topClicks=0,bottomClicks=0,rays=0;bool uiCameraBlocked=true,deletedUpConsumed=false;
    ApplicationServices services;services.ui=&ui;services.maximumFrames=6;services.initialWidth=640;services.initialHeight=480;services.fixedDeltaSeconds=1.0/60;
    services.input=[&](const InputState& raw,bool keyboard,bool pointer)
    {
        const auto current=frame+1;const auto before=view.position;const auto intent=router.Capture(raw,raw.hasFocus,keyboard,pointer);Check(UpdateFreeCamera(view,intent,1.0/60),"game camera input");
        if(current==2||current==3)uiCameraBlocked=uiCameraBlocked&&view.position.x==before.x&&view.position.y==before.y&&view.position.z==before.z&&view.yaw==0&&view.pitch==0;
        if(current==4)deletedUpConsumed=pointer;
        if(!pointer)for(const auto& event:raw.pointerEvents)if(event.type==PointerEventType::Down&&event.button==0)
            if(physics.Raycast(view.position,Rotate(FreeCameraTransform(view).rotation,{0,0,-1}),10))++rays;
    };
    services.update=[&](double)
    {
        ++frame;for(const auto& event:ui.TakeEvents())if(event.kind==UiEventKind::Click){if(event.target==top)++topClicks;if(event.target==bottom)++bottomClicks;}
        if(frame==2)Check(ui.SetText(status,"Clicks: "+std::to_string(topClicks)),"visible button action");
        if(frame==3){Check(ui.Focused()==input&&ui.Captured()==input,"actual text focus and OS capture before delete");Check(ui.Remove(input),"delete focused control in application update");}
        return world.SetTransform(camera,FreeCameraTransform(view));
    };
    services.extract=[&](FrameData& data){BuildFrameData(world,&ui,nullptr,data);};
    Check(RunSandboxApp("en-US",renderer,false,&services)==0&&renderer.valid&&renderer.gpu.CaptureSucceeded(),"actual GPU input sequence");
    Check(topClicks==1&&bottomClicks==0&&rays==1&&uiCameraBlocked&&deletedUpConsumed&&renderer.deletedCaptureReleased&&view.position.z<5,"UI target, no ray leakage, focused camera block, deleted gesture and subsequent scene action");
    const auto a=Ppm(root/"1.ppm"),b=Ppm(root/"2.ppm");size_t pixels=0;for(unsigned y=170;y<210;++y)for(unsigned x=30;x<290;++x){const auto i=(size_t(y)*640+x)*3;if(a[i]!=b[i]||a[i+1]!=b[i+1]||a[i+2]!=b[i+2])++pixels;}
    Check(pixels>10,"button callback changes real visible text pixels");
    std::ofstream report(root/"verification.json");report<<"{\"success\":true,\"frames\":6,\"topClicks\":"<<topClicks<<",\"bottomClicks\":"<<bottomClicks<<",\"sceneRays\":"<<rays<<",\"changedTextPixels\":"<<pixels<<",\"uiCameraBlocked\":true,\"deleteReleasedCapture\":true,\"deleteUpConsumed\":true}\n";
}
}
int main(int argc,char** argv){try{Test(fs::absolute(argc>1?argv[1]:"out/validation/ui-routing-gpu"));std::cout<<"UiRoutingGpuTests passed (real Win32/GPU button, focus, deleted capture, gesture consumption, camera and ray filtering).\n";return 0;}catch(const std::exception& error){std::cerr<<error.what()<<'\n';return 1;}}
