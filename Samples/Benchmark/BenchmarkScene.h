#pragma once
#include "FunctionLayer/Application/FrameData.h"
#include "ResourceLayer/Assets/AssetStore.h"
#include <chrono>
namespace se
{
enum class BenchmarkKind {Repeated,Mixed};
enum class BenchmarkVisibility {Baseline,AllVisible,MostlyOutside};
struct BenchmarkConfig
{
    BenchmarkKind kind=BenchmarkKind::Repeated;
    std::uint32_t objects=1000,seed=424242,width=1280,height=720;
    double warmupSeconds=5,sampleSeconds=60,periodSeconds=60;
    float radiusMultiplier=2.6f,heightMultiplier=0.6f;
    BenchmarkVisibility visibility=BenchmarkVisibility::Baseline;
    bool frustumCulling=false;
    bool sortOpaque=false,cacheOpaqueBindings=false;
    bool instancing=false;
};
struct BenchmarkObject {Transform transform;bool sphere=false;std::uint32_t material=0;};
struct BenchmarkLayout {std::vector<BenchmarkObject> objects;std::uint32_t side=0;std::string digest;};
bool ValidateBenchmarkConfig(const BenchmarkConfig& config);
bool LoadBenchmarkConfig(const std::filesystem::path& path,BenchmarkConfig& output,std::vector<AssetDiagnostic>& diagnostics);
bool BuildBenchmarkLayout(const BenchmarkConfig& config,BenchmarkLayout& output);
const char* BenchmarkVisibilityName(BenchmarkVisibility visibility);
const char* BenchmarkLayoutAlgorithm(BenchmarkVisibility visibility);
FreeCameraState BenchmarkCamera(const BenchmarkConfig& config,std::uint32_t side,double sampleTime);
struct BenchmarkSession
{
    BenchmarkConfig config;
    BenchmarkLayout layout;
    std::chrono::steady_clock::time_point started;
    bool running=false,captureMode=false;
    double sampleTime=0;
    FreeCameraState camera;
};
class BenchmarkScene
{
public:
    explicit BenchmarkScene(BenchmarkSession& session):session_(session){}
    ApplicationServices Services();
private:
    BenchmarkSession& session_;
    World world_;
    EntityHandle camera_;
    std::unique_ptr<AssetStore> assets_;
};
}
