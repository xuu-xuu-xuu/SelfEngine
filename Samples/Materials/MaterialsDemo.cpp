#include "MaterialsDemo.h"
#include "ResourceLayer/Assets/Primitives.h"
#include "PlatformLayer/Windows/ExecutablePath.h"
#include "CoreLayer/Localization.h"
#include "CoreLayer/Log.h"
#include <algorithm>
#include <cmath>
#include <Windows.h>

namespace se
{
ApplicationServices MaterialsDemo::Services()
{
    ApplicationServices services;
    services.windowOpened=[this](HWND){
        world_.Clear();router_.Reset();intent_={};state_=entryCamera_;
        wchar_t routeValue[8]{};
        const auto routeLength=GetEnvironmentVariableW(L"SELFENGINE_LOD_ROUTE",routeValue,8);
        lodRoute_=routeLength>0&&!(routeLength==1&&routeValue[0]==L'0');routeFrame_=0;
        if(lodRoute_)state_={Vec3{0,2.5f,9},0,0};
        angle_=0;paused_=false;normalEnabled_=true;normalIndex_=2;pointsEnabled_=directionalEnabled_=true;exposure_=1;
        environmentIndex_=environmentKind_=="contrast"?1:0;environmentYaw_=0;environmentIntensity_=1;
        if(!environmentKind_.empty())directionalEnabled_=false;
        postSettings_={};
        const auto directory=GetExecutableDirectory()/"Cache/MaterialsSample";
        const auto sources=directory/"Source";std::error_code error;
        std::filesystem::create_directories(sources,error);if(error)return false;
        std::filesystem::copy_file(GetExecutableDirectory()/"Resources/Materials/normal.png",sources/"normal.png",
            std::filesystem::copy_options::overwrite_existing,error);if(error)return false;
        assets_=std::make_unique<AssetStore>(sources,directory/"Derived");
        Localization text;std::string loadError;
        if(!text.Load(GetExecutableDirectory()/"Resources/Localization"/(locale_+".txt"),loadError))return false;
        const auto report=[&](const auto& diagnostics){for(const auto& d:diagnostics)
            Log(d.warning?LogLevel::Warning:LogLevel::Error,text.GetText(d.key)+": "+d.detail);};
        std::vector<AssetDiagnostic> diagnostics;AssetId normal;
        if(!assets_->RegisterSource("normal.png",{},normal,diagnostics)){report(diagnostics);return false;}
        if(!environmentKind_.empty())for(size_t index=0;index<environments_.size();++index)
        {
            const auto name=index?"contrast.seenv":"soft.seenv";
            std::filesystem::copy_file(GetExecutableDirectory()/"Resources/Environments"/name,sources/name,std::filesystem::copy_options::overwrite_existing,error);
            if(error||!assets_->RegisterSource(name,{},environments_[index],diagnostics)){report(diagnostics);return false;}
        }
        camera_=world_.Create("Camera",1);directional_=world_.Create("DirectionalLight",2);const auto light=directional_;
        if(!camera_||!light||!world_.SetCamera(camera_,{Pi/3,0.1f,lodRoute_?2000.0f:100,true})
            ||!world_.SetTransform(camera_,FreeCameraTransform(state_))||!world_.SetLight(light,{{1,0.96f,0.9f},2})
            ||!world_.SetTransform(light,{{},AxisAngle({0,1,0},0.4f)*AxisAngle({1,0,0},-0.5f),{1,1,1}}))return false;
        for(int row=0;row<5;++row)for(int column=0;column<5;++column)
        {
            MaterialDocument material;material.name="MetalRough";material.baseColor={0.8f,0.24f,0.07f,1};
            material.metallic=float(row)/4;material.roughness=std::max(0.045f,float(column)/4);
            AssetId id;const auto name="sphere-"+std::to_string(row)+"-"+std::to_string(column)+".material.json";
            if(!assets_->SaveMaterialSource(sources/name,material,id,diagnostics)){report(diagnostics);return false;}
            const auto entity=world_.Create("MaterialSphere",static_cast<StableEntityId>(10+row*5+column));
            if(!entity||!world_.SetMesh(entity,{BuiltinSphereAssetId,{id}})
                ||!world_.SetTransform(entity,{{(float(column)-2)*1.35f,(2-float(row))*1.25f,0},{},{1.05f,1.05f,1.05f}}))return false;
        }
        for(std::size_t index=0;index<normalMaterials_.size();++index)
        {
            MaterialDocument material;material.name="NormalComparison";material.baseColor={0.32f,0.65f,0.9f,1};material.roughness=0.45f;
            if(index)material.normalTexture=normal;
            material.normalScale=index==1?0.5f:index==3?2.0f:1.0f;
            if(!assets_->SaveMaterialSource(sources/("normal-"+std::to_string(index)+".material.json"),material,normalMaterials_[index],diagnostics))
            {report(diagnostics);return false;}
        }
        for(std::size_t index=0;index<boxes_.size();++index)
        {
            boxes_[index]=world_.Create("NormalCube",static_cast<StableEntityId>(40+index));
            if(!boxes_[index]||!world_.SetMesh(boxes_[index],{BuiltinCubeAssetId,{normalMaterials_[index==0?0:normalIndex_]}}))return false;
        }
        if(lighting_)for(std::size_t index=0;index<points_.size();++index)
        {
            points_[index]=world_.Create("PointLight",static_cast<StableEntityId>(100+index));
            if(!points_[index])return false;
        }
        return true;
    };
    services.input=[this](const InputState& input,bool keyboard,bool pointer){
        if(input.hasFocus&&!keyboard)for(const auto& event:input.keyEvents)if(event.type==KeyEventType::Pressed)
        {
            if(event.keyCode=='R'){state_=entryCamera_;intent_={};router_.Reset();}
            if(event.keyCode=='N')normalEnabled_=!normalEnabled_;
            if(event.keyCode==VK_SPACE)paused_=!paused_;
            if(post_&&event.keyCode=='B')postSettings_.bloom=!postSettings_.bloom;
            if(post_&&event.keyCode=='T')postSettings_.toneMapping=!postSettings_.toneMapping;
            if(post_&&event.keyCode=='F')postSettings_.fxaa=!postSettings_.fxaa;
            if(lighting_&&event.keyCode=='P')pointsEnabled_=!pointsEnabled_;
            if((lighting_||!environmentKind_.empty())&&event.keyCode=='L')directionalEnabled_=!directionalEnabled_;
            if((lighting_||!environmentKind_.empty())&&(event.keyCode==VK_OEM_PLUS||event.keyCode==VK_ADD))exposure_=std::min(8.0f,exposure_*1.25f);
            if((lighting_||!environmentKind_.empty())&&(event.keyCode==VK_OEM_MINUS||event.keyCode==VK_SUBTRACT))exposure_=std::max(0.0625f,exposure_/1.25f);
            if(!environmentKind_.empty())
            {
                if(event.keyCode=='M')environmentIndex_=1-environmentIndex_;
                if(event.keyCode==VK_LEFT)environmentYaw_-=Pi/12;
                if(event.keyCode==VK_RIGHT)environmentYaw_+=Pi/12;
                if(event.keyCode=='I')environmentIntensity_=std::min(8.0f,environmentIntensity_*1.25f);
                if(event.keyCode=='O')environmentIntensity_=std::max(0.0625f,environmentIntensity_/1.25f);
            }
            if(event.keyCode>='1'&&event.keyCode<='3')normalIndex_=static_cast<std::size_t>(event.keyCode-'1'+1);
        }
        intent_=router_.Capture(input,input.hasFocus,keyboard,pointer);
    };
    services.update=[this](double dt){
        if(lodRoute_)
        {
            // 10 个距离档推远（9 m→321 m，几何级数），再按相反顺序拉回；每档 30 帧。
            constexpr std::uint64_t routeSteps=10,routeHoldFrames=30;
            const auto step=routeFrame_/routeHoldFrames;
            const auto sequenceIndex=step<routeSteps?step:2*routeSteps-2-step;
            const float parameter=static_cast<float>(sequenceIndex)/float(routeSteps-1);
            const float distance=9.0f*std::pow(321.0f/9.0f,parameter);
            state_.position={0,2.5f,distance};state_.yaw=0;state_.pitch=0;
            if(routeFrame_<2*routeSteps*routeHoldFrames)++routeFrame_;
            intent_={};
        }
        else if(!UpdateFreeCamera(state_,intent_,dt))return false;
        if(!world_.SetTransform(camera_,FreeCameraTransform(state_)))return false;
        if(!paused_&&!lodRoute_)angle_=std::fmod(angle_+dt*0.35,2.0*Pi); // 路线模式冻结旋转，保证逐帧可对照。
        for(std::size_t index=0;index<boxes_.size();++index)
        {
            const auto rotation=AxisAngle({0,1,0},static_cast<float>(angle_)+0.25f)*AxisAngle({1,0,0},0.15f);
            const auto scale=index==2?Vec3{-1.3f,0.75f,1}:Vec3{1.3f,0.75f,1};
            if(!world_.SetTransform(boxes_[index],{{(float(index)-1)*2.0f,-4,0},rotation,scale})
                ||!world_.SetMesh(boxes_[index],{BuiltinCubeAssetId,{normalMaterials_[index==0||!normalEnabled_?0:normalIndex_]}}))return false;
        }
        if(lighting_||!environmentKind_.empty())
        {
            if(!world_.SetLight(directional_,{{1,0.96f,0.9f},directionalEnabled_?0.35f:0.0f}))return false;
            const Vec3 colors[]{{1,0.15f,0.08f},{0.1f,1,0.25f},{0.15f,0.3f,1}};
            if(lighting_)for(std::size_t index=0;index<points_.size();++index)
            {
                const float phase=static_cast<float>(angle_)+float(index)*2*Pi/3;
                if(!world_.SetTransform(points_[index],{{std::cos(phase)*2.5f,std::sin(phase)*2.5f,2},{},{1,1,1}})
                    ||!world_.SetLight(points_[index],{colors[index],pointsEnabled_?80.0f:0.0f,LightType::Point,6}))return false;
            }
        }
        return true;
    };
    services.extract=[this](FrameData& data){BuildFrameData(world_,nullptr,nullptr,data);data.assets=assets_.get();data.exposure=exposure_;
        // G06.10：Lighting 演示可显式启用 Forward 实例化（布局 30）；其它受光/阴影/环境场景仍走原路径。
        wchar_t instancingValue[8]{};
        const auto instancingLength=GetEnvironmentVariableW(L"SELFENGINE_INSTANCING",instancingValue,8);
        data.instancing=lighting_&&instancingLength>0&&!(instancingLength==1&&instancingValue[0]==L'0');
        if(!environmentKind_.empty()){data.environment={environments_[environmentIndex_],environmentIntensity_,environmentYaw_};data.shadow.enabled=false;data.shadow.resolution=256;}
        if(post_){data.post=postSettings_;data.referenceUiOverlays=true;UiVisual panel{};panel.id=1000;panel.kind=UiKind::Panel;panel.rectangle={16,16,120,24};panel.clip={0,0,1e6f,1e6f};panel.color={0.2f,0.5f,0.8f,1};panel.opacity=1;panel.enabled=true;data.ui.push_back(panel);}};
    services.windowClosing=[this]{intent_={};router_.Reset();assets_.reset();};return services;
}
}
