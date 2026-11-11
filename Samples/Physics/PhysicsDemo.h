#pragma once
#include "FunctionLayer/Application/FrameData.h"
#include "FunctionLayer/Application/FreeCameraInput.h"
#include "FunctionLayer/Runtime/Environment.h"
#include <string>

namespace se
{
class PhysicsDemo
{
public:
    explicit PhysicsDemo(std::string locale="zh-CN"):locale_(std::move(locale)){}
    ApplicationServices Services();
    const PhysicsWorld& Physics()const noexcept{return physics_;}
    bool PushBox(std::size_t index,Vec3 impulse); // 动态箱体按行/列编号，冲量单位 N·s。
private:
    bool Initialize();
    std::string locale_;Environment environment_;World& world_=environment_.GetWorld();PhysicsWorld physics_;CharacterController capsule_;
    std::vector<std::pair<EntityHandle,BodyHandle>> boxes_;
    EntityHandle camera_;FreeCameraState view_{{7,5,10},.61f,-.22f};FreeCameraInput intent_;FreeCameraInputRouter input_;
};
}
