#include "HlodDemo.h"
#include "ResourceLayer/Assets/Primitives.h"
#include "PlatformLayer/Windows/ExecutablePath.h"
#include "CoreLayer/Localization.h"
#include "CoreLayer/Log.h"
#include <algorithm>
#include <Windows.h>

namespace se
{
ApplicationServices HlodDemo::Services()
{
    ApplicationServices services;
    services.windowOpened=[this](HWND){
        world_.Clear();spheres_.clear();
        distance_=400;
        wchar_t value[32]{};
        if(GetEnvironmentVariableW(L"SELFENGINE_HLOD_DISTANCE",value,32)>0)
        {
            const auto parsed=std::wcstol(value,nullptr,10);
            distance_=std::clamp(static_cast<float>(parsed),50.0f,1200.0f);
        }
        const auto directory=GetExecutableDirectory()/"Cache/HlodSample";
        const auto sources=directory/"Source";std::error_code error;
        std::filesystem::create_directories(sources,error);if(error)return false;
        assets_=std::make_unique<AssetStore>(sources,directory/"Derived");
        Localization text;std::string loadError;
        if(!text.Load(GetExecutableDirectory()/"Resources/Localization"/(locale_+".txt"),loadError))return false;
        const auto report=[&](const auto& diagnostics){for(const auto& d:diagnostics)
            Log(d.warning?LogLevel::Warning:LogLevel::Error,text.GetText(d.key)+": "+d.detail);};
        std::vector<AssetDiagnostic> diagnostics;
        MaterialDocument material;material.name="HlodMaterial";material.baseColor={0.85f,0.35f,0.08f,1};material.metallic=0.25f;material.roughness=0.55f;
        AssetId materialId;
        if(!assets_->SaveMaterialSource(sources/"hlod.material.json",material,materialId,diagnostics))
        {report(diagnostics);return false;}
        camera_=world_.Create("Camera",1);directional_=world_.Create("DirectionalLight",2);
        const FreeCameraState cameraState{{128,8,distance_},0,0};
        if(!camera_||!world_.SetCamera(camera_,{Pi/3,0.1f,2000,true})
            ||!world_.SetTransform(camera_,FreeCameraTransform(cameraState))
            ||!world_.SetLight(directional_,{{1,0.96f,0.9f},2})
            ||!world_.SetTransform(directional_,{{},AxisAngle({0,1,0},0.4f)*AxisAngle({1,0,0},-0.5f),{1,1,1}}))return false;
        const float offsets[4][2]={{-10,-10},{10,-10},{-10,10},{10,10}};
        for(int cellX=0;cellX<4;++cellX)for(int cellZ=0;cellZ<4;++cellZ)
            for(const auto& offset:offsets)
            {
                const auto entity=world_.Create("HlodSphere",static_cast<StableEntityId>(1000+spheres_.size()));
                if(!entity||!world_.SetMesh(entity,{BuiltinSphereAssetId,{materialId}})
                    ||!world_.SetTransform(entity,{{32.0f+64.0f*cellX+offset[0],8.0f,32.0f+64.0f*cellZ+offset[1]},
                        {},{2.2f,2.2f,2.2f}}))return false;
                spheres_.push_back(entity);
            }
        return true;
    };
    services.input=[this](const InputState&,bool,bool){};
    services.update=[](double){return true;};
    services.extract=[this](FrameData& data){BuildFrameData(world_,nullptr,nullptr,data);data.assets=assets_.get();};
    services.windowClosing=[this]{assets_.reset();};
    return services;
}
}
