#include "StaticModelDemo.h"
#include "ResourceLayer/Assets/Primitives.h"
#include "PlatformLayer/Windows/ExecutablePath.h"
#include "CoreLayer/Log.h"
#include <algorithm>
#include <stdexcept>

namespace se
{
ApplicationServices StaticModelDemo::Services()
{
    ApplicationServices services;
    services.windowOpened=[this](HWND){
        world_.Clear();router_.Reset();intent_={};state_={{1,0.8f,4},0,0};
        if(format_!="fbx"&&format_!="glb")return false;
        const auto directory=GetExecutableDirectory()/"Resources/Models";
        assets_=std::make_unique<AssetStore>(directory,GetExecutableDirectory()/"Cache/ModelSample");
        AssetId id;std::vector<AssetDiagnostic> diagnostics;
        if(!assets_->RegisterSource("Hierarchy."+format_,{},id,diagnostics)||!assets_->LoadModel(id,diagnostics))
        {
            for(const auto& diagnostic:diagnostics)Log(LogLevel::Error,diagnostic.key+": "+diagnostic.detail);
            return false;
        }
        camera_=world_.Create("Camera",1);
        const auto parent=world_.Create("ModelParent",2),model=world_.Create("AsymmetricModel",3),reference=world_.Create("OneMeterCube",4);
        return camera_&&parent&&model&&reference&&world_.SetCamera(camera_,{Pi/3,0.1f,1000,true})
            &&world_.SetTransform(camera_,FreeCameraTransform(state_))&&world_.SetParent(model,parent)
            &&world_.SetMesh(model,{id,{}})&&world_.SetMesh(reference,{BuiltinCubeAssetId,{}})
            &&world_.SetTransform(reference,{{-0.1f,0.5f,0},{},{1,1,1}});
    };
    services.input=[this](const InputState& input,bool keyboard,bool pointer){
        const bool reset=input.hasFocus&&!keyboard&&std::any_of(input.keyEvents.begin(),input.keyEvents.end(),[](const auto& event){
            return event.type==KeyEventType::Pressed&&event.keyCode=='R';});
        if(reset){state_={{1,0.8f,4},0,0};intent_={};router_.Reset();}
        else intent_=router_.Capture(input,input.hasFocus,keyboard,pointer);
    };
    services.update=[this](double dt){return UpdateFreeCamera(state_,intent_,dt)&&world_.SetTransform(camera_,FreeCameraTransform(state_));};
    services.extract=[this](FrameData& data){BuildFrameData(world_,nullptr,nullptr,data);data.assets=assets_.get();};
    services.windowClosing=[this]{intent_={};router_.Reset();assets_.reset();};
    return services;
}
}
