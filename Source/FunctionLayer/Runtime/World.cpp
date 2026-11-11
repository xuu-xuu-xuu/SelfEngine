#include "FunctionLayer/Runtime/World.h"
#include "FunctionLayer/Runtime/ComponentMetadata.h"

#include <atomic>
#include <algorithm>
#include <exception>
#include <limits>
#include <stdexcept>
#include <utility>

namespace se
{
namespace
{
std::atomic<std::uint64_t> nextGeneration{1};
std::atomic<std::uint64_t> nextComponentGeneration{1};
std::atomic<StableEntityId> nextStableId{1};
std::uint64_t AllocateIdentity(std::atomic<std::uint64_t>& counter,std::uint64_t limit)
{
    auto value=counter.load();
    while(value&&value<=limit)
    {
        const auto next=value==limit?0:value+1;
        if(counter.compare_exchange_weak(value,next))return value;
    }
    return 0; // 耗尽永久保持 0，不回绕到旧身份。
}
bool ValidCamera(const CameraComponent& c)
{
    return IsFinite(c.verticalFovRadians) && c.verticalFovRadians>0 && c.verticalFovRadians<Pi
        && IsFinite(c.nearPlane) && IsFinite(c.farPlane) && c.nearPlane>0 && c.farPlane>c.nearPlane;
}
}

World::~World()
{
    shuttingDown_=true;Clear();
    while(HasPendingCommands())FlushCommands();
}
std::uint32_t World::NextComponentGeneration()noexcept{return static_cast<std::uint32_t>(AllocateIdentity(nextComponentGeneration,UINT32_MAX));}
World::World(const World& other)
{
    try
    {
    if(other.IsInFrame()||other.IsIterating() || other.HasPendingCommands()) throw std::logic_error("World.SnapshotPending");
    for(const auto source:other.Entities())
    {
        const auto* record=other.Get(source);
        const auto target=Create(record->name,record->stableId);
        if(!target) throw std::runtime_error(LastError());
        SetEnabled(target,record->enabled);
    }
    for(const auto source:other.Entities())
    {
        const auto* record=other.Get(source);
        if(record->parent)
            SetParent(FindByStableId(record->stableId),FindByStableId(other.Get(record->parent)->stableId));
    }
    for(const auto source:other.Entities())
    {
        std::string error;
        if(!CloneComponentProperties(other,source,*this,FindByStableId(other.Get(source)->stableId),error))throw std::runtime_error(error);
    }
    usedStableIds_=other.usedStableIds_;
    sceneEnvironment_=other.sceneEnvironment_;
    }
    catch(...){shuttingDown_=true;Clear();while(HasPendingCommands())FlushCommands();throw;}
}

World& World::operator=(const World& other)
{
    if(this==&other) return *this;
    if(IsIterating()) throw std::logic_error("World.MutationDuringIteration");
    World copied(other);
    *this=std::move(copied);
    return *this;
}

World::World(World&& other) noexcept { *this=std::move(other); }
World& World::operator=(World&& other) noexcept
{
    if(this==&other) return *this;
    // 活跃遍历中的世界地址和组件借用不能移动。
    if(IsInFrame()||other.IsInFrame()||IsIterating() || other.IsIterating()) std::terminate();
    shuttingDown_=true;Clear();while(HasPendingCommands())FlushCommands();ResetStorage();shuttingDown_=false;
    slots_=std::move(other.slots_); freeSlots_=std::move(other.freeSlots_);
    stableIds_=std::move(other.stableIds_); transforms_=std::move(other.transforms_);
    usedStableIds_=std::move(other.usedStableIds_);
    cameras_=std::move(other.cameras_); meshes_=std::move(other.meshes_); lights_=std::move(other.lights_);
    scripts_=std::move(other.scripts_);componentSlots_=std::move(other.componentSlots_);freeComponentSlots_=std::move(other.freeComponentSlots_);scriptControls_=std::move(other.scriptControls_);
    registeredPools_=std::move(other.registeredPools_);
    commands_=std::move(other.commands_); aliveCount_=std::exchange(other.aliveCount_,0);
    nextFrameCommands_=std::move(other.nextFrameCommands_);
    lastError_=std::move(other.lastError_);
    staging_=std::exchange(other.staging_,false);
    externalReference_=std::move(other.externalReference_);
    sceneEnvironment_=std::move(other.sceneEnvironment_);
    spatialRevision_=(std::max)(spatialRevision_,other.spatialRevision_);TouchSpatial();
    for(auto& slot:slots_)slot.spatialRevision=spatialRevision_;
    other.ResetStorage();
    return *this;
}

bool World::Fail(const char* key) const { lastError_=key; return false; }
bool World::SetEnvironmentSettings(SceneEnvironmentSettings settings)
{
    if(IsInFrame()||IsIterating()||HasPendingCommands())return Fail("Scene.PendingCommands");
    if(settings.todProfile.size()>256||settings.todProfile.find('\0')!=std::string::npos)return Fail("TOD.InvalidProfileId");
    sceneEnvironment_=std::move(settings);lastError_.clear();return true;
}
void World::TouchSpatial(EntityHandle entity)noexcept
{
    // 饱和后索引强制刷新，不回绕并误用旧包围盒。
    if(spatialRevision_!=UINT64_MAX)++spatialRevision_;
    if(IsAllocated(entity))slots_[entity.index].spatialRevision=spatialRevision_;
}
std::uint64_t World::EntitySpatialRevision(EntityHandle entity)const
{
    if(spatialRevision_==UINT64_MAX)return UINT64_MAX;
    std::uint64_t result=0;
    for(auto cursor=entity;IsAlive(cursor);cursor=slots_[cursor.index].record.parent)result=(std::max)(result,slots_[cursor.index].spatialRevision);
    return result;
}
bool World::IsAllocated(EntityHandle entity) const
{
    return entity && entity.index<slots_.size() && slots_[entity.index].generation==entity.generation
        && slots_[entity.index].state!=SlotState::Free;
}
bool World::IsDestroyPending(EntityHandle entity)const{return IsAllocated(entity)&&slots_[entity.index].state==SlotState::DestroyPending;}
bool World::IsKnown(EntityHandle entity)const{return IsAllocated(entity)&&!IsDestroyPending(entity);}
bool World::IsAlive(EntityHandle entity) const { return IsKnown(entity) && slots_[entity.index].state==SlotState::Alive; }
const EntityRecord* World::Get(EntityHandle entity) const { return IsAlive(entity)?&slots_[entity.index].record:nullptr; }
EntityHandle World::FindByStableId(StableEntityId id) const
{
    const auto it=stableIds_.find(id); return it!=stableIds_.end() && IsAlive(it->second)?it->second:EntityHandle{};
}

EntityHandle World::ResolveReference(StableEntityId id) const
{
    const auto entity=FindByStableId(id);
    if(!entity){Fail("World.UnresolvedReference");return {};}
    lastError_.clear();return entity;
}
bool World::CanResolveReference(StableEntityId id)const
{return static_cast<bool>(FindByStableId(id))||(staging_&&externalReference_&&externalReference_(id));}
std::vector<StableEntityId> World::KnownStableIds() const
{
    std::vector<StableEntityId> result(usedStableIds_.begin(),usedStableIds_.end());
    std::sort(result.begin(),result.end());return result;
}
bool World::ReserveStableId(StableEntityId id)
{
    if(IsIterating()||HasPendingCommands())return Fail("World.SnapshotPending");
    if(!id)return Fail("World.InvalidStableId");
    usedStableIds_.insert(id);lastError_.clear();return true;
}
bool World::ForgetStableId(StableEntityId id)
{
    if(IsIterating()||HasPendingCommands())return Fail("World.SnapshotPending");
    if(!id||stableIds_.contains(id))return Fail("World.InvalidStableId");
    usedStableIds_.erase(id);lastError_.clear();return true;
}

EntityHandle World::Create(std::string name,StableEntityId stableId)
{
    if(shuttingDown_){Fail("World.ShuttingDown");return {};}
    freeSlots_.reserve(freeSlots_.size()+1); // 场景接入失败时退回预留槽，不在回退中分配。
    if(name.size()>4096) {Fail("World.InvalidName");return {};}
    if(stableId!=0 && stableIds_.contains(stableId)) {Fail("World.DuplicateStableId");return {};}
    if(stableId!=0 && usedStableIds_.contains(stableId)) {Fail("World.RetiredStableId");return {};}
    const auto generation=AllocateIdentity(nextGeneration,UINT32_MAX);
    if(!generation) {Fail("World.HandleExhausted");return {};}
    if(stableId==0)
    {
        do {stableId=AllocateIdentity(nextStableId,UINT64_MAX);} while(stableId!=0 && usedStableIds_.contains(stableId));
        if(stableId==0) {Fail("World.HandleExhausted");return {};}
    }
    std::uint32_t index=0;
    if(freeSlots_.empty())
    {
        if(slots_.size()>=UINT32_MAX) {Fail("World.HandleExhausted");return {};}
        index=static_cast<std::uint32_t>(slots_.size()); slots_.emplace_back();
    }
    else {index=freeSlots_.back();freeSlots_.pop_back();}
    auto& slot=slots_[index];
    slot.generation=static_cast<std::uint32_t>(generation);slot.state=SlotState::Reserved;
    slot.record={stableId,std::move(name),{},true};
    const EntityHandle handle{index,slot.generation};stableIds_.emplace(stableId,handle);usedStableIds_.insert(stableId);
    if(sceneCreated_)
    {
        const auto rollback=[&]{stableIds_.erase(stableId);usedStableIds_.erase(stableId);slot.record={};slot.state=SlotState::Free;freeSlots_.push_back(index);};
        try{if(!sceneCreated_(handle)){rollback();Fail("Scene.MembershipRejected");return {};}}
        catch(...){rollback();throw;}
    }
    Defer([handle](World& world){return world.Activate(handle);},true);
    lastError_.clear();return handle;
}
bool World::Activate(EntityHandle entity)
{
    if(!IsAllocated(entity)||IsDestroyPending(entity))return true;
    if(!IsKnown(entity) || slots_[entity.index].state!=SlotState::Reserved) return Fail("World.InvalidEntity");
    slots_[entity.index].state=SlotState::Alive;++aliveCount_;TouchSpatial(entity);
    if(Store(transforms_,entity,Transform{}))return true;
    MarkDestroy(entity);DestroyNow(entity);return false;
}
bool World::Defer(std::function<bool(World&)> operation,bool nextFrame)
{
    if(frameActive_&&nextFrame&&(!preparingFrame_||lifecycleDepth_!=0)){nextFrameCommands_.push_back(std::move(operation));return true;}
    if(IsIterating() || !commands_.empty()) {commands_.push_back(std::move(operation));return true;}
    return operation(*this);
}
bool World::DeferEntity(EntityHandle entity,std::function<bool(World&)> operation,bool nextFrame)
{
    if(!IsKnown(entity))return Fail("World.InvalidEntity");
    return Defer([entity,operation=std::move(operation)](World& world){return !world.IsAllocated(entity)||world.IsDestroyPending(entity)?true:operation(world);},nextFrame||(frameActive_&&slots_[entity.index].state==SlotState::Reserved));
}
bool World::MarkDestroy(EntityHandle entity)
{
    if(!IsKnown(entity))return Fail("World.InvalidEntity");
    std::vector<EntityHandle> pending{entity};
    for(std::size_t i=0;i<pending.size();++i)
        for(std::size_t index=0;index<slots_.size();++index)
            if(slots_[index].state!=SlotState::Free&&slots_[index].record.parent==pending[i])pending.push_back({static_cast<std::uint32_t>(index),slots_[index].generation});
    for(const auto handle:pending)
    {auto& slot=slots_[handle.index];if(slot.state==SlotState::Alive)--aliveCount_;slot.state=SlotState::DestroyPending;}
    TouchSpatial();
    return true;
}
bool World::Destroy(EntityHandle entity)
{
    if(!MarkDestroy(entity))return false;
    return Defer([entity](World& world){return world.DestroyNow(entity);});
}
bool World::DestroyNow(EntityHandle entity)
{
    if(!IsAllocated(entity))return true;
    if(!IsDestroyPending(entity)&&!MarkDestroy(entity))return false;
    std::vector<EntityHandle> pending{entity};
    for(std::size_t i=0;i<pending.size();++i)
        for(std::size_t index=0;index<slots_.size();++index)
            if(slots_[index].state!=SlotState::Free&&slots_[index].record.parent==pending[i])pending.push_back({static_cast<std::uint32_t>(index),slots_[index].generation});
    bool result=true;
    for(auto it=pending.rbegin();it!=pending.rend();++it)
    {
        auto& slot=slots_[it->index];const auto components=slot.components;
        for(auto component=components.rbegin();component!=components.rend();++component)result=ReleaseComponent(*component)&&result;
        stableIds_.erase(slot.record.stableId);slot.components.clear();
        slot.record={};slot.state=SlotState::Free;freeSlots_.push_back(it->index);
    }
    return result;
}
std::vector<EntityHandle> World::Entities() const
{
    std::vector<EntityHandle> result;result.reserve(aliveCount_);
    for(std::size_t i=0;i<slots_.size();++i)if(slots_[i].state==SlotState::Alive)
        result.push_back({static_cast<std::uint32_t>(i),slots_[i].generation});
    return result;
}
bool World::Rename(EntityHandle entity,std::string name)
{
    if(!IsKnown(entity))return Fail("World.InvalidEntity");
    if(name.size()>4096)return Fail("World.InvalidName");
    return DeferEntity(entity,[entity,name=std::move(name)](World& world)
    {if(!world.IsAlive(entity))return world.Fail("World.InvalidEntity");world.slots_[entity.index].record.name=name;return true;});
}
bool World::SetEnabled(EntityHandle entity,bool enabled)
{
    if(!IsKnown(entity))return Fail("World.InvalidEntity");
    if(!enabled){slots_[entity.index].record.enabled=false;TouchSpatial(entity);}
    return DeferEntity(entity,[entity,enabled](World& world)
    {if(!world.IsAlive(entity))return world.Fail("World.InvalidEntity");world.slots_[entity.index].record.enabled=enabled;world.TouchSpatial(entity);return world.RefreshComponents();});
}
bool World::SetParent(EntityHandle entity,EntityHandle parent)
{
    if(shuttingDown_)return Fail("World.ShuttingDown");
    if(!IsKnown(entity) || (parent && !IsKnown(parent)))return Fail("World.InvalidEntity");
    if(sceneParent_&&!sceneParent_(entity,parent))return Fail("Scene.CrossSceneParent");
    return DeferEntity(entity,[entity,parent](World& world){return parent&&!world.IsKnown(parent)?true:world.ParentNow(entity,parent);},true);
}
bool World::ParentNow(EntityHandle entity,EntityHandle parent)
{
    if(!IsAlive(entity) || (parent && !IsAlive(parent)))return Fail("World.InvalidEntity");
    if(sceneParent_&&!sceneParent_(entity,parent))return Fail("Scene.CrossSceneParent");
    for(auto cursor=parent;cursor;cursor=Get(cursor)->parent) if(cursor==entity)return Fail("World.ParentCycle");
    slots_[entity.index].record.parent=parent;TouchSpatial(entity);return RefreshComponents();
}
Mat4 World::GetWorldMatrix(EntityHandle entity) const
{
    Mat4 result;
    if(!IsAlive(entity)) {Fail("World.InvalidEntity");return result;}
    for(auto cursor=entity;cursor;cursor=Get(cursor)->parent) result=GetTransform(cursor)->Matrix()*result;
    return result;
}
const Transform* World::GetTransform(EntityHandle entity) const {const auto* component=IsAlive(entity)?transforms_.Get(entity.index):nullptr;return component?&component->value:nullptr;}
bool World::SetTransform(EntityHandle entity,const Transform& transform,TransformWriteMode mode)
{
    if(!IsKnown(entity))return Fail("World.InvalidEntity");
    if(!IsValid(transform)||(mode!=TransformWriteMode::Normalize&&mode!=TransformWriteMode::PreserveUnit))return Fail("World.InvalidTransform");
    return DeferEntity(entity,[entity,transform,mode](World& world)
    {if(!world.IsAlive(entity))return world.Fail("World.InvalidEntity");auto normalized=transform;
        if(mode!=TransformWriteMode::PreserveUnit||std::abs(LengthSquared(normalized.rotation)-1)>1e-6f)normalized.rotation=Normalize(normalized.rotation);
        if(!world.Store(world.transforms_,entity,normalized))return false;world.TouchSpatial(entity);return true;});
}
const CameraComponent* World::GetCamera(EntityHandle entity) const {const auto* component=IsAlive(entity)?cameras_.Get(entity.index):nullptr;return component?&component->value:nullptr;}
bool World::SetCamera(EntityHandle entity,const CameraComponent& component)
{
    if(!IsKnown(entity))return Fail("World.InvalidEntity");if(!ValidCamera(component))return Fail("World.InvalidComponent");
    return DeferEntity(entity,[entity,component](World& world){if(!world.IsAlive(entity))return world.Fail("World.InvalidEntity");return world.Store(world.cameras_,entity,component);},cameras_.Get(entity.index)==nullptr);
}
bool World::RemoveCamera(EntityHandle entity)
{
    if(!IsKnown(entity))return Fail("World.InvalidEntity");
    if(const auto handle=FindComponent(entity,ComponentTypes::Camera))return RemoveComponent(handle);
    return DeferEntity(entity,[entity](World& world){const auto handle=world.FindComponent(entity,ComponentTypes::Camera);return handle?world.ReleaseComponent(handle):true;},true);
}
const MeshComponent* World::GetMesh(EntityHandle entity) const {const auto* component=IsAlive(entity)?meshes_.Get(entity.index):nullptr;return component?&component->value:nullptr;}
bool World::SetMesh(EntityHandle entity,const MeshComponent& component)
{
    if(!IsKnown(entity))return Fail("World.InvalidEntity");
    if(component.meshAsset.size()>4096 || component.materials.size()>1024)return Fail("World.InvalidComponent");
    for(const auto& material:component.materials)if(material.size()>4096)return Fail("World.InvalidComponent");
    return DeferEntity(entity,[entity,component](World& world){if(!world.IsAlive(entity))return world.Fail("World.InvalidEntity");return world.Store(world.meshes_,entity,component);},meshes_.Get(entity.index)==nullptr);
}
bool World::RemoveMesh(EntityHandle entity)
{
    if(!IsKnown(entity))return Fail("World.InvalidEntity");
    if(const auto handle=FindComponent(entity,ComponentTypes::Mesh))return RemoveComponent(handle);
    return DeferEntity(entity,[entity](World& world){const auto handle=world.FindComponent(entity,ComponentTypes::Mesh);return handle?world.ReleaseComponent(handle):true;},true);
}
const LightComponent* World::GetLight(EntityHandle entity) const {const auto* component=IsAlive(entity)?lights_.Get(entity.index):nullptr;return component?&component->value:nullptr;}
bool World::SetLight(EntityHandle entity,const LightComponent& component)
{
    if(!IsKnown(entity))return Fail("World.InvalidEntity");
    if(!IsFinite(component.color) || !IsFinite(component.intensity) || component.intensity<0
        || component.color.x<0 || component.color.y<0 || component.color.z<0||!IsFinite(component.range)||component.range<=0
        ||(component.type!=LightType::Directional&&component.type!=LightType::Point))return Fail("World.InvalidComponent");
    return DeferEntity(entity,[entity,component](World& world){if(!world.IsAlive(entity))return world.Fail("World.InvalidEntity");return world.Store(world.lights_,entity,component);},lights_.Get(entity.index)==nullptr);
}
bool World::RemoveLight(EntityHandle entity)
{
    if(!IsKnown(entity))return Fail("World.InvalidEntity");
    if(const auto handle=FindComponent(entity,ComponentTypes::Light))return RemoveComponent(handle);
    return DeferEntity(entity,[entity](World& world){const auto handle=world.FindComponent(entity,ComponentTypes::Light);return handle?world.ReleaseComponent(handle):true;},true);
}
void World::ForEach(const std::function<void(EntityHandle)>& callback)
{
    const auto snapshot=Entities();
    ++iterationDepth_;
    try {for(const auto entity:snapshot)callback(entity);}
    catch(...) {--iterationDepth_;throw;}
    --iterationDepth_;
}
bool World::FlushCommands()
{
    if(frameActive_)return Fail("World.MutationDuringFrame");
    for(auto& command:nextFrameCommands_)commands_.push_back(std::move(command));nextFrameCommands_.clear();
    return FlushPhaseCommands();
}
bool World::FlushPhaseCommands()
{
    if(IsIterating())return Fail("World.MutationDuringIteration");
    auto commands=std::move(commands_);commands_.clear();
    bool result=true;std::string firstError;
    for(auto& command:commands)if(!command(*this)) {if(result)firstError=lastError_;result=false;}
    if(result)lastError_.clear();else lastError_=std::move(firstError);
    return result;
}
bool World::BeginFrame()
{
    if(frameActive_||IsIterating())return Fail("World.MutationDuringFrame");
    for(auto& command:nextFrameCommands_)commands_.push_back(std::move(command));nextFrameCommands_.clear();
    frameActive_=true;
    if(FlushPhaseCommands())return true;frameActive_=false;return false;
}
bool World::FinishFrame()
{
    if(!frameActive_||IsIterating())return Fail("World.MutationDuringFrame");
    const bool result=FlushPhaseCommands();frameActive_=false;preparingFrame_=false;return result;
}
void World::InvokeComponentCallback(const std::function<void()>& callback,ComponentHandle origin)
{
    const auto previous=callbackOwner_;if(const auto* component=GetComponent(origin))callbackOwner_=component->Owner();
    ++iterationDepth_;try{callback();}catch(...){--iterationDepth_;callbackOwner_=previous;throw;}--iterationDepth_;callbackOwner_=previous;
}
void World::Clear()
{
    // 只覆盖调用时已存在/预留的身份；后续排队的 Create 仍可激活。
    std::vector<EntityHandle> existing;
    for(std::size_t i=0;i<slots_.size();++i)if(slots_[i].state!=SlotState::Free)existing.push_back({static_cast<std::uint32_t>(i),slots_[i].generation});
    for(const auto entity:existing)if(IsKnown(entity))MarkDestroy(entity);
    const bool result=Defer([existing=std::move(existing)](World& world){bool ok=true;for(const auto entity:existing)if(world.IsAllocated(entity))ok=world.DestroyNow(entity)&&ok;return ok;});
    if(!IsIterating()&&!HasPendingCommands()&&!aliveCount_){ResetStorage();if(result)lastError_.clear();}
}
}
