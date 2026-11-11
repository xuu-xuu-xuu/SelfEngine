#pragma once
#include "FunctionLayer/Application/FrameData.h"
#include "FunctionLayer/Application/FreeCameraInput.h"
#include <string>

namespace se
{
class FirstPersonDemo
{
public:
    explicit FirstPersonDemo(std::string locale="zh-CN"):locale_(std::move(locale)){}
    ApplicationServices Services();
    const CharacterController& Character()const noexcept{return character_;}
    const FreeCameraState& View()const noexcept{return view_;}
    std::optional<RayHit> Target()const;
private:
    bool Initialize();void Reset();
    std::string locale_;World world_;PhysicsWorld physics_;CharacterController character_;FixedStepClock clock_;
    EntityHandle camera_;FreeCameraState view_{};FreeCameraInput intent_;FreeCameraInputRouter input_;bool jumpPending_=false;
};
}
