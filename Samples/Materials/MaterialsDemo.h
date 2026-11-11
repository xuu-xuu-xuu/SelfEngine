#pragma once
#include "FunctionLayer/Application/FrameData.h"
#include "FunctionLayer/Application/FreeCameraInput.h"
#include "ResourceLayer/Assets/AssetStore.h"
#include <array>

namespace se
{
class MaterialsDemo
{
public:
    explicit MaterialsDemo(std::string locale="zh-CN",bool lighting=false,std::string environment={},bool post=false,FreeCameraState camera={{0,-0.5f,9},0,0}):locale_(std::move(locale)),lighting_(lighting),environmentKind_(std::move(environment)),post_(post),entryCamera_(camera) {}
    ApplicationServices Services();
private:
    World world_;
    std::unique_ptr<AssetStore> assets_;
    EntityHandle camera_;
    std::array<EntityHandle,3> boxes_;
    std::array<EntityHandle,3> points_;
    EntityHandle directional_;
    std::array<AssetId,4> normalMaterials_;
    FreeCameraState state_{{0,-0.5f,9},0,0};
    FreeCameraInput intent_;
    FreeCameraInputRouter router_;
    std::string locale_;
    std::size_t normalIndex_=2;
    double angle_=0;
    bool paused_=false,normalEnabled_=true;
    bool lighting_=false,pointsEnabled_=true,directionalEnabled_=true;
    float exposure_=1;
    std::string environmentKind_;
    std::array<AssetId,2> environments_;
    std::size_t environmentIndex_=0;
    float environmentYaw_=0,environmentIntensity_=1;
    bool post_=false;
    rhi::PostProcessSettings postSettings_;
    FreeCameraState entryCamera_;
    // G05.08：固定远景相机路线。SELFENGINE_LOD_ROUTE=1 时按固定步序推远再拉近，
    // 每步保持 30 帧（0.5 s）让 0.15 s 过渡完成，便于逐帧对照三角数与 dither。
    bool lodRoute_=false;
    std::uint64_t routeFrame_=0;
};
}
