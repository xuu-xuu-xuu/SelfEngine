#include "DemoPreset.h"
#include "ResourceLayer/Assets/AssetIO.h"
#include <nlohmann/json.hpp>
#include <cmath>
namespace se
{
bool LoadDemoPreset(const std::filesystem::path& path,DemoPreset& output,std::vector<AssetDiagnostic>& diagnostics)
{
    const auto invalid=[&]{asset_io::Diagnostic(diagnostics,"Sample.DemoPresetInvalid",asset_io::Utf8(path));return false;};
    std::vector<std::uint8_t> bytes;if(!asset_io::Read(path,bytes)||bytes.size()>64*1024)return invalid();
    try
    {
        const auto root=nlohmann::json::parse(bytes);if(!root.is_object()||!root.at("schema").is_number_integer()||root.at("schema")!=1)return invalid();
        DemoPreset candidate;const auto scene=root.at("scene").get<std::string>();
        if(scene=="materials")candidate.scene=DemoScene::Materials;else if(scene=="shadows")candidate.scene=DemoScene::Shadows;else if(scene=="postprocess")candidate.scene=DemoScene::PostProcess;else return invalid();
        const auto size=root.at("size");if(!size.is_array()||size.size()!=2)return invalid();
        for(const auto& dimension:size)if(!dimension.is_number_unsigned()||dimension.get<std::uint64_t>()<1||dimension.get<std::uint64_t>()>16384)return invalid();
        candidate.width=size[0].get<std::uint32_t>();candidate.height=size[1].get<std::uint32_t>();if(std::uint64_t(candidate.width)*candidate.height>(asset_io::MaxFileBytes-64)/3)return invalid();
        const auto camera=root.at("camera"),position=camera.at("position");if(!position.is_array()||position.size()!=3)return invalid();
        float* coordinates[]{&candidate.camera.position.x,&candidate.camera.position.y,&candidate.camera.position.z};
        for(size_t axis=0;axis<3;++axis){const auto value=position[axis].get<double>();if(!std::isfinite(value)||std::abs(value)>1e6)return invalid();*coordinates[axis]=static_cast<float>(value);}
        const double yaw=camera.at("yaw").get<double>(),pitch=camera.at("pitch").get<double>();if(!std::isfinite(yaw)||!std::isfinite(pitch)||std::abs(yaw)>1e6||std::abs(pitch)>FreeCameraPitchLimit)return invalid();
        candidate.camera.yaw=static_cast<float>(std::remainder(yaw,2*Pi));candidate.camera.pitch=static_cast<float>(pitch);candidate.fixedDelta=root.at("fixed_delta").get<double>();
        if(!std::isfinite(candidate.fixedDelta)||candidate.fixedDelta<=0||candidate.fixedDelta>0.1||!root.at("capture_frame").is_number_unsigned())return invalid();
        candidate.captureFrame=root.at("capture_frame").get<std::uint64_t>();if(!candidate.captureFrame||candidate.captureFrame>1'000'000)return invalid();output=candidate;return true;
    }
    catch(const nlohmann::json::exception&){return invalid();}
}
}
