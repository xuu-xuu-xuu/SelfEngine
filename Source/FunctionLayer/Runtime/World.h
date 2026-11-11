#pragma once

#include "CoreLayer/Math/Math.h"
#include "RuntimeComponents.h"
#include "RegisteredComponentPool.h"
#include "ResourceLayer/Environment/TODProfile.h"
#include <cstdint>
#include <deque>
#include <functional>
#include <optional>
#include <span>
#include <memory>
#include <type_traits>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

namespace se
{
class SceneDocument;
enum class TransformWriteMode { Normalize,PreserveUnit };
struct EntityRecord
{
    StableEntityId stableId = 0;
    std::string name;
    EntityHandle parent{};
    bool enabled = true;
};

template<class T> class ComponentPool
{
public:
    const T* Get(std::uint32_t index) const { const auto it=values_.find(index);return it==values_.end()?nullptr:&it->second; }
    T* Get(std::uint32_t index) { const auto it=values_.find(index);return it==values_.end()?nullptr:&it->second; }
    T* Set(std::uint32_t index,const T& value) { return &values_.insert_or_assign(index,value).first->second; }
    void Remove(std::uint32_t index) { values_.erase(index); }
    std::size_t Size()const noexcept{return values_.size();}
    void ReserveAdditional(std::size_t count){values_.reserve(values_.size()+count);}
    void TransferMapped(ComponentPool& source,std::span<const std::uint32_t> indices)noexcept
    {while(!source.values_.empty()){auto node=source.values_.extract(source.values_.begin());node.key()=indices[node.key()];values_.insert(std::move(node));}}
private:
    std::unordered_map<std::uint32_t,T> values_;
};

class World
{
public:
    World() = default;
    struct StagingTag{};
    explicit World(StagingTag):staging_(true){}
    ~World();
    // 深拷贝：稳定 ID 不变，运行句柄重新发放，旧世界句柄不能访问副本。
    World(const World& other);
    World& operator=(const World& other);
    World(World&& other) noexcept;
    World& operator=(World&& other) noexcept;
    const SceneEnvironmentSettings& EnvironmentSettings()const noexcept{return sceneEnvironment_;}
    bool SetEnvironmentSettings(SceneEnvironmentSettings settings);

