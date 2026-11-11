#include "BenchmarkScene.h"
#include "ResourceLayer/Assets/AssetIO.h"
#include "ResourceLayer/Assets/Primitives.h"
#include "PlatformLayer/Windows/ExecutablePath.h"
#include "CoreLayer/Hash.h"
#include "CoreLayer/Log.h"
#include "CoreLayer/Localization.h"
#include <nlohmann/json.hpp>
#include <algorithm>

namespace se
{
namespace
{
struct Random
{
    std::uint32_t state;
    std::uint32_t Next(){state^=state<<13;state^=state>>17;state^=state<<5;return state;}
    float Unit(){return float(Next()>>8)*(1.0f/16777216);}
};
}
bool ValidateBenchmarkConfig(const BenchmarkConfig& c)
{
    return (c.visibility==BenchmarkVisibility::Baseline||c.visibility==BenchmarkVisibility::AllVisible||c.visibility==BenchmarkVisibility::MostlyOutside)
        &&(c.visibility==BenchmarkVisibility::Baseline||(c.radiusMultiplier>=5&&c.radiusMultiplier<=6&&c.heightMultiplier<=0.6f))
        &&(c.visibility!=BenchmarkVisibility::AllVisible||float(c.width)/float(c.height)>=1)
        &&(c.kind==BenchmarkKind::Repeated||c.kind==BenchmarkKind::Mixed)&&c.objects&&c.objects<=50000&&c.seed&&c.width>=160&&c.width<=8192&&c.height>=120&&c.height<=8192
        &&std::uint64_t(c.width)*c.height<=16*1024*1024&&std::isfinite(c.warmupSeconds)&&c.warmupSeconds>=0&&c.warmupSeconds<=60
        &&std::isfinite(c.sampleSeconds)&&c.sampleSeconds>=0.05&&c.sampleSeconds<=300&&std::isfinite(c.periodSeconds)&&c.periodSeconds>=1&&c.periodSeconds<=600
        &&IsFinite(c.radiusMultiplier)&&c.radiusMultiplier>=1&&c.radiusMultiplier<=10&&IsFinite(c.heightMultiplier)&&c.heightMultiplier>=0&&c.heightMultiplier<=5;
}
bool LoadBenchmarkConfig(const std::filesystem::path& path,BenchmarkConfig& output,std::vector<AssetDiagnostic>& diagnostics)
{
    const auto invalid=[&]{asset_io::Diagnostic(diagnostics,"Sample.BenchmarkConfigInvalid",asset_io::Utf8(path));return false;};std::vector<std::uint8_t> bytes;
    if(!asset_io::Read(path,bytes)||bytes.size()>64*1024)return invalid();
    try
    {
        const auto root=nlohmann::json::parse(bytes);if(!root.is_object()||!root.at("schema").is_number_integer()||root.at("schema")!=1)return invalid();BenchmarkConfig candidate;
        const auto scene=root.at("scene").get<std::string>();if(scene=="repeated")candidate.kind=BenchmarkKind::Repeated;else if(scene=="mixed")candidate.kind=BenchmarkKind::Mixed;else return invalid();
        const auto integer=[&](const nlohmann::json& number,std::uint32_t& target){if(!number.is_number_unsigned()||number.get<std::uint64_t>()>UINT32_MAX)return false;target=number.get<std::uint32_t>();return true;};
        const auto size=root.at("size");if(!size.is_array()||size.size()!=2||!integer(root.at("object_count"),candidate.objects)||!integer(root.at("seed"),candidate.seed)||!integer(size[0],candidate.width)||!integer(size[1],candidate.height))return invalid();
        candidate.warmupSeconds=root.at("warmup_seconds").get<double>();candidate.sampleSeconds=root.at("sample_seconds").get<double>();const auto camera=root.at("camera_path");
        candidate.periodSeconds=camera.at("period_seconds").get<double>();candidate.radiusMultiplier=camera.at("radius_multiplier").get<float>();candidate.heightMultiplier=camera.at("height_multiplier").get<float>();
        if(root.contains("visibility_case"))
        {
            const auto visibility=root.at("visibility_case").get<std::string>();
            if(visibility=="all_visible")candidate.visibility=BenchmarkVisibility::AllVisible;
            else if(visibility=="mostly_outside")candidate.visibility=BenchmarkVisibility::MostlyOutside;
            else if(visibility!="baseline")return invalid();
        }
        if(root.contains("frustum_culling"))candidate.frustumCulling=root.at("frustum_culling").get<bool>();
        if(root.contains("sorting"))candidate.sortOpaque=root.at("sorting").get<bool>();
        if(root.contains("binding_cache"))candidate.cacheOpaqueBindings=root.at("binding_cache").get<bool>();
        if(root.contains("instancing"))candidate.instancing=root.at("instancing").get<bool>();
        if(!ValidateBenchmarkConfig(candidate))return invalid();output=candidate;return true;
    }
    catch(const nlohmann::json::exception&){return invalid();}
}
bool BuildBenchmarkLayout(const BenchmarkConfig& config,BenchmarkLayout& output)
{
    if(!ValidateBenchmarkConfig(config))return false;BenchmarkLayout candidate;candidate.side=1;
    while(std::uint64_t(candidate.side)*candidate.side*candidate.side<config.objects)++candidate.side;
    Random random{config.seed};candidate.objects.reserve(config.objects);nlohmann::json state=nlohmann::json::array();
    for(std::uint32_t index=0;index<config.objects;++index)
    {
        const auto x=index%candidate.side,z=(index/candidate.side)%candidate.side,y=index/(candidate.side*candidate.side);const float half=float(candidate.side-1)*0.5f;
        BenchmarkObject object;object.transform.position={(float(x)-half)*2.2f+(random.Unit()-0.5f)*0.25f,(float(y)-half)*2.2f+(random.Unit()-0.5f)*0.25f,(float(z)-half)*2.2f+(random.Unit()-0.5f)*0.25f};
        object.transform.rotation=AxisAngle({0,1,0},random.Unit()*2*Pi);const float scale=0.7f+random.Unit()*0.3f;const auto selector=random.Next();object.transform.scale={scale,scale,scale};
        if(config.kind==BenchmarkKind::Mixed){object.sphere=(selector&1)!=0;object.material=(selector>>1)%24;object.transform.scale.y*=0.8f+float((selector>>16)&255)*(0.4f/255);if(index%17==0)object.transform.scale.x=-scale;}
        if(config.visibility==BenchmarkVisibility::MostlyOutside&&index%10!=0)
            object.transform.position+=Vec3{float(candidate.side)*40,float(candidate.side)*40,0};
        candidate.objects.push_back(object);const auto& p=object.transform.position;const auto& q=object.transform.rotation;const auto& s=object.transform.scale;
        state.push_back({{"position",{p.x,p.y,p.z}},{"rotation",{q.x,q.y,q.z,q.w}},{"scale",{s.x,s.y,s.z}},{"sphere",object.sphere},{"material_slot",object.material}});
    }
    auto identity=nlohmann::json{{"algorithm",BenchmarkLayoutAlgorithm(config.visibility)},{"seed",config.seed},{"objects",std::move(state)}};
    if(config.visibility!=BenchmarkVisibility::Baseline)identity["visibility_case"]=BenchmarkVisibilityName(config.visibility);
    candidate.digest=Sha256(identity.dump());
    if(candidate.digest.empty())return false;output=std::move(candidate);return true;
}
FreeCameraState BenchmarkCamera(const BenchmarkConfig& config,std::uint32_t side,double sampleTime)
{
    const double angle=std::remainder(sampleTime/config.periodSeconds*2*double(Pi),2*double(Pi));const float radius=float(side)*config.radiusMultiplier,height=float(side)*config.heightMultiplier;
    return {{radius*float(std::sin(angle)),height,radius*float(std::cos(angle))},float(angle),-std::atan2(height,radius)};
}
const char* BenchmarkVisibilityName(BenchmarkVisibility visibility)
{return visibility==BenchmarkVisibility::AllVisible?"all_visible":visibility==BenchmarkVisibility::MostlyOutside?"mostly_outside":"baseline";}
const char* BenchmarkLayoutAlgorithm(BenchmarkVisibility visibility)
{return visibility==BenchmarkVisibility::Baseline?"SelfEngine.BenchmarkLayout.1":"SelfEngine.BenchmarkLayout.2";}
ApplicationServices BenchmarkScene::Services()
{
    ApplicationServices services;services.initialWidth=session_.config.width;services.initialHeight=session_.config.height;
    services.windowOpened=[this](HWND){
        if(!BuildBenchmarkLayout(session_.config,session_.layout))return false;World candidate;const auto root=GetExecutableDirectory()/"Cache/BenchmarkSample"/(session_.config.kind==BenchmarkKind::Mixed?"mixed":"repeated");
        std::error_code error;std::filesystem::create_directories(root/"Source",error);if(error)return false;auto assets=std::make_unique<AssetStore>(root/"Source",root/"Derived");
        std::vector<AssetDiagnostic> diagnostics;std::array<AssetId,4> textures;const std::uint32_t textureCount=session_.config.kind==BenchmarkKind::Mixed?4:1;
        for(std::uint32_t index=0;index<textureCount;++index)
        {
            const auto name="texture-"+std::to_string(index)+".png";std::filesystem::copy_file(GetExecutableDirectory()/"Resources/Benchmark"/name,root/"Source"/name,std::filesystem::copy_options::overwrite_existing,error);
            if(error||!assets->RegisterSource(name,{},textures[index],diagnostics))return false;
        }
        const std::uint32_t materialCount=session_.config.kind==BenchmarkKind::Mixed?24:1;std::vector<AssetId> materials(materialCount);
        for(std::uint32_t index=0;index<materialCount;++index)
        {
            MaterialDocument material;material.name="Benchmark";material.baseColorTexture=textures[index%textureCount];material.baseColor={0.65f+float(index%3)*0.15f,0.7f+float(index%2)*0.2f,0.8f,1};
            material.metallic=float(index%3)*0.5f;material.roughness=0.2f+float(index%5)*0.2f;
            if(!assets->SaveMaterialSource(root/"Source"/("material-"+std::to_string(index)+".material.json"),material,materials[index],diagnostics))return false;
        }
        const auto camera=candidate.Create("BenchmarkCamera",1),light=candidate.Create("BenchmarkLight",2);if(!camera||!light||!candidate.SetCamera(camera,{Pi/3,0.1f,1000,true})
            ||!candidate.SetLight(light,{{1,0.96f,0.9f},3})||!candidate.SetTransform(light,{{},AxisAngle({0,1,0},0.4f)*AxisAngle({1,0,0},-0.6f),{1,1,1}}))return false;
        session_.camera=BenchmarkCamera(session_.config,session_.layout.side,session_.captureMode?0:-session_.config.warmupSeconds);
        if(!candidate.SetTransform(camera,FreeCameraTransform(session_.camera)))return false;
        for(size_t index=0;index<session_.layout.objects.size();++index)
        {
            const auto& object=session_.layout.objects[index];const auto entity=candidate.Create("BenchmarkObject",100+index);
            if(!entity||!candidate.SetTransform(entity,object.transform)||!candidate.SetMesh(entity,{object.sphere?BuiltinSphereAssetId:BuiltinCubeAssetId,{materials[object.material]}}))return false;
        }
        world_=std::move(candidate);camera_=world_.FindByStableId(1);assets_=std::move(assets);return true;
    };
    services.update=[this](double){if(!session_.running)return true;session_.sampleTime=std::chrono::duration<double>(std::chrono::steady_clock::now()-session_.started).count()-session_.config.warmupSeconds;
        session_.camera=BenchmarkCamera(session_.config,session_.layout.side,session_.captureMode?0:session_.sampleTime);return world_.SetTransform(camera_,FreeCameraTransform(session_.camera));};
    services.extract=[this](FrameData& data){BuildFrameData(world_,nullptr,nullptr,data);data.assets=assets_.get();data.frustumCulling=session_.config.frustumCulling;data.sortOpaque=session_.config.sortOpaque;data.cacheOpaqueBindings=session_.config.cacheOpaqueBindings;data.instancing=session_.config.instancing;};services.windowClosing=[this]{assets_.reset();world_.Clear();};return services;
}
}
