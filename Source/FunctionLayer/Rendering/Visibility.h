#pragma once
#include "CoreLayer/Math/Bounds.h"
#include "FunctionLayer/Runtime/World.h"
#include "StaticModelRender.h"
#include <unordered_map>

namespace se
{
class WorldBoundsCache
{
public:
    void BeginFrame(){++epoch_;updates_=0;}
    std::optional<Aabb> Resolve(StableEntityId entity,const Mat4& world,const std::shared_ptr<const StaticRenderModel>& model);
    void EndFrame();
    void Clear(){entries_.clear();epoch_=updates_=0;}
    std::uint64_t Updates() const{return updates_;}
    size_t Size() const{return entries_.size();}
private:
    struct Entry {Mat4 world;std::weak_ptr<const StaticRenderModel> model;std::optional<Aabb> bounds;std::uint64_t epoch=0;};
    std::unordered_map<StableEntityId,Entry> entries_;
    std::uint64_t epoch_=0,updates_=0;
};
}