    EntityHandle Create(std::string name, StableEntityId stableId = 0);
    bool Destroy(EntityHandle entity); // 子树立即逻辑失效；回调/释放留到安全边界。
    bool IsAlive(EntityHandle entity) const;
    bool IsPending(EntityHandle entity)const{return IsKnown(entity)&&!IsAlive(entity);}
    const EntityRecord* Get(EntityHandle entity) const;
    EntityHandle FindByStableId(StableEntityId id) const;
    EntityHandle ResolveReference(StableEntityId id) const; // 失败记录 World.UnresolvedReference。
    bool CanResolveReference(StableEntityId id)const;
    std::vector<StableEntityId> KnownStableIds() const;
    // 保留持久引用身份；不创建实体，已存活 ID 可重复保留。仅在安全点调用。
    bool ReserveStableId(StableEntityId id);
    // 只用于撤销“本会话新建后删除、且从未保存”的实体：释放其已不存在的身份预留。
    bool ForgetStableId(StableEntityId id);
    std::vector<EntityHandle> Entities() const;
    std::size_t Size() const noexcept { return aliveCount_; }
    bool Rename(EntityHandle entity, std::string name);
    bool SetEnabled(EntityHandle entity, bool enabled);
    bool IsEnabled(EntityHandle entity) const; // 个人状态与祖先状态的有效值。
    bool SetParent(EntityHandle entity, EntityHandle parent); // 保持局部变换，不自动分解世界矩阵。
    Mat4 GetWorldMatrix(EntityHandle entity) const;
    std::uint64_t SpatialRevision()const noexcept{return spatialRevision_;}
    std::uint64_t EntitySpatialRevision(EntityHandle entity)const;
    const Transform* GetTransform(EntityHandle entity) const;
    // 快照保留已单位化的 float 位值；普通修改仍执行规范化。
    bool SetTransform(EntityHandle entity,const Transform& transform,TransformWriteMode mode=TransformWriteMode::Normalize);
    const CameraComponent* GetCamera(EntityHandle entity) const;
    bool SetCamera(EntityHandle entity,const CameraComponent& component);
    bool RemoveCamera(EntityHandle entity);
    const MeshComponent* GetMesh(EntityHandle entity) const;
    bool SetMesh(EntityHandle entity,const MeshComponent& component);
    bool RemoveMesh(EntityHandle entity);
    const LightComponent* GetLight(EntityHandle entity) const;
    bool SetLight(EntityHandle entity,const LightComponent& component);
    bool RemoveLight(EntityHandle entity);
    ComponentHandle FindComponent(EntityHandle owner,ComponentTypeId type) const;
    std::vector<ComponentHandle> Components(EntityHandle owner) const;
    const ComponentBase* GetComponent(ComponentHandle component) const;
    bool IsComponentPending(ComponentHandle component,ComponentTypeId type=0)const;
    const ScriptComponent* GetScriptComponent(ComponentHandle component) const;
    ComponentHandle AttachScriptComponent(EntityHandle owner,std::uint64_t controlledId,ScriptComponent::Callback callback={},bool enabled=true);
    bool SetComponentEnabled(ComponentHandle component,bool enabled);
    bool RemoveComponent(ComponentHandle component);
    // 扩展类型从 1024 起显式分配稳定 ID；属性候选在挂载之前准备完成。
    template<class T> ComponentHandle AttachRegisteredComponent(EntityHandle owner,const T& value,bool enabled=true,bool multiple=false)
    {
        static_assert(std::is_base_of_v<ComponentBase,T> && T::TypeId>=1024);
        if(!IsKnown(owner)){Fail("World.InvalidEntity");return {};}
        if(!multiple)for(const auto handle:slots_[owner.index].components)
            if(componentSlots_[handle.index].type==T::TypeId&&componentSlots_[handle.index].state!=ComponentSlotState::Removing)
            {Fail("Metadata.ComponentExists");return {};}
        const auto it=registeredPools_.find(T::TypeId);
        if(it!=registeredPools_.end()&&it->second->TypeToken()!=&registeredComponentTypeToken<T>)
        {Fail("Metadata.DuplicateType");return {};}
        const auto handle=ReserveComponent(owner,T::TypeId);if(!handle)return {};
        componentSlots_[handle.index].initialEnabled=enabled;
        if(!Defer([owner,handle,value](World& world){
            if(handle.index>=world.componentSlots_.size()||world.componentSlots_[handle.index].generation!=handle.generation||world.componentSlots_[handle.index].state==ComponentSlotState::Free)return true;
            if(world.IsDestroyPending(owner)||world.componentSlots_[handle.index].state==ComponentSlotState::Removing){world.ReleaseComponent(handle);return true;}
            if(!world.IsAlive(owner))return world.Fail("World.InvalidEntity");
            auto& pool=world.registeredPools_[T::TypeId];if(!pool)pool=std::make_unique<TypedRegisteredComponentPool<T>>();
            auto& typed=static_cast<TypedRegisteredComponentPool<T>&>(*pool);
            auto& component=typed.values.emplace(handle.index,value).first->second;
            component.enabled_=world.componentSlots_[handle.index].initialEnabled;
            if(world.AttachComponent(component,handle))return true;
            world.ReleaseComponent(handle);return false;
        },true))return {};
        return handle;
    }
    template<class T> const T* GetRegisteredComponent(ComponentHandle handle)const
    {
        const auto it=registeredPools_.find(T::TypeId);
        if(it==registeredPools_.end()||it->second->TypeToken()!=&registeredComponentTypeToken<T>)return nullptr;
        const auto* value=GetComponent(handle);return value&&value->Type()==T::TypeId?static_cast<const T*>(value):nullptr;
    }
    // 由注册的属性适配器先完成候选验证；赋值保留 E01 运行身份。
    template<class T> bool UpdateRegisteredComponent(ComponentHandle handle,const T& candidate)
    {
        if(IsIterating()||(HasPendingCommands()&&!sceneCommitting_))return Fail("Metadata.PendingCommands");
        const auto* current=GetRegisteredComponent<T>(handle);if(!current)return Fail("World.InvalidComponent");
        *const_cast<T*>(current)=candidate;return true;
    }
    // 回调仅借用 handle；遍历结束不自动应用，调用方在安全点 FlushCommands。
    void ForEach(const std::function<void(EntityHandle)>& callback);
    bool FlushCommands();
    bool HasPendingCommands() const noexcept { return !commands_.empty()||!nextFrameCommands_.empty(); }
    bool IsInFrame()const noexcept{return frameActive_;}
    bool BeginFrame();
    bool FinishFrame();
    bool FlushPhaseCommands(); // 仅安全阶段边界；不提交本帧新增结构。
    void InvokeComponentCallback(const std::function<void()>& callback,ComponentHandle origin={});
    bool IsIterating() const noexcept { return iterationDepth_ != 0||lifecycleDepth_!=0; }
    const std::string& LastError() const noexcept { return lastError_; }
    void Clear();
    bool IsStaging()const noexcept{return staging_;}
    bool IsSceneCommitBoundary()const noexcept{return sceneCommitting_;}
    bool ActivateStagedComponents(); // 主线程发布后执行；候选销毁不执行任何组件生命周期。

private:
    friend class Environment;
    friend class SceneManager;
    friend bool InstantiateScene(const SceneDocument&,World&,std::string&);
    bool CommitScene(World& candidate,std::span<const EntityHandle> remove,std::span<const StableEntityId> retiredIds,std::vector<EntityHandle>& added,const std::function<void()>& beforeRetire);
    enum class SlotState { Free, Reserved, Alive,DestroyPending };
    struct Slot { std::uint32_t generation = 0; SlotState state = SlotState::Free; EntityRecord record;std::vector<ComponentHandle> components;std::uint64_t spatialRevision=0; };
    enum class ComponentSlotState {Free,Reserved,Alive,Removing};
    struct ComponentSlot {std::uint32_t generation=0;ComponentSlotState state=ComponentSlotState::Free;EntityHandle owner{};ComponentTypeId type=0;bool initialEnabled=true;};
    bool Fail(const char* key) const;
    bool IsKnown(EntityHandle entity) const;
    bool IsAllocated(EntityHandle entity) const;
    bool IsDestroyPending(EntityHandle entity) const;
    bool MarkDestroy(EntityHandle entity);
    bool Activate(EntityHandle entity);
    bool DestroyNow(EntityHandle entity);
    bool ParentNow(EntityHandle entity,EntityHandle parent);
    bool Defer(std::function<bool(World&)> operation,bool nextFrame=false);
    bool DeferEntity(EntityHandle entity,std::function<bool(World&)> operation,bool nextFrame=false);
    ComponentHandle ReserveComponent(EntityHandle owner,ComponentTypeId type);
    static std::uint32_t NextComponentGeneration()noexcept;
    bool AttachComponent(ComponentBase& component,ComponentHandle handle);
    ComponentBase* RawComponent(ComponentHandle handle);
    const ComponentBase* RawComponent(ComponentHandle handle) const;
    bool ReleaseComponent(ComponentHandle handle);
    bool InvokeLifecycle(ComponentBase& component,ComponentEventKind kind);
    bool RefreshComponents();
    void ResetStorage() noexcept;
    void TouchSpatial(EntityHandle entity={})noexcept;
    template<class Runtime,class Value> bool Store(ComponentPool<Runtime>& pool,EntityHandle owner,const Value& properties)
    {
        if(auto* value=pool.Get(owner.index)){value->value=properties;return true;}
        const auto handle=ReserveComponent(owner,Runtime::TypeId);if(!handle)return false;
        auto* value=pool.Set(owner.index,Runtime{properties});
        if(AttachComponent(*value,handle))return true;
        ReleaseComponent(handle);return false;
    }
    std::deque<Slot> slots_;
    std::vector<std::uint32_t> freeSlots_;
    std::unordered_map<StableEntityId,EntityHandle> stableIds_;
    std::unordered_set<StableEntityId> usedStableIds_;
    ComponentPool<TransformComponent> transforms_;
    ComponentPool<CameraRuntimeComponent> cameras_;
    ComponentPool<MeshRuntimeComponent> meshes_;
    ComponentPool<LightRuntimeComponent> lights_;
    ComponentPool<ScriptComponent> scripts_;
    std::unordered_map<ComponentTypeId,std::unique_ptr<RegisteredComponentPool>> registeredPools_;
    std::deque<ComponentSlot> componentSlots_;
    std::vector<std::uint32_t> freeComponentSlots_;
    std::unordered_map<std::uint64_t,ComponentHandle> scriptControls_;
    std::vector<std::function<bool(World&)>> commands_;
    std::vector<std::function<bool(World&)>> nextFrameCommands_;
    bool frameActive_=false;
    bool preparingFrame_=false;
    std::size_t aliveCount_ = 0;
    std::uint64_t spatialRevision_=1;
    SceneEnvironmentSettings sceneEnvironment_;
    std::size_t iterationDepth_ = 0;
    std::size_t lifecycleDepth_=0;
    bool shuttingDown_=false;
    bool staging_=false;
    bool sceneCommitting_=false;
    EntityHandle callbackOwner_;
    std::function<bool(EntityHandle)> sceneCreated_;
    std::function<bool(EntityHandle,EntityHandle)> sceneParent_;
    std::function<bool(StableEntityId)> externalReference_;
    mutable std::string lastError_;
};
}
