#pragma once
#include "FunctionLayer/Application/FrameData.h"
#include "FunctionLayer/Application/FreeCameraInput.h"
#include "ResourceLayer/Assets/AssetStore.h"

namespace se
{
class StaticModelDemo
{
public:
    explicit StaticModelDemo(std::string format):format_(std::move(format)){}
    ApplicationServices Services();
    const World& Scene() const noexcept { return world_; }
private:
    std::string format_;
    World world_;
    EntityHandle camera_;
    FreeCameraState state_{{1,0.8f,4},0,0};
    FreeCameraInput intent_;
    FreeCameraInputRouter router_;
    std::unique_ptr<AssetStore> assets_;
};
}
