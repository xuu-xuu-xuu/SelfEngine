#include "FirstPersonDemo.h"
#include "ResourceLayer/Assets/Primitives.h"
#include "PlatformLayer/Windows/ExecutablePath.h"
#include "CoreLayer/Localization.h"
#include <algorithm>

namespace se
{
void FirstPersonDemo::Reset()
{character_={};character_.position={0,1,5};view_={character_.position+Vec3{0,.65f,0},0,0};intent_={};input_.Reset();jumpPending_=false;}
bool FirstPersonDemo::Initialize()
{
    world_.Clear();physics_=PhysicsWorld{};clock_=FixedStepClock{};Reset();Localization text;std::string error;
    if(!text.Load(GetExecutableDirectory()/"Resources/Localization"/(locale_+".txt"),error))return false;
    camera_=world_.Create(text.GetText("Sample.FirstPersonCamera"));if(!camera_||!world_.SetCamera(camera_,{Pi/3,.1f,100,true})||!world_.SetTransform(camera_,FreeCameraTransform(view_)))return false;
    const auto box=[&](const char* key,Vec3 center,Vec3 half){const auto entity=world_.Create(text.GetText(key));Transform pose;pose.position=center;pose.scale=half*2;
        if(!entity||!world_.SetTransform(entity,pose)||!world_.SetMesh(entity,{BuiltinCubeAssetId,{}}))return false;
        BodyDesc body;body.mass=0;body.shape.center=center;body.shape.halfExtents=half;body.entityStableId=world_.Get(entity)->stableId;return bool(physics_.CreateBody(body));};
    return box("Sample.FirstPersonFloor",{0,-.5f,0},{20,.5f,20})
        &&box("Sample.FirstPersonWall",{-4,1.5f,-1},{.01f,1.5f,8})&&box("Sample.FirstPersonWall",{4,1.5f,-1},{.01f,1.5f,8})
        &&box("Sample.FirstPersonWall",{0,1.5f,-9},{4,1.5f,.01f})&&box("Sample.FirstPersonTarget",{0,1,-3},{.5f,1,.5f});
}
std::optional<RayHit> FirstPersonDemo::Target()const
{return physics_.Raycast(view_.position,Rotate(FreeCameraTransform(view_).rotation,{0,0,-1}),10);}
ApplicationServices FirstPersonDemo::Services()
{
    ApplicationServices services;services.windowOpened=[this](HWND){return Initialize();};
    services.input=[this](const InputState& state,bool keyboard,bool pointer){
        if(!state.hasFocus)jumpPending_=false;
        if(state.hasFocus&&!keyboard)for(const auto& event:state.keyEvents)if(event.type==KeyEventType::Pressed){if(event.keyCode=='R')Reset();if(event.keyCode==32)jumpPending_=true;}
        intent_=input_.Capture(state,state.hasFocus,keyboard,pointer);
    };
    services.update=[this](double seconds){
        FreeCameraInput look;look.lookPixels=intent_.lookPixels;if(!UpdateFreeCamera(view_,look,seconds))return false;
        const auto direction=Rotate(AxisAngle({0,1,0},view_.yaw),{intent_.move.x,0,-intent_.move.z});bool valid=true;
        clock_.Advance(seconds,[&](double step){if(!valid)return;const bool jump=jumpPending_;jumpPending_=false;valid=character_.Move(physics_,direction,jump,static_cast<float>(step))&&physics_.Step(static_cast<float>(step));});
        view_.position=character_.position+Vec3{0,.65f,0};return valid&&world_.SetTransform(camera_,FreeCameraTransform(view_));
    };
    services.extract=[this](FrameData& frame){BuildFrameData(world_,nullptr,&physics_,frame);
        if(const auto hit=Target()){const auto point=hit->point+hit->normal*.025f;frame.debugLines.push_back({point,point+hit->normal*.25f,{1,.8f,.1f}});
            for(const auto axis:{Vec3{.05f,0,0},Vec3{0,.05f,0},Vec3{0,0,.05f}})frame.debugLines.push_back({point-axis,point+axis,{0,1,1}});}
    };return services;
}
}
