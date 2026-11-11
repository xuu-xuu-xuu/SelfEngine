#pragma once
#include "FunctionLayer/Application/FrameData.h"
#include "FunctionLayer/Application/FreeCameraInput.h"
#include "ResourceLayer/Assets/AssetStore.h"

namespace se
{
class ShadowsDemo
{
public:
    explicit ShadowsDemo(std::string locale="zh-CN",FreeCameraState camera={{0,4.5f,8},0,-0.45f}):locale_(std::move(locale)),entryCamera_(camera) {}
    ApplicationServices Services();
private:
    World world_;
    std::unique_ptr<AssetStore> assets_;
    EntityHandle camera_,light_,cube_;
    FreeCameraState state_{{0,4.5f,8},0,-0.45f};
    FreeCameraInput intent_;
    FreeCameraInputRouter router_;
    DirectionalShadowView shadow_;
    std::string locale_;
    double angle_=0;
    float sunAngle_=0.6f;
    unsigned biasStage_=1;
    bool paused_=false;
    FreeCameraState entryCamera_;
};
}
