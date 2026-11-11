#include "ShadowsDemo.h"
#include "ResourceLayer/Assets/Primitives.h"
#include "PlatformLayer/Windows/ExecutablePath.h"
#include "CoreLayer/Localization.h"
#include "CoreLayer/Log.h"

namespace se
{
ApplicationServices ShadowsDemo::Services()
{
    ApplicationServices services;
    services.windowOpened=[this](HWND){
        world_.Clear();router_.Reset();intent_={};state_=entryCamera_;shadow_={};angle_=0;sunAngle_=0.6f;paused_=false;biasStage_=1;
        shadow_.halfExtent=6;shadow_.lightDistance=12;
        const auto root=GetExecutableDirectory()/"Cache/ShadowsSample";std::error_code error;
        std::filesystem::create_directories(root/"Source",error);if(error)return false;
        assets_=std::make_unique<AssetStore>(root/"Source",root/"Derived");
        Localization text;std::string loadError;if(!text.Load(GetExecutableDirectory()/"Resources/Localization"/(locale_+".txt"),loadError))return false;
        std::vector<AssetDiagnostic> diagnostics;AssetId groundMaterial,cubeMaterial,sphereMaterial;
        MaterialDocument material;material.name="Ground";material.baseColor={0.55f,0.6f,0.65f,1};
        bool saved=assets_->SaveMaterialSource("ground.material.json",material,groundMaterial,diagnostics);
        material.name="Caster";material.baseColor={0.8f,0.15f,0.045f,1};material.roughness=0.5f;
        saved=saved&&assets_->SaveMaterialSource("caster.material.json",material,cubeMaterial,diagnostics);
        material.name="Sphere";material.baseColor={0.15f,0.55f,0.25f,1};material.metallic=0.2f;material.roughness=0.4f;
        saved=saved&&assets_->SaveMaterialSource("sphere.material.json",material,sphereMaterial,diagnostics);
        if(!saved){for(const auto& d:diagnostics)Log(LogLevel::Error,text.GetText(d.key)+": "+d.detail);return false;}
        camera_=world_.Create("Camera",1);light_=world_.Create("Sun",2);const auto floor=world_.Create("Ground",3);
        cube_=world_.Create("MovingCaster",4);const auto sphere=world_.Create("ElevatedSphere",5),fill=world_.Create("PointFill",6);
        return camera_&&light_&&floor&&cube_&&sphere&&fill&&world_.SetCamera(camera_,{Pi/3,0.1f,100,true})
            &&world_.SetTransform(camera_,FreeCameraTransform(state_))&&world_.SetLight(light_,{{1,0.98f,0.9f},3})
            &&world_.SetMesh(floor,{BuiltinCubeAssetId,{groundMaterial},false,true})&&world_.SetTransform(floor,{{0,-0.05f,0},{},{10,0.1f,10}})
            &&world_.SetMesh(cube_,{BuiltinCubeAssetId,{cubeMaterial},true,true})
            &&world_.SetMesh(sphere,{BuiltinSphereAssetId,{sphereMaterial},true,true})&&world_.SetTransform(sphere,{{2.5f,2,-0.5f},{},{1.5f,1.5f,1.5f}})
            &&world_.SetLight(fill,{{0.2f,0.35f,1},4,LightType::Point,8})&&world_.SetTransform(fill,{{-3,3,2},{},{1,1,1}});
    };
    services.input=[this](const InputState& input,bool keyboard,bool pointer){
        if(input.hasFocus&&!keyboard)for(const auto& event:input.keyEvents)if(event.type==KeyEventType::Pressed)
        {
            if(event.keyCode=='R'){state_=entryCamera_;intent_={};router_.Reset();}
            if(event.keyCode=='H')shadow_.enabled=!shadow_.enabled;
            if(event.keyCode==VK_F2)shadow_.preview=!shadow_.preview;
            if(event.keyCode==VK_F3)shadow_.pcf=!shadow_.pcf;
            if(event.keyCode=='1')shadow_.resolution=512;if(event.keyCode=='2')shadow_.resolution=1024;if(event.keyCode=='3')shadow_.resolution=2048;
            if(event.keyCode=='B'){biasStage_=(biasStage_+1)%3;shadow_.depthBias=biasStage_==0?0:biasStage_==1?0.0008f:0.003f;}
            if(event.keyCode==VK_LEFT)sunAngle_=std::remainder(sunAngle_-0.1f,2*Pi);
            if(event.keyCode==VK_RIGHT)sunAngle_=std::remainder(sunAngle_+0.1f,2*Pi);
            if(event.keyCode==VK_SPACE)paused_=!paused_;
        }
        intent_=router_.Capture(input,input.hasFocus,keyboard,pointer);
    };
    services.update=[this](double dt){
        if(!UpdateFreeCamera(state_,intent_,dt)||!world_.SetTransform(camera_,FreeCameraTransform(state_)))return false;
        if(!paused_)angle_=std::fmod(angle_+dt*0.5,2.0*Pi);
        return world_.SetTransform(cube_,{{std::sin(static_cast<float>(angle_))*1.5f,0.5f,0},AxisAngle({0,1,0},static_cast<float>(angle_)*0.8f),{1,1,1}})
            &&world_.SetTransform(light_,{{},AxisAngle({0,1,0},sunAngle_)*AxisAngle({1,0,0},-0.9f),{1,1,1}});
    };
    services.extract=[this](FrameData& data){BuildFrameData(world_,nullptr,nullptr,data);data.assets=assets_.get();data.shadow=shadow_;};
    services.windowClosing=[this]{intent_={};router_.Reset();assets_.reset();};return services;
}
}
