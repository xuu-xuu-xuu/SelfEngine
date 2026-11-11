#pragma once
#include "ComponentBase.h"
#include "ComponentValues.h"
#include <functional>
#include <utility>

namespace se
{
class TransformComponent final:public ComponentBase
{
public:
    static constexpr ComponentTypeId TypeId=ComponentTypes::Transform;
    Transform value;
    TransformComponent()=default;
    explicit TransformComponent(const Transform& properties):value(properties){}
};
class CameraRuntimeComponent final:public ComponentBase
{
public:
    static constexpr ComponentTypeId TypeId=ComponentTypes::Camera;
    CameraComponent value;
    explicit CameraRuntimeComponent(const CameraComponent& properties={}):value(properties){}
};
class MeshRuntimeComponent final:public ComponentBase
{
public:
    static constexpr ComponentTypeId TypeId=ComponentTypes::Mesh;
    MeshComponent value;
    explicit MeshRuntimeComponent(const MeshComponent& properties={}):value(properties){}
};
class LightRuntimeComponent final:public ComponentBase
{
public:
    static constexpr ComponentTypeId TypeId=ComponentTypes::Light;
    LightComponent value;
    explicit LightRuntimeComponent(const LightComponent& properties={}):value(properties){}
};
class ScriptComponent final:public ComponentBase
{
public:
    static constexpr ComponentTypeId TypeId=ComponentTypes::Script;
    using Callback=std::function<void(const ComponentEvent&)>;
    ScriptComponent(std::uint64_t controlled,Callback callback={}):controlled_(controlled),callback_(std::move(callback)){}
    std::uint64_t ControlledId()const noexcept{return controlled_;}
protected:
    void OnAttach()override{Notify(ComponentEventKind::Attach);}
    void OnEnable()override{Notify(ComponentEventKind::Enable);}
    void OnDisable()override{Notify(ComponentEventKind::Disable);}
    void OnDetach()override{Notify(ComponentEventKind::Detach);}
private:
    void Notify(ComponentEventKind kind){if(callback_)callback_({kind,Owner(),Handle(),Type()});}
    std::uint64_t controlled_=0;
    Callback callback_;
};
}
