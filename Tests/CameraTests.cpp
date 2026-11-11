#include "FunctionLayer/Runtime/Camera.h"
#include "FunctionLayer/Application/FrameData.h"
#include "FunctionLayer/Application/FreeCameraInput.h"
#include "FunctionLayer/Application/SandboxApp.h"
#include "FunctionLayer/Rendering/Renderer.h"
#include "Camera/CameraDemo.h"
#include <Windows.h>
#include <algorithm>
#include <iostream>
#include <limits>
#include <stdexcept>

namespace
{
using namespace se;
void Check(bool value, const char* message) { if (!value) throw std::runtime_error(message); }
bool Near(float a, float b, float tolerance = 1e-5f) { return std::abs(a-b) <= tolerance; }
bool Near(Vec3 a, Vec3 b, float tolerance = 1e-5f)
{ return Near(a.x,b.x,tolerance) && Near(a.y,b.y,tolerance) && Near(a.z,b.z,tolerance); }
void Identity(const Mat4& matrix)
{
    for (std::size_t row=0; row<4; ++row)
        for (std::size_t column=0; column<4; ++column)
            Check(Near(matrix(row,column), row==column ? 1.0f : 0.0f, 1e-4f), "matrix inverse round trip");
}
CameraSnapshot Snapshot(CameraInstance instance = {1, {}, {Pi/2,1,11,true}, true},
    CameraViewport viewport = {100,50,800,400})
{
    CameraSnapshot result;
    CameraError error;
    Check(BuildCameraSnapshot(instance,viewport,42,result,error), "build valid camera snapshot");
    Check(error==CameraError::None && result.entity==instance.entity && result.frameIndex==42, "camera identity and version");
    return result;
}

void ProjectionTests()
{
    const auto camera = Snapshot();
    Check(Near(*ProjectWorldPoint(camera,{0,0,-1}),{500,250,0}), "near plane maps to zero at viewport center");
    Check(Near(*ProjectWorldPoint(camera,{0,0,-11}),{500,250,1}), "far plane maps to one");
    const auto right = ProjectWorldPoint(camera,{2,0,-2});
    const auto above = ProjectWorldPoint(camera,{0,1,-2});
    Check(right && Near(right->x,700) && above && Near(above->y,150), "right/up projection and top-left viewport");
    Check(!ProjectWorldPoint(camera,{0,0,1}) && !ProjectWorldPoint(camera,{0,0,0}), "behind/eye plane rejected");
    Identity(camera.projection * camera.inverseProjection);
    Identity(camera.viewProjection * camera.inverseViewProjection);
    const auto ortho = OrthographicRH(8,4,1,11);
    Check(ortho && Near(TransformPoint(*ortho,{4,2,-1}),{1,1,0})
        && Near(TransformPoint(*ortho,{-4,-2,-11}),{-1,-1,1}), "orthographic bounds and ordinary depth");
    const float nan = std::numeric_limits<float>::quiet_NaN();
    Check(!PerspectiveRH(0,1,1,11) && !PerspectiveRH(Pi,1,1,11) && !PerspectiveRH(Pi/2,0,1,11)
        && !PerspectiveRH(Pi/2,1,0,11) && !PerspectiveRH(Pi/2,1,2,1)
        && !PerspectiveRH(nan,1,1,11) && !OrthographicRH(0,4,1,11)
        && !OrthographicRH(8,4,1,1), "invalid lens rejected");

    const Mat4 world = Translation({4,2,5}) * Rotation(AxisAngle({0,1,0},Pi/2));
    const auto moved = Snapshot({9,world,{Pi/2,1,11,true},true});
    const auto projected = ProjectWorldPoint(moved,TransformPoint(world,{0,0,-2}));
    Check(projected && Near(projected->x,500,1e-4f) && Near(projected->y,250,1e-4f), "rigid translated/rotated camera");
    Identity(moved.view * world);
    Identity(moved.viewProjection * moved.inverseViewProjection);
    Check(Near(moved.position,{4,2,5}), "snapshot contains world camera position");

    const Mat4 scaledObject = Translation({3,4,5}) * Rotation(AxisAngle({0,1,0},Pi/4)) * Scaling({2,3,4});
    const auto normal = TransformNormal(scaledObject,{1,-1,0});
    Check(normal && Near(Length(*normal),1) && Near(Dot(*normal,TransformVector(scaledObject,{1,1,0})),0),
        "nonuniform object normals use inverse transpose");
    Check(!TransformNormal(Scaling({1,0,1}),{0,1,0}), "singular normal matrix fails");
}

void CameraValidationTests()
{
    CameraInstance instance{1,{}, {Pi/2,1,11,true},true};
    CameraSnapshot output = Snapshot();
    const auto unchanged = output.viewProjection.values;
    auto reject = [&](const CameraInstance& bad, CameraViewport viewport, CameraError expected) {
        CameraError error;
        Check(!BuildCameraSnapshot(bad,viewport,99,output,error) && error==expected
            && output.frameIndex==42 && output.viewProjection.values==unchanged, "camera failure preserves prior output");
    };
    auto bad=instance; bad.world=Scaling({2,1,1}); reject(bad,{0,0,800,400},CameraError::InvalidTransform);
    bad.world=Scaling({-1,1,1}); reject(bad,{0,0,800,400},CameraError::InvalidTransform);
    bad.world={}; bad.world(0,1)=0.25f; reject(bad,{0,0,800,400},CameraError::InvalidTransform);
    bad.world={}; bad.world(3,0)=0.25f; reject(bad,{0,0,800,400},CameraError::InvalidTransform);
    bad.world=Scaling({0,1,1}); reject(bad,{0,0,800,400},CameraError::InvalidTransform);
    bad.world={}; bad.world(0,0)=std::numeric_limits<float>::infinity(); reject(bad,{0,0,800,400},CameraError::InvalidTransform);
    bad=instance; bad.rigidHierarchy=false; reject(bad,{0,0,800,400},CameraError::InvalidTransform);
    bad=instance; bad.camera.nearPlane=11; reject(bad,{0,0,800,400},CameraError::InvalidProjection);
    bad=instance; bad.entity=0; reject(bad,{0,0,800,400},CameraError::InvalidIdentity);
    reject(instance,{0,0,0,400},CameraError::InvalidViewport);
    reject(instance,{0,0,800,-1},CameraError::InvalidViewport);
    Check(CameraErrorKey(CameraError::InvalidTransform)=="Camera.InvalidTransform", "stable error key");
}

void SelectionTests()
{
    std::vector<CameraInstance> candidates{ {20,{}, {Pi/2,1,11,true},true}, {4,{}, {Pi/2,1,11,true},true},
        {10,{}, {Pi/2,1,11,false},true} };
    const CameraViewport viewport{0,0,800,400};
    auto selected=SelectCamera(candidates,viewport,7);
    Check(selected.snapshot && selected.snapshot->entity==4 && selected.multiplePrimary, "stable smallest primary");
    std::reverse(candidates.begin(),candidates.end());
    selected=SelectCamera(candidates,viewport,8);
    Check(selected.snapshot && selected.snapshot->entity==4 && selected.snapshot->frameIndex==8, "candidate order independence");
    selected=SelectCamera(candidates,viewport,9,10);
    Check(selected.snapshot && selected.snapshot->entity==10 && !selected.multiplePrimary, "explicit non-primary binding");
    selected=SelectCamera(candidates,viewport,9,99);
    Check(!selected.snapshot && selected.error==CameraError::BindingMissing, "missing explicit binding does not fall back");
    candidates[1].rigidHierarchy=false; // 当前顺序为 10、4、20。
    selected=SelectCamera(candidates,viewport,10);
    Check(selected.snapshot && selected.snapshot->entity==20 && selected.rejectedPrimaryCount==1, "reject bad lower-ID primary");
    selected=SelectCamera(candidates,viewport,10,4);
    Check(!selected.snapshot && selected.error==CameraError::InvalidTransform, "invalid explicit binding does not fall back");
    candidates[2].rigidHierarchy=false;
    selected=SelectCamera(candidates,viewport,10);
    Check(!selected.snapshot && selected.error==CameraError::InvalidTransform && selected.rejectedPrimaryCount==2,
        "all primary candidates invalid");
    candidates={candidates[0]};
    Check(SelectCamera(candidates,viewport,10).error==CameraError::NoPrimary, "no implicit non-primary fallback");
    candidates.push_back(candidates[0]);
    Check(SelectCamera(candidates,viewport,10).error==CameraError::InvalidIdentity, "duplicate candidate IDs rejected");
    candidates[0].entity=0;
    Check(SelectCamera(candidates,viewport,10).error==CameraError::InvalidIdentity, "zero identity rejected");
}

void HierarchyAndRayTests()
{
    World world;
    const auto parent=world.Create("parent",100), child=world.Create("camera",101);
    Transform parentPose{{4,2,5},AxisAngle({0,1,0},0.4f),{1,1,1}};
    Transform local{{1,0,2},{},{1,1,1}};
    Check(world.SetTransform(parent,parentPose) && world.SetTransform(child,local)
        && world.SetParent(child,parent) && world.SetCamera(child,{Pi/2,1,11,true}), "camera hierarchy setup");
    FrameData frame;
    BuildFrameData(world,nullptr,nullptr,frame);
    Check(frame.cameras.size()==1 && frame.cameras[0].rigidHierarchy, "rigid ancestors extracted as values");
    const auto snapshot=Snapshot(frame.cameras[0]);
    const Vec3 point=TransformPoint(frame.cameras[0].world,{1,0.5f,-3});
    const auto pixel=ProjectWorldPoint(snapshot,point);
    const auto ray=pixel ? ViewportPointToRay(snapshot,{pixel->x,pixel->y}) : std::nullopt;
    Check(ray && Near(Length(ray->direction),1) && Length(Cross(point-ray->origin,ray->direction))<1e-4f,
        "projection and picking ray round trip with parent rotation");
    const auto originCamera=Snapshot();
    const auto center=ViewportPointToRay(originCamera,{500,250});
    Check(center && Near(center->origin,{0,0,-1}) && Near(center->direction,{0,0,-1}), "center ray begins on near plane");
    Check(!ViewportPointToRay(originCamera,{99,250}) && !ViewportPointToRay(originCamera,{500,451}), "outside viewport rejected");
    Check(ViewportPointToRay(originCamera,{99,250},true).has_value()&&ViewportPointToRay(originCamera,{500,451},true).has_value(),"captured manipulation may continue outside viewport");
    const auto farCamera=Snapshot({1,{}, {Pi/2,0.1f,1e9f,true},true});
    Check(ViewportPointToRay(farCamera,{500,250}).has_value(), "ray avoids far-end float cancellation");
    Check(world.SetEnabled(parent,false), "disable camera ancestor");
    BuildFrameData(world,nullptr,nullptr,frame);
    Check(frame.cameras.empty(), "disabled ancestor removes camera candidate");
    Check(world.SetEnabled(parent,true), "restore camera ancestor");
    parentPose.scale={2,2,2}; local.scale={0.5f,0.5f,0.5f};
    Check(world.SetTransform(parent,parentPose) && world.SetTransform(child,local), "cancelling scale setup");
    BuildFrameData(world,nullptr,nullptr,frame);
    Check(frame.cameras.size()==1 && !frame.cameras[0].rigidHierarchy
        && InverseRigid(frame.cameras[0].world).has_value(), "ancestor scale cannot hide in rigid final product");
    Check(!SelectCamera(frame.cameras,{100,50,800,400},13).snapshot, "reject cancelling parent scales");
    parentPose.scale={1,1,1}; local.scale={-1,-1,1};
    Check(world.SetTransform(parent,parentPose) && world.SetTransform(child,local), "double reflection setup");
    BuildFrameData(world,nullptr,nullptr,frame);
    Check(!frame.cameras[0].rigidHierarchy && !SelectCamera(frame.cameras,{100,50,800,400},14).snapshot,
        "reject negative scales even when determinant is positive");
    Check(world.Destroy(parent), "destroy camera world objects");
    const auto after=ProjectWorldPoint(snapshot,point);
    Check(after && Near(*after,*pixel), "camera snapshot survives World mutation");
}

void FreeCameraTests()
{
    for (const int frequency : {30,60,144})
    {
        FreeCameraState state;
        for(int frame=0;frame<frequency;++frame)
            Check(UpdateFreeCamera(state,{{0,0,1},{},false},1.0/frequency), "step free camera");
        Check(Near(state.position,{0,0,-3},1e-4f), "movement speed independent of frame rate");
    }
    FreeCameraState diagonal,fast;
    Check(UpdateFreeCamera(diagonal,{{1,1,1},{},false},1) && Near(Length(diagonal.position),3), "diagonal movement normalized");
    Check(UpdateFreeCamera(fast,{{0,0,1},{},true},1) && Near(fast.position.z,-12), "Shift speed multiplier");
    FreeCameraState whole,split;
    Check(UpdateFreeCamera(whole,{{},{100,-50},false},0), "zero-dt camera look");
    Check(UpdateFreeCamera(split,{{},{50,-25},false},1.0/30)
        && UpdateFreeCamera(split,{{},{50,-25},false},1.0/144), "split mouse look");
    Check(Near(whole.yaw,-0.3f) && Near(whole.pitch,0.15f)
        && Near(whole.yaw,split.yaw) && Near(whole.pitch,split.pitch), "mouse pixels do not multiply dt");
    const auto forward=Rotate(FreeCameraTransform(whole).rotation,{0,0,-1});
    Check(forward.x>0 && forward.y>0 && forward.z<0, "right/up mouse turns camera right/up");
    const auto old=whole;
    Check(!UpdateFreeCamera(whole,{},-1) && Near(whole.position,old.position)
        && whole.yaw==old.yaw && whole.pitch==old.pitch, "negative dt fails without mutation");
    const float nan=std::numeric_limits<float>::quiet_NaN();
    Check(!UpdateFreeCamera(whole,{{nan,0,0},{},false},0.1)
        && !UpdateFreeCamera(whole,{},std::numeric_limits<double>::infinity())
        && !UpdateFreeCamera(whole,{},0.1,{0,4,0.003f}), "invalid control inputs rejected");
    Check(UpdateFreeCamera(whole,{{},{0,-1e9f},false},0) && whole.pitch==FreeCameraPitchLimit
        && IsFinite(FreeCameraTransform(whole).Matrix()), "pitch limit avoids flip/singular look");
}

void InputRouterTests()
{
    FreeCameraInputRouter router;
    InputState input;
    input.keysDown['W']=input.keysDown['D']=input.keysDown['E']=input.keysDown[0x10]=true;
    input.mouseButtons[1]=true;
    input.pointerEvents={{PointerEventType::Down,10,20,1,0},{PointerEventType::Move,25,10,0,0}};
    auto intent=router.Capture(input,true,false,false);
    Check(Near(intent.move,{1,1,1}) && intent.fast && Near(intent.lookPixels.x,15) && Near(intent.lookPixels.y,-10),
        "route movement, fast and ordered right drag");
    Check(input.pointerEvents.size()==2 && input.keysDown['W'], "router does not mutate input");
    input.ClearEvents();
    intent=router.Capture(input,true,true,false);
    Check(Near(intent.move,{}) && !intent.fast, "UI consumes keyboard only");
    input.pointerEvents={{PointerEventType::Move,500,500,0,0}};
    intent=router.Capture(input,true,false,true);
    Check(intent.lookPixels.x==0 && Near(intent.move,{1,1,1}), "UI consumes pointer without consuming keyboard");
    intent=router.Capture(input,true,false,false);
    Check(intent.lookPixels.x==0 && intent.lookPixels.y==0, "UI release does not inherit old drag");
    input.pointerEvents={{PointerEventType::Down,500,500,1,0},{PointerEventType::Move,503,506,0,0}};
    intent=router.Capture(input,true,false,false);
    Check(intent.lookPixels.x==3 && intent.lookPixels.y==6, "new drag has no position jump");
    input.pointerEvents={{PointerEventType::Cancel,0,0,0,0},{PointerEventType::Move,900,900,0,0}};
    Check(router.Capture(input,true,false,false).lookPixels.x==0, "capture cancellation stops look");
    input.pointerEvents={{PointerEventType::Down,0,0,1,0}};
    router.Capture(input,true,false,false);
    input.pointerEvents={{PointerEventType::Move,100,100,0,0}};
    intent=router.Capture(input,false,false,false);
    Check(Near(intent.move,{}) && intent.lookPixels.x==0, "focus loss cancels movement and drag");
    Check(router.Capture(input,true,false,false).lookPixels.x==0, "focus return requires fresh drag");
    input.pointerEvents={{PointerEventType::Down,0,0,1,0},{PointerEventType::Move,30,30,0,0}};
    input.keyEvents={{KeyEventType::Pressed,0x1b}};
    Check(router.Capture(input,true,false,false).lookPixels.x==0, "Escape cancels same-batch look");
}

class CameraProbe final : public Renderer
{
public:
    CameraDemo& demo;
    bool shutdownWithWindow=false;
    unsigned frames=0, stops=0;
    HWND window=nullptr;
    explicit CameraProbe(CameraDemo& value):demo(value) {}
    bool Initialize(const RenderSurface& surface,const Localization&) override
    {
        window=surface.window;
        SendMessageW(window,WM_SETFOCUS,0,0);
        SendMessageW(window,WM_KEYDOWN,'W',0);
        return true;
    }
    RenderResult Render(const RenderFrame& frame,const Localization&) override
    {
        ++frames;
        Check(frame.data && frame.data->cameras.size()==1, "sample delivers actual World camera");
        const auto selected=SelectCamera(frame.data->cameras,{0,0,float(frame.surface.width),float(frame.surface.height)},frame.updateIndex);
        Check(selected.snapshot && selected.snapshot->frameIndex==frame.updateIndex
            && Near(selected.snapshot->position,demo.State().position), "sample updates camera before extraction");
        const auto& state=demo.State();
        if(frames<8) Check(Near(state.position,{0,0,1.95f},1e-4f), "sample movement stops for focus/UI capture");
        if(frames<=5) Check(state.yaw==0 && state.pitch==0, "blocked/stale pointer does not look");
        if(frames==6 || frames==7)
        {
            std::cout<<"Win32 frame="<<frames<<" yaw="<<state.yaw<<" pitch="<<state.pitch<<'\n';
            Check(Near(state.yaw,-0.03f) && Near(state.pitch,0.03f), "fresh Win32 drag and Escape cleanup");
        }
        if(frames==8) Check(Near(state.position,{0,0,2}) && state.yaw==0 && state.pitch==0, "sample reset key");
        return RenderResult::Success;
    }
    void Shutdown() noexcept override { ++stops; shutdownWithWindow=IsWindow(window)!=FALSE; }
};

void WindowIntegrationTests()
{
    CameraDemo demo;
    CameraProbe renderer(demo);
    auto services=demo.Services();
    const auto update=services.update;
    const auto capture=services.input;
    services.input=[&](const InputState& input,bool keyboard,bool pointer) {
        // 必须在消费前送消息；Render 之后注入会被宿主本帧 ClearEvents 清掉。
        const auto window=renderer.window;
        if(renderer.frames==1)
        { SendMessageW(window,WM_KILLFOCUS,0,0); SendMessageW(window,WM_KEYDOWN,'W',0); }
        if(renderer.frames==2)
        {
            SendMessageW(window,WM_SETFOCUS,0,0); SendMessageW(window,WM_KEYUP,'W',0);
            SendMessageW(window,WM_KEYDOWN,'W',0);
        }
        if(renderer.frames==3)
        {
            SendMessageW(window,WM_KEYUP,'W',0);
            SendMessageW(window,WM_RBUTTONDOWN,MK_RBUTTON,MAKELPARAM(10,10));
            SendMessageW(window,WM_MOUSEMOVE,MK_RBUTTON,MAKELPARAM(30,10));
        }
        if(renderer.frames==4)SendMessageW(window,WM_MOUSEMOVE,MK_RBUTTON,MAKELPARAM(1000,1000));
        if(renderer.frames==5)
        {
            SendMessageW(window,WM_RBUTTONUP,0,MAKELPARAM(1000,1000));
            SendMessageW(window,WM_RBUTTONDOWN,MK_RBUTTON,MAKELPARAM(1000,1000));
            SendMessageW(window,WM_MOUSEMOVE,MK_RBUTTON,MAKELPARAM(1010,990));
        }
        if(renderer.frames==6)
        { SendMessageW(window,WM_KEYDOWN,VK_ESCAPE,0); SendMessageW(window,WM_MOUSEMOVE,0,MAKELPARAM(2000,2000)); }
        if(renderer.frames==7)SendMessageW(window,WM_KEYDOWN,'R',0);
        capture(input,keyboard,pointer);
    };
    services.update=[update](double) { return update(1.0/60); };
    services.captureKeyboard=[&] { return renderer.frames==2; };
    services.capturePointer=[&] { return renderer.frames==3; };
    services.maximumFrames=8;
    Check(RunSandboxApp("en-US",renderer,false,&services)==0 && renderer.frames==8
        && renderer.stops==1 && renderer.shutdownWithWindow, "hidden Win32 camera host integration");
}
}

int main()
{
    try
    {
        ProjectionTests(); CameraValidationTests(); SelectionTests(); HierarchyAndRayTests();
        FreeCameraTests(); InputRouterTests(); WindowIntegrationTests();
        std::cout << "CameraTests passed (projection, rigid hierarchy, selection, rays, free camera, Win32 routing)\n";
        return 0;
    }
    catch(const std::exception& error) { std::cerr << "CameraTests failed: " << error.what() << '\n'; return 1; }
}
