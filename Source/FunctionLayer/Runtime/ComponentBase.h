#pragma once
#include "RuntimeHandles.h"

namespace se
{
class World;
enum class ComponentLifecycle {Detached,Attached,Active,Removing};
enum class ComponentEventKind {Attach,Enable,Disable,Detach};
struct ComponentEvent {ComponentEventKind kind;EntityHandle owner;ComponentHandle component;ComponentTypeId type;};
class ComponentBase
{
public:
    ComponentBase()=default;
    virtual ~ComponentBase()=default;
    // 属性候选复制不继承运行身份；对已有池元素赋值只更新派生类属性。
    ComponentBase(const ComponentBase&)noexcept{}
    ComponentBase(ComponentBase&&)noexcept{}
    ComponentBase& operator=(const ComponentBase&)noexcept{return *this;}
    ComponentBase& operator=(ComponentBase&&)noexcept{return *this;}
    EntityHandle Owner()const noexcept{return owner_;}
    ComponentHandle Handle()const noexcept{return handle_;}
    ComponentTypeId Type()const noexcept{return type_;}
    bool Enabled()const noexcept{return enabled_;}
    bool Active()const noexcept{return active_;}
    ComponentLifecycle Lifecycle()const noexcept{return lifecycle_;}
protected:
    virtual void OnAttach(){}
    virtual void OnEnable(){}
    virtual void OnDisable(){}
    virtual void OnDetach(){}
private:
    friend class World;
    EntityHandle owner_{};
    ComponentHandle handle_{};
    ComponentTypeId type_=0;
    bool enabled_=true,active_=false,faulted_=false;
    ComponentLifecycle lifecycle_=ComponentLifecycle::Detached;
};
}
