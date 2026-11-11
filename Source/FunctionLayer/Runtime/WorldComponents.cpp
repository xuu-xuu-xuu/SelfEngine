#include "World.h"
#include "ComponentMetadata.h"
#include <algorithm>

namespace se
{
ComponentHandle World::ReserveComponent(EntityHandle owner,ComponentTypeId type)
{
    if(shuttingDown_){Fail("World.ShuttingDown");return {};}
    if(!IsKnown(owner)){Fail("World.InvalidEntity");return {};}
    const auto generation=NextComponentGeneration();if(!generation){Fail("World.ComponentHandleExhausted");return {};}
    std::uint32_t index=0;
    if(freeComponentSlots_.empty())
    {
        if(componentSlots_.size()>=UINT32_MAX){Fail("World.ComponentHandleExhausted");return {};}
        index=static_cast<std::uint32_t>(componentSlots_.size());componentSlots_.emplace_back();
    }
    else{index=freeComponentSlots_.back();freeComponentSlots_.pop_back();}
    componentSlots_[index]={generation,ComponentSlotState::Reserved,owner,type};
    const ComponentHandle handle{index,generation};slots_[owner.index].components.push_back(handle);return handle;
}
ComponentBase* World::RawComponent(ComponentHandle handle)
{
    if(!handle||handle.index>=componentSlots_.size())return nullptr;
    const auto& slot=componentSlots_[handle.index];if(slot.generation!=handle.generation||slot.state==ComponentSlotState::Free)return nullptr;
    switch(slot.type)
    {
    case ComponentTypes::Transform:return transforms_.Get(slot.owner.index);
    case ComponentTypes::Camera:return cameras_.Get(slot.owner.index);
    case ComponentTypes::Mesh:return meshes_.Get(slot.owner.index);
    case ComponentTypes::Light:return lights_.Get(slot.owner.index);
    case ComponentTypes::Script:return scripts_.Get(handle.index);
    default:{const auto it=registeredPools_.find(slot.type);return it==registeredPools_.end()?nullptr:it->second->Get(handle.index);}
    }
}
const ComponentBase* World::RawComponent(ComponentHandle handle)const{return const_cast<World*>(this)->RawComponent(handle);}
const ComponentBase* World::GetComponent(ComponentHandle handle)const
{
    const auto* component=RawComponent(handle);
    return component&&component->Handle()==handle&&componentSlots_[handle.index].state==ComponentSlotState::Alive&&IsAlive(component->Owner())?component:nullptr;
}
bool World::IsComponentPending(ComponentHandle handle,ComponentTypeId type)const
{return handle&&handle.index<componentSlots_.size()&&componentSlots_[handle.index].generation==handle.generation&&componentSlots_[handle.index].state==ComponentSlotState::Reserved&&(!type||componentSlots_[handle.index].type==type)&&IsKnown(componentSlots_[handle.index].owner);}
const ScriptComponent* World::GetScriptComponent(ComponentHandle handle)const
{const auto* component=GetComponent(handle);return component&&component->Type()==ComponentTypes::Script?static_cast<const ScriptComponent*>(component):nullptr;}
std::vector<ComponentHandle> World::Components(EntityHandle owner)const
{
    std::vector<ComponentHandle> result;if(!IsAlive(owner))return result;
    for(const auto handle:slots_[owner.index].components)if(GetComponent(handle))result.push_back(handle);return result;
}
ComponentHandle World::FindComponent(EntityHandle owner,ComponentTypeId type)const
{for(const auto handle:Components(owner))if(GetComponent(handle)->Type()==type)return handle;return {};}
bool World::InvokeLifecycle(ComponentBase& component,ComponentEventKind kind)
{
    const auto previous=callbackOwner_;callbackOwner_=component.Owner();
    ++lifecycleDepth_;bool result=true;
    try
    {
        switch(kind)
        {
        case ComponentEventKind::Attach:component.OnAttach();break;
        case ComponentEventKind::Enable:component.OnEnable();break;
        case ComponentEventKind::Disable:component.OnDisable();break;
        case ComponentEventKind::Detach:component.OnDetach();break;
        }
    }
    catch(...){component.faulted_=true;component.enabled_=false;result=Fail("World.ComponentCallbackFailed");}
    --lifecycleDepth_;callbackOwner_=previous;return result;
}
bool World::AttachComponent(ComponentBase& component,ComponentHandle handle)
{
    auto& slot=componentSlots_[handle.index];if(slot.state!=ComponentSlotState::Reserved)return false;
    component.owner_=slot.owner;component.handle_=handle;component.type_=slot.type;component.lifecycle_=ComponentLifecycle::Attached;slot.state=ComponentSlotState::Alive;
    if(staging_){component.lifecycle_=ComponentLifecycle::Detached;return true;}
    if(!InvokeLifecycle(component,ComponentEventKind::Attach))return false;
    if(slot.state!=ComponentSlotState::Alive||!IsAlive(slot.owner))return true;
    if(component.enabled_&&IsEnabled(slot.owner))
    {component.active_=true;component.lifecycle_=ComponentLifecycle::Active;if(!InvokeLifecycle(component,ComponentEventKind::Enable))return false;}
    return true;
}
bool World::ReleaseComponent(ComponentHandle handle)
{
    if(!handle||handle.index>=componentSlots_.size())return true;
    auto& slot=componentSlots_[handle.index];if(slot.generation!=handle.generation||slot.state==ComponentSlotState::Free)return true;
    slot.state=ComponentSlotState::Removing;bool result=true;
    if(auto* component=RawComponent(handle);component&&component->lifecycle_!=ComponentLifecycle::Detached)
    {
        if(component->active_){component->active_=false;component->lifecycle_=ComponentLifecycle::Attached;result=InvokeLifecycle(*component,ComponentEventKind::Disable)&&result;}
        component->lifecycle_=ComponentLifecycle::Removing;result=InvokeLifecycle(*component,ComponentEventKind::Detach)&&result;
    }
    switch(slot.type)
    {
    case ComponentTypes::Transform:transforms_.Remove(slot.owner.index);break;
    case ComponentTypes::Camera:cameras_.Remove(slot.owner.index);break;
    case ComponentTypes::Mesh:meshes_.Remove(slot.owner.index);break;
    case ComponentTypes::Light:lights_.Remove(slot.owner.index);break;
    case ComponentTypes::Script:
        if(const auto* component=scripts_.Get(handle.index))scriptControls_.erase(component->ControlledId());
        else{for(auto it=scriptControls_.begin();it!=scriptControls_.end();)if(it->second==handle)it=scriptControls_.erase(it);else ++it;}
        scripts_.Remove(handle.index);break;
    default:if(const auto it=registeredPools_.find(slot.type);it!=registeredPools_.end())it->second->Remove(handle.index);break;
    }
    if(IsAllocated(slot.owner)){auto& handles=slots_[slot.owner.index].components;std::erase(handles,handle);}
    slot.state=ComponentSlotState::Free;slot.owner={};slot.type=0;freeComponentSlots_.push_back(handle.index);return result;
}
bool World::IsEnabled(EntityHandle entity)const
{
    for(auto cursor=entity;cursor;)
    {const auto* record=Get(cursor);if(!record||!record->enabled)return false;cursor=record->parent;}
    return static_cast<bool>(entity);
}
bool World::RefreshComponents()
{
    if(staging_)return true;
    std::vector<ComponentHandle> handles;
    for(std::size_t i=0;i<componentSlots_.size();++i)if(componentSlots_[i].state==ComponentSlotState::Alive)handles.push_back({static_cast<std::uint32_t>(i),componentSlots_[i].generation});
    bool result=true;
    for(const auto handle:handles)
    {
        auto* component=RawComponent(handle);if(!component||component->lifecycle_==ComponentLifecycle::Detached||componentSlots_[handle.index].state!=ComponentSlotState::Alive)continue;
        const bool active=component->enabled_&&!component->faulted_&&IsEnabled(component->Owner());
        if(active==component->active_)continue;
        component->active_=active;component->lifecycle_=active?ComponentLifecycle::Active:ComponentLifecycle::Attached;
        if(!InvokeLifecycle(*component,active?ComponentEventKind::Enable:ComponentEventKind::Disable))
        {
            result=false;
            if(active){component->active_=false;component->lifecycle_=ComponentLifecycle::Attached;InvokeLifecycle(*component,ComponentEventKind::Disable);}
        }
    }
    return result;
}
ComponentHandle World::AttachScriptComponent(EntityHandle owner,std::uint64_t controlledId,ScriptComponent::Callback callback,bool enabled)
{
    if(!IsKnown(owner)||!controlledId){Fail("World.InvalidComponent");return {};}
    if(scriptControls_.contains(controlledId)){Fail("World.DuplicateScriptControl");return {};}
    const auto handle=ReserveComponent(owner,ComponentTypes::Script);if(!handle)return {};
    componentSlots_[handle.index].initialEnabled=enabled;
    scriptControls_.emplace(controlledId,handle);
    const bool accepted=Defer([owner,handle,controlledId,callback=std::move(callback)](World& world){
        if(handle.index>=world.componentSlots_.size()||world.componentSlots_[handle.index].generation!=handle.generation||world.componentSlots_[handle.index].state==ComponentSlotState::Free)return true;
        if(world.IsDestroyPending(owner)||world.componentSlots_[handle.index].state==ComponentSlotState::Removing){world.ReleaseComponent(handle);return true;}
        if(!world.IsAlive(owner))return world.Fail("World.InvalidEntity");
        auto* component=world.scripts_.Set(handle.index,ScriptComponent{controlledId,callback});
        component->enabled_=world.componentSlots_[handle.index].initialEnabled;
        if(world.AttachComponent(*component,handle))return true;
        world.ReleaseComponent(handle);return false;
    },true);
    if(!accepted)return {};
    return handle;
}
bool World::SetComponentEnabled(ComponentHandle handle,bool enabled)
{
    if(IsComponentPending(handle)){componentSlots_[handle.index].initialEnabled=enabled;return true;}
    const auto* component=GetComponent(handle);if(!component)return Fail("World.InvalidComponent");
    if(enabled&&component->faulted_)return Fail("World.ComponentFaulted");
    if(!enabled){auto* value=RawComponent(handle);value->enabled_=false;}
    return Defer([handle,enabled](World& world){auto* value=world.RawComponent(handle);if(!value||world.componentSlots_[handle.index].state!=ComponentSlotState::Alive)return true;value->enabled_=enabled;return world.RefreshComponents();});
}
bool World::RemoveComponent(ComponentHandle handle)
{
    if(IsComponentPending(handle)){componentSlots_[handle.index].state=ComponentSlotState::Removing;return Defer([handle](World& world){return world.ReleaseComponent(handle);});}
    const auto* component=GetComponent(handle);if(!component)return Fail("World.InvalidComponent");
    if(const auto* descriptor=FindComponentDescriptor(component->Type());descriptor&&!descriptor->optional)return Fail("World.RequiredComponent");
    componentSlots_[handle.index].state=ComponentSlotState::Removing;
    return Defer([handle](World& world){return world.ReleaseComponent(handle);});
}
void World::ResetStorage()noexcept
{
    TouchSpatial();
    sceneEnvironment_={};
    slots_.clear();freeSlots_.clear();stableIds_.clear();usedStableIds_.clear();commands_.clear();nextFrameCommands_.clear();frameActive_=false;preparingFrame_=false;
    transforms_={};cameras_={};meshes_={};lights_={};scripts_={};registeredPools_.clear();componentSlots_.clear();freeComponentSlots_.clear();scriptControls_.clear();aliveCount_=0;staging_=false;externalReference_={};
}
bool World::ActivateStagedComponents()
{
    if(IsIterating())return Fail("World.MutationDuringFrame");
    staging_=false;externalReference_={};bool result=true;const auto count=componentSlots_.size();
    for(std::size_t index=0;index<count;++index)
    {
        const auto& slot=componentSlots_[index];const ComponentHandle handle{static_cast<std::uint32_t>(index),slot.generation};
        auto* component=RawComponent(handle);
        if(slot.state!=ComponentSlotState::Alive||!component||component->lifecycle_!=ComponentLifecycle::Detached||!IsAlive(slot.owner))continue;
        component->lifecycle_=ComponentLifecycle::Attached;
        if(!InvokeLifecycle(*component,ComponentEventKind::Attach)){result=false;continue;}
        if(componentSlots_[index].state!=ComponentSlotState::Alive||!IsAlive(component->Owner())||!component->Enabled()||!IsEnabled(component->Owner()))continue;
        component->active_=true;component->lifecycle_=ComponentLifecycle::Active;
        if(!InvokeLifecycle(*component,ComponentEventKind::Enable))
        {component->active_=false;component->lifecycle_=ComponentLifecycle::Attached;InvokeLifecycle(*component,ComponentEventKind::Disable);result=false;}
    }
    return result;
}
}
