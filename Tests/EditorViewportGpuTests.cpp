#include "ToolLayer/Editor/EditorApplication.h"
#include "ToolLayer/Editor/EditorRenderer.h"
#include "ToolLayer/Editor/EditorSession.h"
#include "ToolLayer/Editor/EditorGizmo.h"
#include "ToolLayer/Editor/EditorViewportCamera.h"
#include "FunctionLayer/Application/FrameData.h"
#include "ResourceLayer/Scenes/SceneSerializer.h"
#include "ResourceLayer/Assets/AssetIO.h"
#include <fstream>
#include <iostream>
#include <stdexcept>

using namespace se;
namespace
{
void Check(bool value,const char* label){if(!value)throw std::runtime_error(label);}
struct Probe final:Renderer
{
    EditorRenderer gpu;
    EditorSession* session=nullptr;
    StableEntityId cube=0;
    HWND window=nullptr;
    std::filesystem::path directory;
    int tick=0;
    Vec3 initialCamera;
    Transform initialTransform,moved,rotated,scaled;
    CameraSnapshot camera;
    float dpi=1;
    UiRect skyRegion;
    std::vector<std::string> captures;
    explicit Probe(std::filesystem::path path):directory(std::move(path)){}
    bool Initialize(const RenderSurface& surface,const Localization& text)override
    {window=surface.window;dpi=GetDpiForWindow(window)/96.f;return gpu.Initialize(surface,text);}
    void Pointer(UINT message,Vec2 p,WPARAM flags=0)
    {SendMessageW(window,message,flags,MAKELPARAM(std::lround(p.x*dpi),std::lround(p.y*dpi)));}
    void Key(int value)
    {SendMessageW(window,WM_KEYDOWN,value,0);SendMessageW(window,WM_KEYUP,value,0);}
    Transform Value(){return *session->EditWorld().GetTransform(session->EditWorld().FindByStableId(cube));}
    void Drag(GizmoMode mode)
    {
        const auto e=session->EditWorld().FindByStableId(cube);
        const auto origin=TransformPoint(session->EditWorld().GetWorldMatrix(e),{});
        const auto p=ProjectWorldPoint(camera,origin);Check(bool(p),"gizmo origin projects");
        const auto v=camera.view*Vec4{origin.x,origin.y,origin.z,1};
        const float radius=-v.z*2*std::tan(Pi/6)*80/camera.viewport.height;
        Vec2 start,direction;
        if(mode==GizmoMode::Rotate)
        {
            // X 轴旋转环上的一段；选择远离三环交点的位置。
            const float a=.7f,b=.8f;
            const auto x=ProjectWorldPoint(camera,origin+Vec3{0,-std::sin(a),std::cos(a)}*radius);
            const auto y=ProjectWorldPoint(camera,origin+Vec3{0,-std::sin(b),std::cos(b)}*radius);
            Check(x&&y,"rotation ring projects");start={x->x,x->y};direction={y->x-x->x,y->y-x->y};
        }
        else
        {
            const auto end=ProjectWorldPoint(camera,origin+Vec3{radius,0,0});Check(bool(end),"X axis projects");
            direction={end->x-p->x,end->y-p->y};start={p->x+direction.x*.8f,p->y+direction.y*.8f};
        }
        const float len=std::hypot(direction.x,direction.y);Check(len>1,"usable handle length");
        const Vec2 end{start.x+direction.x/len*28,start.y+direction.y/len*28};
        Pointer(WM_LBUTTONDOWN,start,MK_LBUTTON);Pointer(WM_MOUSEMOVE,end,MK_LBUTTON);Pointer(WM_LBUTTONUP,end);
    }
    RenderResult Render(const RenderFrame& frame,const Localization& text)override
    {
        ++tick;Check(frame.data&&frame.data->editorGuiEnabled,"toolkit GUI frame");
        dpi=frame.data->editorGui.framebufferScale.x;
        if(frame.data->editorCamera)camera=*frame.data->editorCamera;
        std::string capture;
        if(tick==4)capture="initial";
        if(tick==5)capture="selected";
        if(tick==7)capture="moved";
        if(tick==9)capture="rotated";
        if(tick==11)capture="scaled";
        if(tick==15)capture="navigated";
        if(tick==17)capture="play";
        if(tick==19)capture="stop";
        if(tick==21)capture="sky-hidden";
        if(tick==22)capture="sky-translated";
        if(tick==25)capture="resized";
        if(!capture.empty()){gpu.SetCaptureOutput(directory/(capture+".ppm"),frame.updateIndex);captures.push_back(capture);}
        FrameData overridden;
        auto submitted=frame;
        if(tick==21){overridden=*frame.data;overridden.editorSkyEnabled=false;submitted.data=&overridden;}
        if(tick==22)
        {
            overridden=*frame.data;CameraSnapshot shifted;CameraError error;
            Check(BuildCameraSnapshot({UINT64_MAX,Translation({7,3,9})*camera.inverseView,camera.lens,true},camera.viewport,frame.updateIndex,shifted,error),"translated sky camera");
            overridden.editorCamera=shifted;submitted.data=&overridden;
        }
        const auto result=gpu.Render(submitted,text);Check(result!=RenderResult::Failed,"GPU render");
        if(!capture.empty())Check(gpu.CaptureSucceeded(),"GPU capture");
        if(tick==4)
        {
            initialCamera=camera.position;initialTransform=Value();
            auto p=ProjectWorldPoint(camera,initialTransform.position);Check(bool(p),"cube projects");
            // 相机/灯光图标可能投影在立方体中心，点击右侧可见的实体表面。
            Pointer(WM_LBUTTONDOWN,{p->x+20,p->y},MK_LBUTTON);Pointer(WM_LBUTTONUP,{p->x+20,p->y});
        }
        if(tick==5){Check(session->Selected()==cube&&frame.data->editorSelected==cube,"window selection reaches the visible mesh outline pass");Key('W');}
        if(tick==6)Drag(GizmoMode::Translate);
        if(tick==7){moved=Value();Check(Length(moved.position-initialTransform.position)>.01f,"window move handle changes position");Key('E');}
        if(tick==8)Drag(GizmoMode::Rotate);
        if(tick==9){rotated=Value();Check(rotated.Matrix().values!=moved.Matrix().values,"window rotation handle changes rotation");Key('R');}
        if(tick==10)Drag(GizmoMode::Scale);
        if(tick==11){scaled=Value();Check(Length(scaled.scale-rotated.scale)>.01f,"window scale handle changes scale");Pointer(WM_RBUTTONDOWN,{camera.viewport.x+100,camera.viewport.y+100},MK_RBUTTON);SendMessageW(window,WM_KEYDOWN,'W',0);}
        if(tick==14){SendMessageW(window,WM_KEYUP,'W',0);Pointer(WM_RBUTTONUP,{camera.viewport.x+100,camera.viewport.y+100});}
        if(tick==15){Check(Length(camera.position-initialCamera)>.001f,"window WASD changes rendered camera");Check(session->StartPlay(),"start Play");}
        if(tick==17){Check(!frame.data->editorCamera,"Play uses game camera");session->StopPlay();}
        if(tick==19)
        {
            Check(frame.data->editorCamera&&Length(camera.position-initialCamera)>.001f,"Stop restores navigated camera");
            skyRegion={(camera.viewport.x+30)*dpi,(camera.viewport.y+90)*dpi,(camera.viewport.width-60)*dpi,20*dpi};
        }
        if(tick==23)SetWindowPos(window,nullptr,0,0,1100,720,SWP_NOMOVE|SWP_NOZORDER|SWP_NOACTIVATE);
        return result;
    }
    EditorRenderView QueryEditorView()const override{return gpu.QueryEditorView();}
    void Shutdown()noexcept override{gpu.Shutdown();}
};
std::vector<unsigned char> Pixels(const std::filesystem::path& path,int& width,int& height)
{
    std::ifstream file(path,std::ios::binary);std::string magic;int maximum=0;file>>magic>>width>>height>>maximum;file.get();
    Check(magic=="P6"&&maximum==255,"capture PPM header");
    std::vector<unsigned char> pixels(static_cast<std::size_t>(width)*height*3);
    file.read(reinterpret_cast<char*>(pixels.data()),static_cast<std::streamsize>(pixels.size()));Check(bool(file),"capture bytes");return pixels;
}
void CheckSky(const Probe& probe)
{
    int width=0,height=0,w=0,h=0;const auto sky=Pixels(probe.directory/"stop.ppm",width,height);
    const auto translated=Pixels(probe.directory/"sky-translated.ppm",w,h);Check(w==width&&h==height,"sky comparison size");
    const auto hidden=Pixels(probe.directory/"sky-hidden.ppm",w,h);int maximumDelta=0;std::size_t blue=0,changed=0;
    for(int y=static_cast<int>(probe.skyRegion.y);y<probe.skyRegion.y+probe.skyRegion.height&&y<height;++y)
        for(int x=static_cast<int>(probe.skyRegion.x);x<probe.skyRegion.x+probe.skyRegion.width&&x<width;++x)
        {
            const auto i=(static_cast<std::size_t>(y)*width+x)*3;
            blue+=sky[i+2]>sky[i]+8;
            for(int c=0;c<3;++c){maximumDelta=std::max(maximumDelta,std::abs(int(sky[i+c])-int(translated[i+c])));changed+=sky[i+c]!=hidden[i+c];}
        }
    Check(blue>100&&changed>100,"sky has blue color and display toggle changes sky pixels");
    Check(maximumDelta<=2,"sky does not move when camera translates");
}
}
int main(int argc,char** argv)
{
    try
    {
        const auto directory=std::filesystem::absolute(argc>1?argv[1]:"out/validation/editor-viewport-unity/gpu");
        std::filesystem::create_directories(directory);
        const auto project=directory/("project-"+asset_io::NewId());std::filesystem::create_directories(project/"Assets");
        SetEnvironmentVariableW(L"SELFENGINE_EDITOR_TRACE_VIEWPORT",L"1");
        Probe probe(directory);EditorApplicationHooks hooks;hooks.editorUi="toolkit";
        hooks.initialize=[&](EditorSession& session,UiTree&){
            probe.session=&session;std::string error;Check(session.NewDefaultScene(error),"default scene");
            for(auto e:session.EditWorld().Entities())if(session.EditWorld().Get(e)->name=="Cube")probe.cube=session.EditWorld().Get(e)->stableId;
            Check(probe.cube!=0,"default cube");session.Select(0);return true;
        };
        Check(RunEditorApp(probe,"en-US",project,true,27,&hooks)==0,"editor application completed");
        Check(probe.tick==27&&probe.captures.size()==11,"completed GPU interaction matrix");
        CheckSky(probe);
        std::ofstream(directory/"verification.json")<<"{\"success\":true,\"sky\":true,\"camera\":true,\"selection\":true,\"translate\":true,\"rotate\":true,\"scale\":true,\"playStop\":true,\"resize\":true,\"captures\":11}\n";
        std::cout<<"Editor viewport GPU and window-message integration passed.\n";return 0;
    }
    catch(const std::exception& error){std::cerr<<"EditorViewportGpu: "<<error.what()<<'\n';return 1;}
}
