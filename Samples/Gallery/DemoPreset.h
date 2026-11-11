#pragma once
#include "FunctionLayer/Runtime/Camera.h"
#include "ResourceLayer/Assets/StaticModel.h"
namespace se
{
enum class DemoScene {Materials,Shadows,PostProcess};
struct DemoPreset
{
    DemoScene scene=DemoScene::Materials;
    std::uint32_t width=960,height=720;
    FreeCameraState camera{{0,3,9},0,-0.3f};
    double fixedDelta=1.0/60;
    std::uint64_t captureFrame=30;
};
bool LoadDemoPreset(const std::filesystem::path& path,DemoPreset& output,std::vector<AssetDiagnostic>& diagnostics);
}
