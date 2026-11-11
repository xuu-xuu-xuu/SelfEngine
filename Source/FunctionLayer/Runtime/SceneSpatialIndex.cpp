#include "SceneSpatialIndex.h"
#include <algorithm>
#include <chrono>

namespace se
{
namespace
{
using Timer=std::chrono::steady_clock;
double Milliseconds(Timer::time_point start){return std::chrono::duration<double,std::milli>(Timer::now()-start).count();}
bool ValidClass(SpatialClass value){return value==SpatialClass::Static||value==SpatialClass::Dynamic;}
}
bool SceneSpatialIndex::OwnerThread(){return std::this_thread::get_id()==thread_||Fail("Spatial.WrongThread");}
void SceneSpatialIndex::Clear()
{if(!OwnerThread())return;entries_.clear();conservative_.clear();static_.Clear();dynamic_.Clear();statistics_={};revision_=0;localDirty_=staticDirty_=false;error_.clear();}
bool SceneSpatialIndex::BindWorld(World& world)
{if(!OwnerThread())return false;if(world_!=&world){Clear();world_=&world;}return true;}
bool SceneSpatialIndex::Insert(EntityHandle entity,const Aabb& bounds,SpatialClass classification)
{
    if(!OwnerThread())return false;if(!world_->IsAlive(entity))return Fail("Spatial.InvalidEntity");
    if(!IsValid(bounds)||!ValidClass(classification))return Fail("Spatial.InvalidBounds");
    if(entries_.contains(entity.Pack()))return Fail("Spatial.AlreadyIndexed");
    entries_.emplace(entity.Pack(),Entry{entity,bounds,{},classification});localDirty_=true;error_.clear();return true;
}
void SceneSpatialIndex::RemoveLeaf(Entry& entry)
{
    if(!entry.indexed)return;if(entry.classification==SpatialClass::Static)staticDirty_=true;else dynamic_.Remove(entry.entity.Pack());entry.indexed=false;
}
bool SceneSpatialIndex::Remove(EntityHandle entity)
{
    if(!OwnerThread())return false;const auto found=entries_.find(entity.Pack());if(found==entries_.end())return Fail("Spatial.InvalidEntity");
    RemoveLeaf(found->second);entries_.erase(found);localDirty_=true;++statistics_.removed;error_.clear();return true;
}
bool SceneSpatialIndex::UpdateBounds(EntityHandle entity,const Aabb& bounds)
{
    if(!OwnerThread())return false;if(!IsValid(bounds))return Fail("Spatial.InvalidBounds");
    const auto found=entries_.find(entity.Pack());if(found==entries_.end()||!world_->IsAlive(entity))return Fail("Spatial.InvalidEntity");
    found->second.local=bounds;found->second.dirty=localDirty_=true;error_.clear();return true;
}
bool SceneSpatialIndex::SetClassification(EntityHandle entity,SpatialClass classification)
{
    if(!OwnerThread())return false;if(!ValidClass(classification))return Fail("Spatial.InvalidBounds");
    const auto found=entries_.find(entity.Pack());if(found==entries_.end()||!world_->IsAlive(entity))return Fail("Spatial.InvalidEntity");
    if(found->second.classification!=classification){RemoveLeaf(found->second);found->second.classification=classification;found->second.dirty=localDirty_=true;}error_.clear();return true;
}
bool SceneSpatialIndex::Synchronize()
{
    if(!OwnerThread())return false;error_.clear();const auto revision=world_->SpatialRevision();
    if(!localDirty_&&revision_==revision&&revision!=UINT64_MAX)return true;
    const auto start=Timer::now();++statistics_.synchronizations;std::vector<Entry*> dirty;
    for(auto it=entries_.begin();it!=entries_.end();)
    {
        auto& entry=it->second;++statistics_.scanned;
        if(!world_->IsAlive(entry.entity)){RemoveLeaf(entry);it=entries_.erase(it);++statistics_.removed;continue;}
        const auto version=world_->EntitySpatialRevision(entry.entity);const bool enabled=world_->IsEnabled(entry.entity);
        if(entry.dirty||version!=entry.revision||version==UINT64_MAX||enabled!=entry.enabled)
        {entry.revision=version;entry.enabled=enabled;dirty.push_back(&entry);}
        ++it;
    }
    const auto updateStart=Timer::now();
    for(auto* entry:dirty)
    {
        entry->dirty=false;++statistics_.updated;
        if(!entry->enabled){RemoveLeaf(*entry);entry->valid=false;continue;}
        const auto bounds=TransformBounds(entry->local,world_->GetWorldMatrix(entry->entity));
        // 合法局部 bounds 在祖先乘积溢出时保守返回候选，绝不静默裁掉对象。
        if(!bounds){RemoveLeaf(*entry);entry->valid=false;continue;}entry->world=*bounds;entry->valid=true;entry->indexed=true;
        if(entry->classification==SpatialClass::Static)staticDirty_=true;
        else if(!dynamic_.Upsert({entry->entity.Pack(),entry->world}))return Fail("Spatial.InvalidBounds");
    }
    statistics_.updateMilliseconds+=Milliseconds(updateStart);
    if(staticDirty_)
    {
        const auto rebuild=Timer::now();std::vector<spatial::Leaf> leaves;
        for(const auto& [key,entry]:entries_)if(entry.enabled&&entry.valid&&entry.classification==SpatialClass::Static)leaves.push_back({key,entry.world});
        if(!static_.Build(leaves))return Fail("Spatial.InvalidBounds");staticDirty_=false;++statistics_.staticRebuilds;statistics_.rebuildMilliseconds+=Milliseconds(rebuild);
    }
    conservative_.clear();for(const auto& [key,entry]:entries_)if(entry.enabled&&!entry.valid)conservative_.push_back(key);
    revision_=revision;localDirty_=false;statistics_.synchronizeMilliseconds+=Milliseconds(start);return true;
}
bool SceneSpatialIndex::Eligible(const Entry& entry,SceneHandle scene)const
{return entry.enabled&&world_->IsAlive(entry.entity)&&world_->IsEnabled(entry.entity)&&(!scene||scenes_->SceneOf(entry.entity)==scene);}
std::vector<EntityHandle> SceneSpatialIndex::Resolve(std::vector<std::uint64_t> keys,SceneHandle scene)
{
    std::sort(keys.begin(),keys.end());keys.erase(std::unique(keys.begin(),keys.end()),keys.end());std::vector<EntityHandle> result;result.reserve(keys.size());
    for(const auto key:keys){const auto found=entries_.find(key);if(found!=entries_.end()&&Eligible(found->second,scene))result.push_back(found->second.entity);}
    ++statistics_.queries;statistics_.candidates+=result.size();return result;
}
std::vector<EntityHandle> SceneSpatialIndex::QueryAabb(const Aabb& bounds,SceneHandle scene,bool bruteForce)
{
    const auto start=Timer::now();if(!OwnerThread())return {};if(!IsValid(bounds)){Fail("Spatial.InvalidBounds");return {};}if(!Synchronize())return {};
    std::vector<std::uint64_t> keys;
    if(bruteForce){for(const auto& [key,entry]:entries_)if(entry.enabled&&(!entry.valid||spatial::Overlaps(entry.world,bounds)))keys.push_back(key);}
    else
    {
        std::size_t a=0,b=0;keys=static_.Query(bounds,&a);auto dynamic=dynamic_.Query(bounds,&b);keys.insert(keys.end(),dynamic.begin(),dynamic.end());statistics_.nodeTests+=a+b;
        keys.insert(keys.end(),conservative_.begin(),conservative_.end());
    }
    auto result=Resolve(std::move(keys),scene);statistics_.queryMilliseconds+=Milliseconds(start);return result;
}
std::vector<EntityHandle> SceneSpatialIndex::RaycastCandidates(const spatial::Ray& ray,SceneHandle scene,bool bruteForce)
{
    const auto start=Timer::now();if(!OwnerThread())return {};const auto prepared=spatial::PrepareRay(ray);if(!prepared){Fail("Spatial.InvalidRay");return {};}if(!Synchronize())return {};
    std::vector<std::uint64_t> keys;
    if(bruteForce){for(const auto& [key,entry]:entries_)if(entry.enabled&&(!entry.valid||spatial::Intersect(entry.world,*prepared)))keys.push_back(key);}
    else
    {
        std::size_t a=0,b=0;keys=static_.Query(*prepared,&a);auto dynamic=dynamic_.Query(*prepared,&b);keys.insert(keys.end(),dynamic.begin(),dynamic.end());statistics_.nodeTests+=a+b;
        keys.insert(keys.end(),conservative_.begin(),conservative_.end());
    }
    auto result=Resolve(std::move(keys),scene);statistics_.queryMilliseconds+=Milliseconds(start);return result;
}
std::optional<Aabb> SceneSpatialIndex::WorldBounds(EntityHandle entity)
{if(!Synchronize())return std::nullopt;const auto found=entries_.find(entity.Pack());return found!=entries_.end()&&found->second.valid?std::optional<Aabb>{found->second.world}:std::nullopt;}
SpatialStatistics SceneSpatialIndex::Statistics()const
{
    auto result=statistics_;const auto stat=static_.Statistics(),dyn=dynamic_.Statistics();result.tracked=entries_.size();result.staticLeaves=stat.leaves;result.dynamicLeaves=dyn.leaves;result.staticHeight=stat.height;result.dynamicHeight=dyn.height;result.dynamicRebuilds=dyn.rebuilds;
    result.dynamicRebuildMilliseconds=dyn.rebuildMilliseconds;
    for(const auto& [_,entry]:entries_)if(entry.enabled&&!entry.valid)++result.conservativeLeaves;return result;
}
}
