#include "PhysicsDemo.h"
#include "ResourceLayer/Assets/Primitives.h"
#include "PlatformLayer/Windows/ExecutablePath.h"
#include "CoreLayer/Localization.h"
#include <algorithm>
#include <stdexcept>

namespace se
{
bool PhysicsDemo::Initialize()
{
    environment_.Clear();environment_.ConfigureClock(1.0/60,8);physics_=PhysicsWorld{};physics_.SetEnvironmentDriven(true);boxes_.clear();input_.Reset();intent_={};view_={{7,5,10},.61f,-.22f};
    Localization text;std::string error;text.Load(GetExecutableDirectory()/"Resources/Localization"/(locale_+".txt"),error);
    camera_=world_.Create(text.GetText("Sample.PhysicsCamera"));
    if(!camera_||!world_.SetCamera(camera_,{Pi/3,.1f,100,true})||!world_.SetTransform(camera_,FreeCameraTransform(view_)))return false;
    const auto box=[&](std::string name,Vec3 position,Vec3 half,float mass){
        const auto entity=world_.Create(std::move(name));Transform pose;pose.position=position;pose.scale=half*2;
        if(!entity||!world_.SetTransform(entity,pose)||!world_.SetMesh(entity,{BuiltinCubeAssetId,{}}))return false;
        BodyDesc description;description.shape.center=position;description.shape.halfExtents=half;description.mass=mass;description.entityStableId=world_.Get(entity)->stableId;
        const auto body=physics_.CreateBody(description);if(!body)return false;boxes_.push_back({entity,body});return true;
    };
    if(!box(text.GetText("Sample.PhysicsFloor"),{0,-.5f,0},{20,.5f,20},0))return false;
    for(int row=0;row<5;++row)for(int column=0;column<4;++column)
        if(!box(text.GetText("Sample.PhysicsBox")+" "+std::to_string(row*4+column),{column&1?1.25f:-1.25f,.5f+static_cast<float>(row)*1.005f,column&2?1.25f:-1.25f},{.5f,.5f,.5f},1))return false;
    capsule_={};capsule_.position={0,.9f,2};
    TickDescriptor cameraTick;cameraTick.key="physics.camera";cameraTick.component=world_.FindComponent(camera_,ComponentTypes::Camera);cameraTick.stage=TickStage::PostTick;
    cameraTick.callback=[this](const TickContext& context){if(!UpdateFreeCamera(view_,intent_,context.deltaSeconds)||!world_.SetTransform(camera_,FreeCameraTransform(view_)))throw std::runtime_error("Tick.ServiceFailed");};
    return environment_.Ticks().Register(world_,std::move(cameraTick))!=0;
}
bool PhysicsDemo::PushBox(std::size_t index,Vec3 impulse)
{
    if(index>=20||index+1>=boxes_.size())return false;
    const auto body=boxes_[index+1].second;const auto* state=physics_.GetBody(body);
    return state&&physics_.ApplyImpulse(body,impulse,state->description.shape.center);
}
ApplicationServices PhysicsDemo::Services()
{
    ApplicationServices services;services.windowOpened=[this](HWND){return Initialize();};
    services.input=[this](const InputState& state,bool keyboard,bool pointer){
        if(state.hasFocus&&!keyboard&&std::any_of(state.keyEvents.begin(),state.keyEvents.end(),[](const auto& event){return event.type==KeyEventType::Pressed&&event.keyCode=='P';}))PushBox(16,{2,0,0});
        const bool reset=state.hasFocus&&!keyboard&&std::any_of(state.keyEvents.begin(),state.keyEvents.end(),[](const auto& event){return event.type==KeyEventType::Pressed&&event.keyCode=='R';});
        if(reset){view_={{7,5,10},.61f,-.22f};intent_={};input_.Reset();}else intent_=input_.Capture(state,state.hasFocus,keyboard,pointer);
    };
    services.update=[this](double seconds){
        EnvironmentHooks hooks;hooks.physics=[this](double dt){return physics_.StepFromEnvironment(static_cast<float>(dt));};
        hooks.synchronize=[this]{for(const auto& [entity,body]:boxes_){const auto* value=physics_.GetBody(body);if(!value)return false;auto pose=*world_.GetTransform(entity);pose.position=value->description.shape.center;pose.rotation=value->description.shape.rotation;if(!world_.SetTransform(entity,pose,TransformWriteMode::PreserveUnit))return false;}return true;};
        return environment_.RunFrame(seconds,hooks)&&std::none_of(environment_.Ticks().Diagnostics().begin(),environment_.Ticks().Diagnostics().end(),[](const auto& item){return item.key=="Tick.CallbackFailed";});
    };
    services.extract=[this](FrameData& frame){BuildFrameData(world_,nullptr,&physics_,frame);const auto lines=capsule_.DebugLines();frame.debugLines.insert(frame.debugLines.end(),lines.begin(),lines.end());};
    return services;
}
}
