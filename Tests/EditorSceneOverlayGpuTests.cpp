#include "ToolLayer/Editor/EditorRenderer.h"
#include "ToolLayer/Editor/EditorViewportController.h"
#include "FunctionLayer/Application/FrameData.h"
#include "PlatformLayer/Windows/Window.h"
#include "PlatformLayer/Windows/ExecutablePath.h"
#include "ResourceLayer/Assets/Primitives.h"
#include "CoreLayer/Localization.h"
#include <d3d12sdklayers.h>
#include <wrl/client.h>
#include <algorithm>
#include <fstream>
#include <iostream>
#include <stdexcept>

using namespace se;
namespace
{
void Check(bool value,const char* message){if(!value)throw std::runtime_error(message);}
struct Image
{
    int width=0,height=0;std::vector<unsigned char> pixels;
    explicit Image(const std::filesystem::path& path)
    {
        std::ifstream file(path,std::ios::binary);std::string magic;int maximum;
        file>>magic>>width>>height>>maximum;file.get();Check(magic=="P6"&&maximum==255,"PPM header");
        pixels.resize(size_t(width)*height*3);file.read(reinterpret_cast<char*>(pixels.data()),pixels.size());Check(bool(file),"PPM bytes");
    }
    bool Different(const Image& b,int x,int y)const
    {const auto p=(size_t(y)*width+x)*3;return pixels[p]!=b.pixels[p]||pixels[p+1]!=b.pixels[p+1]||pixels[p+2]!=b.pixels[p+2];}
};
// 独立的 CPU 射线参考，只判几何前后关系，不复用被测 GPU 深度／掩码实现。
std::optional<float> Box(const CameraRay& ray,Vec3 center,Vec3 half)
{
    float entry=0,exit=1e9f;
    const float origin[]{ray.origin.x-center.x,ray.origin.y-center.y,ray.origin.z-center.z};
    const float direction[]{ray.direction.x,ray.direction.y,ray.direction.z},extent[]{half.x,half.y,half.z};
    for(int i=0;i<3;++i)
    {
        if(std::abs(direction[i])<1e-8f){if(std::abs(origin[i])>extent[i])return {};continue;}
        float a=(-extent[i]-origin[i])/direction[i],b=(extent[i]-origin[i])/direction[i];if(a>b)std::swap(a,b);
        entry=std::max(entry,a);exit=std::min(exit,b);if(entry>exit)return {};
    }
    return entry;
}
std::optional<float> Sphere(const CameraRay& ray,Vec3 center,float radius)
{
    const auto v=ray.origin-center;const float b=Dot(v,ray.direction),d=b*b-Dot(v,v)+radius*radius;
    if(d<0)return {};const float t=-b-std::sqrt(d);return t>=0?std::optional<float>(t):std::nullopt;
}
struct Fixture
{
    std::filesystem::path directory;Localization text;WindowState window;EditorRenderer renderer;
    EditorSession session;AssetStore assets;EditorViewportCamera camera;EditorViewportController view{camera};
    StableEntityId cube=0,blocker=0;std::uint64_t frameIndex=0;int width=640,height=480;float scale=1;
    static constexpr Vec3 center{0,1,0};
    explicit Fixture(std::filesystem::path output):directory(std::move(output)),assets(".",directory/"cache")
    {
        std::filesystem::create_directories(directory);std::string error;
        Check(text.Load(GetExecutableDirectory()/"Resources/Localization/en-US.txt",error),"localization");
        window.postQuitOnDestroy=false;Check(CreateEngineWindow(window,false)&&SetEngineWindowClientSize(window,width,height),"GPU window");
        Check(renderer.Initialize({window.hwnd,UINT(width),UINT(height)},text),"renderer");
        Check(session.Apply("scene",[&](World& world){const auto e=world.Create("Selected mesh");cube=world.Get(e)->stableId;
            return world.SetMesh(e,{BuiltinCubeAssetId,{}})&&world.SetTransform(e,{center,{},{2,2,2}});}),"scene");
        camera.state={{3,3,6},.46f,-.32f};Check(session.Select(cube),"select cube");view.Action("Q",session,assets);
    }
    ~Fixture(){renderer.Shutdown();DestroyEngineWindow(window);}
    Image Capture(const std::string& name,bool grid,bool selected,const std::vector<rhi::SceneLine>* lines=nullptr)
    {
        view.Prepare(session,assets,{0,0,width/scale,height/scale},frameIndex+1);
        FrameData data;BuildFrameData(session.EditWorld(),nullptr,nullptr,data);data.assets=&assets;data.editorGuiEnabled=true;
        data.editorCamera=view.Camera();data.editorSelected=selected?session.Selected():0;
        data.sceneViewport={0,0,width/scale,height/scale};data.editorGridEnabled=grid;
        if(grid)data.editorSceneLines=view.SceneLines();if(lines)data.editorSceneLines=*lines;
        static const unsigned char white[]{255,255,255,255};data.editorFontPixels=white;data.editorFontWidth=data.editorFontHeight=1;
        auto& gui=data.editorGui;const float w=width/scale,h=height/scale;gui.displaySize={w,h};gui.framebufferScale={scale,scale};
        gui.vertices={{{0,0},{0,0}},{{w,0},{1,0}},{{w,h},{1,1}},{{0,h},{0,1}}};gui.indices={0,1,2,0,2,3};
        gui.commands={{{0,0,w,h},2,0,6,0,true}};
        const auto path=directory/(name+".ppm");RenderFrame frame;frame.surface={window.hwnd,UINT(width),UINT(height)};frame.data=&data;
        // 交替复用两个帧槽，同时覆盖掩码重用／resize 的资源寿命。
        for(int i=0;i<3;++i)
        {
            frame.updateIndex=++frameIndex;if(i==2)renderer.SetCaptureOutput(path,frameIndex);
            Check(renderer.Render(frame,text)!=RenderResult::Failed,"scene overlay GPU frame");
        }
        Check(renderer.CaptureSucceeded(),"capture");return Image(path);
    }
    CameraRay Ray(int x,int y){const auto r=ViewportPointToRay(*view.Camera(),{(x+.5f)/scale,(y+.5f)/scale});Check(bool(r),"pixel ray");return *r;}
    void Block(Vec3 position,Vec3 size)
    {
        Check(session.Apply("occluder",[&](World& world){auto e=world.FindByStableId(blocker);if(!e){e=world.Create("Occluder");blocker=world.Get(e)->stableId;}
            return world.SetMesh(e,{BuiltinCubeAssetId,{}})&&world.SetTransform(e,{position,{},size});}),"occluder");
    }
};
void GridAndSelection(Fixture& f,const std::string& prefix)
{
    const auto solid=f.Capture(prefix+"solid",false,false),grid=f.Capture(prefix+"grid",true,false);
    size_t hidden=0,visible=0;
    for(int y=0;y<f.height;++y)for(int x=0;x<f.width;++x)
    {
        const auto ray=f.Ray(x,y);const auto hit=Box(ray,Fixture::center,{.96f,.96f,.96f});
        const float plane=std::abs(ray.direction.y)>.001f?-ray.origin.y/ray.direction.y:-1;
        if(hit&&plane>*hit+.05f){++hidden;Check(!solid.Different(grid,x,y),"grid must not paint over the cube surface");}
        if(!Box(ray,Fixture::center,{1.05f,1.05f,1.05f})&&solid.Different(grid,x,y))++visible;
    }
    Check(hidden>1000&&visible>200,"reference samples include hidden and visible grid");
    const auto selected=f.Capture(prefix+"selected",false,true);size_t outline=0,filled=0;
    for(int y=0;y<f.height;++y)for(int x=0;x<f.width;++x)if(solid.Different(selected,x,y))
    {
        ++outline;Check(bool(Box(f.Ray(x,y),Fixture::center,{1.01f,1.01f,1.01f})),"outline stays inside visible mesh silhouette");
        const auto p=(size_t(y)*f.width+x)*3;
        Check(selected.pixels[p]>240&&selected.pixels[p+1]>135&&selected.pixels[p+1]<170&&selected.pixels[p+2]<45,"outline preserves orange sRGB color after composition");
        if(Box(f.Ray(x,y),Fixture::center,{.7f,.7f,.7f}))++filled;
    }
    Check(outline>80&&filled==0,"selected mesh has an outline without filling its interior or rear edges");
    std::cout<<prefix<<"hidden grid samples="<<hidden<<", visible grid="<<visible<<", outline="<<outline<<'\n';
}
void InfiniteWorldGrid(Fixture& f)
{
    const auto previous=f.camera.state;
    const auto sample=[&](const std::string& name,Vec3 camera,Vec3 anchor){
        f.camera.state={camera,0,-Pi/2+.01f};
        const auto solid=f.Capture(name+"-off",false,false),grid=f.Capture(name+"-on",true,false);
        const auto pixel=ProjectWorldPoint(*f.view.Camera(),anchor);
        Check(pixel&&pixel->x>=2&&pixel->x<f.width-2&&pixel->y>=2&&pixel->y<f.height-2,"world grid anchor projects inside viewport");
        bool changed=false;
        for(int dy=-1;dy<=1;++dy)for(int dx=-1;dx<=1;++dx)
            changed|=solid.Different(grid,int(pixel->x)+dx,int(pixel->y)+dy);
        Check(changed,"fixed one-metre world grid survives camera height and translation");
        size_t quadrants[4]{};
        for(int y=8;y<f.height-8;++y)for(int x=8;x<f.width-8;++x)
            if(solid.Different(grid,x,y))++quadrants[(y>=f.height/2)*2+(x>=f.width/2)];
        for(auto count:quadrants)Check(count>200,"grid covers all viewport quadrants without a finite mesh edge");
        std::cout<<name<<" fixed anchor="<<anchor.x<<","<<anchor.z<<", quadrant coverage="
            <<quadrants[0]<<","<<quadrants[1]<<","<<quadrants[2]<<","<<quadrants[3]<<'\n';
    };
    // 跨越旧 step=1→10 的距离阈值，并检查离原点远大于旧矩形的地面。
    sample("grid-height-30",{.3f,30,2.4f},{3,0,.35f});
    sample("grid-height-33",{.3f,33,2.4f},{3,0,.35f});
    sample("grid-translated-high",{700.3f,80,502.4f},{701,0,500.35f});
    f.camera.state=previous;
}
void PlaneSelection(Fixture& f,const std::string& prefix)
{
    StableEntityId plane=0;
    Check(f.session.Apply("plane-selection fixture",[&](World& world)
    {
        const auto e=world.Create("Selected ground plane");plane=world.Get(e)->stableId;
        return world.SetMesh(e,{BuiltinCubeAssetId,{}})&&world.SetTransform(e,{{0,-.05f,0},{},{8,.1f,8}});
    }),"create plane beneath unselected cube");
    Check(f.session.Select(plane)&&f.session.Selected()!=f.cube,"select only the plane");
    const auto solid=f.Capture(prefix+"plane-solid",false,false),selected=f.Capture(prefix+"plane-selected",false,true);
    const auto count=size_t(f.width)*f.height;
    std::vector<std::uint8_t> cubePixels(count),planeInterior(count);
    size_t interiorSamples=0,cubeSamples=0,interiorChanged=0,cubeChanged=0,totalOutline=0,adjacentSamples=0;
    for(int y=0;y<f.height;++y)for(int x=0;x<f.width;++x)
    {
        const auto p=size_t(y)*f.width+x;const auto ray=f.Ray(x,y);
        cubePixels[p]=bool(Box(ray,Fixture::center,{1,1,1}));
        // 平面的完整几何内部区域，包含方块投影遮住的部分；远离平面真实外边界。
        if(ray.direction.y<-.001f)
        {
            const float t=-ray.origin.y/ray.direction.y;const auto point=ray.origin+ray.direction*t;
            planeInterior[p]=t>0&&std::abs(point.x)<3.6f&&std::abs(point.z)<3.6f;
        }
        const bool changed=solid.Different(selected,x,y);totalOutline+=changed;
        if(planeInterior[p]){++interiorSamples;interiorChanged+=changed;}
        if(cubePixels[p]){++cubeSamples;cubeChanged+=changed;}
    }
    // 明确覆盖方块轮廓外侧的平面像素，旧算法恰好会在这些位置画出遮挡缺口边线。
    const int radius=int(std::ceil(3*f.scale));
    for(int y=radius;y<f.height-radius;++y)for(int x=radius;x<f.width-radius;++x)
    {
        const auto p=size_t(y)*f.width+x;if(!planeInterior[p]||cubePixels[p])continue;
        if(cubePixels[p-radius]||cubePixels[p+radius]||cubePixels[p-size_t(radius)*f.width]||cubePixels[p+size_t(radius)*f.width])++adjacentSamples;
    }
    std::cout<<prefix<<"plane interior="<<interiorSamples<<", cube samples="<<cubeSamples<<", adjacent plane samples="<<adjacentSamples
        <<", false interior outlines="<<interiorChanged<<", outer outline="<<totalOutline<<'\n';
    Check(interiorSamples>1000&&cubeSamples>1000&&adjacentSamples>80,"plane fixture includes the unselected cube and its occlusion boundary");
    Check(interiorChanged==0&&cubeChanged==0,"unselected cube must not gain an outline when the plane is selected");
    Check(totalOutline>100,"selected plane retains its own outer outline");
    Check(f.session.Select(f.cube),"switch selection to cube");
    const auto cubeSelected=f.Capture(prefix+"cube-over-plane-selected",false,true);size_t cubeOutline=0;
    for(int y=0;y<f.height;++y)for(int x=0;x<f.width;++x)cubeOutline+=solid.Different(cubeSelected,x,y);
    Check(cubeOutline>80,"cube still gains its own outline when explicitly selected");
    Check(f.session.Select(0),"clear selection");const auto cleared=f.Capture(prefix+"plane-selection-cleared",false,true);
    Check(cleared.pixels==solid.pixels,"clearing selection leaves no stale outline");
    Check(f.session.Apply("remove plane",[&](World& world){return world.Destroy(world.FindByStableId(plane));}),"remove plane fixture");
    Check(f.session.Select(f.cube),"restore original selection");
}
void OcclusionAndShape(Fixture& f)
{
    Check(f.session.SetMesh(f.cube,{BuiltinSphereAssetId,{}}),"sphere mesh");
    auto base=f.Capture("sphere",false,false),selected=f.Capture("sphere-selected",false,true);size_t outline=0;
    for(int y=0;y<f.height;++y)for(int x=0;x<f.width;++x)if(base.Different(selected,x,y))
    {++outline;Check(bool(Sphere(f.Ray(x,y),Fixture::center,1.015f)),"selection follows actual sphere instead of its bounding box");}
    Check(outline>80,"sphere has visible outline");
    const Vec3 partial{.6f,1,1.6f},half{.4f,1.5f,.5f};f.Block(partial,half*2);
    base=f.Capture("partial",false,false);selected=f.Capture("partial-selected",false,true);size_t covered=0,remaining=0;
    for(int y=0;y<f.height;++y)for(int x=0;x<f.width;++x)
    {
        const auto ray=f.Ray(x,y);const auto sphere=Sphere(ray,Fixture::center,1),blocker=Box(ray,partial,half*.98f);
        if(sphere&&blocker&&*blocker<*sphere-.02f){++covered;Check(!base.Different(selected,x,y),"selection cannot draw through foreground occluder");}
        remaining+=base.Different(selected,x,y);
    }
    Check(covered>100&&remaining>30,"partially occluded selection retains only visible outline");
    f.Block((f.camera.state.position+Fixture::center)*.5f,{4,4,4});
    base=f.Capture("covered",false,false);selected=f.Capture("covered-selected",false,true);
    Check(base.pixels==selected.pixels,"fully occluded selected mesh leaves no outline");
}
void Handles(Fixture& f)
{
    Check(f.session.SetMesh(f.cube,{BuiltinCubeAssetId,{}}),"restore cube");
    Check(f.session.Apply("remove blocker",[&](World& world){return world.Destroy(world.FindByStableId(f.blocker));}),"remove blocker");
    f.view.Action("W",f.session,f.assets);f.view.Prepare(f.session,f.assets,{0,0,f.width/f.scale,f.height/f.scale},f.frameIndex+1);
    std::vector<rhi::SceneLine> faded,opaque,visible;
    for(auto line:f.view.SceneLines())if(line.occludedAlpha>0){faded.push_back(line);line.occludedAlpha=1;opaque.push_back(line);line.occludedAlpha=0;visible.push_back(line);}
    Check(!faded.empty(),"world-space transform handles");
    const auto base=f.Capture("handle-base",false,false),front=f.Capture("handle-visible",false,false,&visible);
    const auto dim=f.Capture("handle-faded",false,false,&faded),full=f.Capture("handle-opaque",false,false,&opaque);
    size_t fadedPixels=0;
    for(int y=0;y<f.height;++y)for(int x=0;x<f.width;++x)if(!base.Different(front,x,y)&&base.Different(dim,x,y))
    {
        const auto p=(size_t(y)*f.width+x)*3;int a=0,b=0;
        for(int c=0;c<3;++c){a+=std::abs(int(dim.pixels[p+c])-base.pixels[p+c]);b+=std::abs(int(full.pixels[p+c])-base.pixels[p+c]);}
        if(a>0&&a<b)++fadedPixels;
    }
    Check(fadedPixels>10,"occluded handles remain visible at reduced opacity");
    std::cout<<"occluded handle pixels with reduced opacity="<<fadedPixels<<'\n';
}
}
int main(int argc,char** argv)
{
    try
    {
        Microsoft::WRL::ComPtr<ID3D12Debug> debug;Check(SUCCEEDED(D3D12GetDebugInterface(IID_PPV_ARGS(&debug))),"D3D12 debug layer");debug->EnableDebugLayer();
        const auto output=std::filesystem::absolute(argc>1?argv[1]:"out/validation/editor-viewport-depth/gpu");
        std::filesystem::create_directories(output);
        std::ofstream(output/"verification.json")<<"{\"success\":false,\"status\":\"running\"}\n";
        Fixture f(output);
        InfiniteWorldGrid(f);
        for(float scale:{1.f,1.5f,2.f})
        {
            f.scale=scale;const auto prefix="dpi-"+std::to_string(int(scale*100))+"-";
            GridAndSelection(f,prefix);PlaneSelection(f,prefix);
        }
        f.scale=1;OcclusionAndShape(f);Handles(f);
        f.width=720;f.height=400;Check(SetEngineWindowClientSize(f.window,f.width,f.height),"resize window");f.view.Action("Q",f.session,f.assets);
        GridAndSelection(f,"resized-");PlaneSelection(f,"resized-");
        Check(bool(f.renderer.WaitIdle()),"GPU completion");const auto stats=f.renderer.Statistics();
        for(const auto& message:f.renderer.DebugMessages())std::cerr<<message<<'\n';
        Check(stats.debugErrors==0&&stats.debugWarnings==0,"scene overlay has no D3D12 warnings/errors");
        std::ofstream(f.directory/"verification.json")<<"{\"success\":true,\"gridOcclusion\":true,\"meshSilhouette\":true,\"partialOcclusion\":true,\"fullOcclusion\":true,\"planeSelectionNoOccluderOutline\":true,\"selectionSwitchAndClear\":true,\"fadedHandles\":true,\"dpi\":[100,150,200],\"resize\":true,\"debugLayerEnabled\":true,\"debugErrors\":0,\"debugWarnings\":0}\n";
        std::cout<<"Scene overlay depth and visible selection GPU tests passed.\n";return 0;
    }
    catch(const std::exception& error){std::cerr<<"EditorSceneOverlayGpu: "<<error.what()<<'\n';return 1;}
}
