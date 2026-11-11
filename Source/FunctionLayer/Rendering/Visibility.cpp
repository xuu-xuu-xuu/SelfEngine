#include "Visibility.h"

namespace se
{
std::optional<Aabb> WorldBoundsCache::Resolve(StableEntityId entity,const Mat4& world,const std::shared_ptr<const StaticRenderModel>& model)
{
    if(!model||!entity)return std::nullopt;
    auto [found,inserted]=entries_.try_emplace(entity);auto& entry=found->second;
    if(inserted||entry.model.lock()!=model||entry.world.values!=world.values)
    {
        entry.model=model;entry.world=world;entry.bounds=TransformBounds({model->layout.minimum,model->layout.maximum},world);
        // 旧模型校验允许微小仿射误差；存在投影分量时不据其顶点盒拒绝整个三角形。
        for(const auto& part:model->layout.parts)
            if(part.modelFromMesh(3,0)!=0||part.modelFromMesh(3,1)!=0||part.modelFromMesh(3,2)!=0||part.modelFromMesh(3,3)!=1){entry.bounds.reset();break;}
        ++updates_;
    }
    entry.epoch=epoch_;return entry.bounds;
}
void WorldBoundsCache::EndFrame()
{
    std::erase_if(entries_,[&](const auto& item){return item.second.epoch!=epoch_||item.second.model.expired();});
}
}
