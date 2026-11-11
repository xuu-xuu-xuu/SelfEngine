#include "CameraDemo.h"
#include <algorithm>
#include "ResourceLayer/Assets/Primitives.h"
#include <cmath>

namespace se
{
ApplicationServices CameraDemo::Services()
{
    ApplicationServices services;
    services.windowOpened = [this](HWND) {
        world_.Clear();
        state_ = { {0,0,2}, 0, 0 };
        intent_ = {};
        router_.Reset();
        camera_ = world_.Create("Camera", 1);
        if(!camera_||!world_.SetCamera(camera_,{Pi/3,0.1f,1000,true})
            ||!world_.SetTransform(camera_,FreeCameraTransform(state_)))return false;
        angle_=0;
        if(texturedCubes_)
        {
            cube_=world_.Create("Cube",2);backCube_=world_.Create("BackCube",3);
            if(!cube_||!backCube_||!world_.SetMesh(cube_,{BuiltinCubeAssetId,{}})
                ||!world_.SetMesh(backCube_,{BuiltinCubeAssetId,{}}))return false;
        }
        return true;
    };
    services.input = [this](const InputState& input, bool keyboardConsumed, bool pointerConsumed) {
        const bool reset = input.hasFocus && !keyboardConsumed
            && std::any_of(input.keyEvents.begin(), input.keyEvents.end(), [](const auto& event) {
                return event.type == KeyEventType::Pressed && event.keyCode == 'R'; });
        if (reset)
        {
            state_ = { {0,0,2}, 0, 0 };
            intent_ = {};
            router_.Reset();
        }
        else intent_ = router_.Capture(input, input.hasFocus, keyboardConsumed, pointerConsumed);
    };
    services.update = [this](double dt) {
        if(!UpdateFreeCamera(state_,intent_,dt)||!world_.SetTransform(camera_,FreeCameraTransform(state_)))return false;
        if(texturedCubes_)
        {
            angle_=std::fmod(angle_+dt*0.5,2.0*Pi);
            const float angle=static_cast<float>(angle_);
            if(!world_.SetTransform(cube_,{{-0.2f,0,0},AxisAngle({0,1,0},angle)*AxisAngle({1,0,0},angle*0.6f),{0.8f,0.8f,0.8f}})
                ||!world_.SetTransform(backCube_,{{0.45f,0,-1.2f},AxisAngle({0,1,0},-angle+0.4f),{1,1,1}}))return false;
        }
        return true;
    };
    services.extract = [this](FrameData& data) { BuildFrameData(world_, nullptr, nullptr, data); };
    services.windowClosing = [this] { intent_ = {}; router_.Reset(); };
    return services;
}
}
