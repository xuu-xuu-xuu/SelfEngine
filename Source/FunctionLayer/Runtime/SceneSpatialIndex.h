#pragma once
#include "SceneManager.h"
#include "CoreLayer/Spatial/BoundsIndex.h"
#include <map>

namespace se
{
enum class SpatialClass {Static,Dynamic};
struct SpatialStatistics
{
    std::size_t tracked=0,staticLeaves=0,dynamicLeaves=0,conservativeLeaves=0;
    int staticHeight=0,dynamicHeight=0;
    std::uint64_t synchronizations=0,scanned=0,updated=0,removed=0,staticRebuilds=0,dynamicRebuilds=0,queries=0,nodeTests=0,candidates=0;
    double synchronizeMilliseconds=0,updateMilliseconds=0,rebuildMilliseconds=0,dynamicRebuildMilliseconds=0,queryMilliseconds=0;
};
class SceneSpatialIndex
{
public:
    SceneSpatialIndex(World& world,SceneManager& scenes):world_(&world),scenes_(&scenes){}
    bool BindWorld(World& world);
    bool Insert(EntityHandle entity,const Aabb& localBounds,SpatialClass classification=SpatialClass::Dynamic);
    bool Remove(EntityHandle entity);
    bool UpdateBounds(EntityHandle entity,const Aabb& localBounds);
    bool SetClassification(EntityHandle entity,SpatialClass classification);
    bool Synchronize();
    void Clear();
    // 自有、按运行句柄排序的 broadphase 候选；每次查询前同步本阶段已提交的修改。
    std::vector<EntityHandle> QueryAabb(const Aabb& bounds,SceneHandle scene={},bool bruteForce=false);
    std::vector<EntityHandle> RaycastCandidates(const spatial::Ray& ray,SceneHandle scene={},bool bruteForce=false);
    std::optional<Aabb> WorldBounds(EntityHandle entity);
    SpatialStatistics Statistics()const;
    const std::string& LastError()const noexcept{return error_;}
private:
    struct Entry {EntityHandle entity;Aabb local,world;SpatialClass classification=SpatialClass::Dynamic;std::uint64_t revision=0;bool dirty=true,enabled=false,valid=false,indexed=false;};
    bool OwnerThread();
    bool Fail(const char* key){error_=key;return false;}
    void RemoveLeaf(Entry& entry);
    bool Eligible(const Entry& entry,SceneHandle scene)const;
    std::vector<EntityHandle> Resolve(std::vector<std::uint64_t> keys,SceneHandle scene);
    World* world_;SceneManager* scenes_;
    std::map<std::uint64_t,Entry> entries_;
    std::vector<std::uint64_t> conservative_;
    spatial::BoundsIndex static_,dynamic_;
    std::uint64_t revision_=0;
    bool localDirty_=false,staticDirty_=false;
    SpatialStatistics statistics_;
    std::thread::id thread_=std::this_thread::get_id();
    std::string error_;
};
}
