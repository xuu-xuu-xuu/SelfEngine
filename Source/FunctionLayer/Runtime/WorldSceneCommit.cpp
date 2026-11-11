#include "World.h"
#include <algorithm>

namespace se
{
// 只在场景主线程边界调用。先准备容量和映射，再停用旧成员；保留成员不重新发放身份。
bool World::CommitScene(World& candidate,std::span<const EntityHandle> remove,std::span<const StableEntityId> retiredIds,std::vector<EntityHandle>& added,const std::function<void()>& beforeRetire)
{
    if(&candidate==this||staging_||sceneCommitting_||!candidate.staging_||IsIterating()||!commands_.empty()||(frameActive_&&!preparingFrame_)
        ||candidate.IsInFrame()||candidate.IsIterating()||candidate.HasPendingCommands())return Fail("Scene.CommitNotSafe");
    struct Retiring {EntityHandle entity;std::vector<ComponentHandle> components;std::size_t depth;};
    std::vector<Retiring> retiring;retiring.reserve(remove.size());
    std::unordered_set<std::uint64_t> removed;removed.reserve(remove.size());
    for(const auto entity:remove)
    {
        if(!IsAllocated(entity)||!removed.insert(entity.Pack()).second)continue;
        std::size_t depth=0;for(auto parent=slots_[entity.index].record.parent;IsAllocated(parent);parent=slots_[parent.index].record.parent)++depth;
        retiring.push_back({entity,slots_[entity.index].components,depth});
    }
    for(std::size_t i=0;i<slots_.size();++i)if(slots_[i].state!=SlotState::Free&&slots_[i].record.parent&&removed.contains(slots_[i].record.parent.Pack())
        &&!removed.contains(EntityHandle{static_cast<std::uint32_t>(i),slots_[i].generation}.Pack()))return Fail("Scene.CrossSceneParent");
    std::sort(retiring.begin(),retiring.end(),[](const auto& a,const auto& b){return a.depth>b.depth||(a.depth==b.depth&&a.entity.Pack()>b.entity.Pack());});

    auto used=usedStableIds_;for(const auto id:retiredIds)used.erase(id);
    for(const auto id:candidate.usedStableIds_)if(!used.insert(id).second)return Fail("Scene.StableIdConflict");
    auto stable=stableIds_;for(const auto& old:retiring)stable.erase(slots_[old.entity.index].record.stableId);
    stable.reserve(stable.size()+candidate.stableIds_.size());
    auto freeEntities=freeSlots_,freeComponents=freeComponentSlots_;
    std::size_t removedComponents=0;
    for(const auto& old:retiring){freeEntities.push_back(old.entity.index);for(const auto component:old.components){freeComponents.push_back(component.index);++removedComponents;}}
    auto entityCount=slots_.size(),componentCount=componentSlots_.size();
    const auto allocate=[](std::vector<std::uint32_t>& available,std::size_t& size)->std::uint32_t{
        if(!available.empty()){const auto index=available.back();available.pop_back();return index;}
        if(size>=UINT32_MAX)return UINT32_MAX;return static_cast<std::uint32_t>(size++);
    };
    std::vector<std::uint32_t> entities(candidate.slots_.size(),UINT32_MAX),components(candidate.componentSlots_.size(),UINT32_MAX);
    std::vector<EntityHandle> published;published.reserve(candidate.aliveCount_);
    for(std::size_t i=0;i<candidate.slots_.size();++i)
    {
        const auto& slot=candidate.slots_[i];if(slot.state==SlotState::Free)continue;
        if(slot.state!=SlotState::Alive)return Fail("Scene.InvalidCandidate");
        const auto index=allocate(freeEntities,entityCount);if(index==UINT32_MAX)return Fail("World.HandleExhausted");entities[i]=index;
        const EntityHandle handle{index,slot.generation};if(!stable.emplace(slot.record.stableId,handle).second)return Fail("Scene.StableIdConflict");published.push_back(handle);
    }
    for(std::size_t i=0;i<candidate.componentSlots_.size();++i)
    {
        const auto& slot=candidate.componentSlots_[i];if(slot.state==ComponentSlotState::Free)continue;
        const ComponentHandle handle{static_cast<std::uint32_t>(i),slot.generation};const auto* value=candidate.RawComponent(handle);
        if(slot.state!=ComponentSlotState::Alive||!value||value->Lifecycle()!=ComponentLifecycle::Detached||!candidate.IsAlive(slot.owner))return Fail("Scene.InvalidCandidate");
        const auto index=allocate(freeComponents,componentCount);if(index==UINT32_MAX)return Fail("World.ComponentHandleExhausted");components[i]=index;
    }
    auto controls=scriptControls_;
    for(auto it=controls.begin();it!=controls.end();)if(removed.contains(componentSlots_[it->second.index].owner.Pack()))it=controls.erase(it);else ++it;
    for(const auto& [control,handle]:candidate.scriptControls_)if(!controls.emplace(control,ComponentHandle{components[handle.index],handle.generation}).second)return Fail("World.DuplicateScriptControl");

    transforms_.ReserveAdditional(candidate.transforms_.Size());cameras_.ReserveAdditional(candidate.cameras_.Size());meshes_.ReserveAdditional(candidate.meshes_.Size());
    lights_.ReserveAdditional(candidate.lights_.Size());scripts_.ReserveAdditional(candidate.scripts_.Size());
    registeredPools_.reserve(registeredPools_.size()+candidate.registeredPools_.size());
    for(const auto& [type,incoming]:candidate.registeredPools_)
    {
        auto found=registeredPools_.find(type);
        if(found!=registeredPools_.end()&&found->second->TypeToken()!=incoming->TypeToken())return Fail("Metadata.DuplicateType");
        if(found==registeredPools_.end())found=registeredPools_.emplace(type,incoming->Empty()).first;
        found->second->ReserveAdditional(incoming->Size());
    }
    freeSlots_.reserve(freeSlots_.size()+retiring.size());freeComponentSlots_.reserve(freeComponentSlots_.size()+removedComponents);
    const auto oldEntityCount=slots_.size(),oldComponentCount=componentSlots_.size();
    try{slots_.resize(entityCount);componentSlots_.resize(componentCount);}
    catch(...){slots_.resize(oldEntityCount);componentSlots_.resize(oldComponentCount);throw;}

    // 此后的池节点转移不分配；旧组件回调的外部作用遵守提交后故障语义。
    const bool previousShutdown=shuttingDown_;shuttingDown_=true;sceneCommitting_=true;
    struct Restore{bool& shutdown;bool& committing;bool previous;~Restore(){shutdown=previous;committing=false;}} restore{shuttingDown_,sceneCommitting_,previousShutdown};
    if(beforeRetire)try{beforeRetire();}catch(...){/* 已进入提交点，宿主回调故障不能回滚其外部作用。 */}
    for(const auto& old:retiring){auto& slot=slots_[old.entity.index];if(slot.state==SlotState::Alive)--aliveCount_;slot.state=SlotState::DestroyPending;}
    for(const auto& old:retiring)
    {
        for(auto component=old.components.rbegin();component!=old.components.rend();++component)ReleaseComponent(*component);
        auto& slot=slots_[old.entity.index];slot.components.clear();slot.record={};slot.state=SlotState::Free;
    }
    shuttingDown_=previousShutdown;
    for(std::size_t i=0;i<candidate.componentSlots_.size();++i)if(components[i]!=UINT32_MAX)
    {
        auto& slot=candidate.componentSlots_[i];auto* value=candidate.RawComponent({static_cast<std::uint32_t>(i),slot.generation});
        value->owner_.index=entities[value->owner_.index];value->handle_.index=components[i];slot.owner.index=entities[slot.owner.index];
    }
    for(std::size_t i=0;i<candidate.slots_.size();++i)if(entities[i]!=UINT32_MAX)
    {
        auto& slot=candidate.slots_[i];if(slot.record.parent)slot.record.parent.index=entities[slot.record.parent.index];
        for(auto& handle:slot.components)handle.index=components[handle.index];slots_[entities[i]]=std::move(slot);
    }
    for(std::size_t i=0;i<candidate.componentSlots_.size();++i)if(components[i]!=UINT32_MAX)componentSlots_[components[i]]=candidate.componentSlots_[i];
    transforms_.TransferMapped(candidate.transforms_,entities);cameras_.TransferMapped(candidate.cameras_,entities);meshes_.TransferMapped(candidate.meshes_,entities);lights_.TransferMapped(candidate.lights_,entities);
    scripts_.TransferMapped(candidate.scripts_,components);
    for(auto& [type,incoming]:candidate.registeredPools_)registeredPools_.at(type)->TransferMapped(*incoming,components);
    stableIds_.swap(stable);usedStableIds_.swap(used);scriptControls_.swap(controls);freeSlots_.swap(freeEntities);freeComponentSlots_.swap(freeComponents);
    aliveCount_+=candidate.aliveCount_;TouchSpatial();for(const auto entity:published)TouchSpatial(entity);
    candidate.ResetStorage();added.swap(published);sceneCommitting_=false;lastError_.clear();return true;
}
}
