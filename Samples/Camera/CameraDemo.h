#pragma once

#include "FunctionLayer/Application/FrameData.h"
#include "FunctionLayer/Application/FreeCameraInput.h"

namespace se
{
class CameraDemo
{
public:
    explicit CameraDemo(bool texturedCubes=false):texturedCubes_(texturedCubes) {}
    // 返回的回调借用本对象，须覆盖 RunSandboxApp 全部生命周期。
    ApplicationServices Services();
    const World& Scene() const noexcept { return world_; }
    const FreeCameraState& State() const noexcept { return state_; }
private:
    World world_;
    EntityHandle camera_;
    FreeCameraState state_{ {0,0,2}, 0, 0 };
    FreeCameraInput intent_;
    FreeCameraInputRouter router_;
    bool texturedCubes_=false;
    EntityHandle cube_,backCube_;
    double angle_=0;
};
}
