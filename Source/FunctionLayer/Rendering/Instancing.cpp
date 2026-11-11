#include "Instancing.h"
#include <bit>
#include <map>
#include <stdexcept>

namespace se
{
namespace
{
struct Key
{
    OpaqueSortKey draw;
    std::array<std::uint32_t,25> shared{};
    auto operator<=>(const Key&) const=default;
};
Key BatchKey(const rhi::DrawIndexed& draw)
{
    Key key{BuildOpaqueSortKey(draw)};size_t index=0;
    for(const auto value:{draw.pbr.cameraPosition.x,draw.pbr.cameraPosition.y,draw.pbr.cameraPosition.z,
        draw.pbr.directionToLight.x,draw.pbr.directionToLight.y,draw.pbr.directionToLight.z,draw.pbr.radiance.x,draw.pbr.radiance.y,draw.pbr.radiance.z})key.shared[index++]=std::bit_cast<std::uint32_t>(value);
    for(float value:draw.viewProjection.values)key.shared[index++]=std::bit_cast<std::uint32_t>(value);return key;
}
}
std::span<const PbrInstanceBatch> PbrInstanceBatcher::Build(std::span<const rhi::DrawIndexed> draws,
    std::span<const Aabb> bounds)
{
    if(draws.size()>rhi::MaximumFrameInstances)throw std::length_error("instances.capacity");
    const bool withBounds=!bounds.empty()&&bounds.size()==draws.size();
    for(auto& group:groups_){group.instances.clear();group.bounds.clear();}
    active_=0;std::map<Key,size_t> lookup;
    for(size_t drawIndex=0;drawIndex<draws.size();++drawIndex)
    {
        const auto& draw=draws[drawIndex];
        const auto key=BatchKey(draw);const auto [found,inserted]=lookup.try_emplace(key,active_);
        if(inserted){if(active_==groups_.size())groups_.emplace_back();groups_[active_++].draw=draw;}
        const auto& p=draw.pbr;groups_[found->second].instances.push_back({draw.world,draw.tint,p.metallic,p.roughness,p.normalScale,p.normalMapping,draw.unlit,p.doubleSided,p.receivesShadow});
        if(withBounds)groups_[found->second].bounds.push_back(bounds[drawIndex]);
    }
    if(InstanceCapacity()>2*rhi::MaximumFrameInstances)
        for(auto& group:groups_)std::vector<rhi::InstanceParameters>(group.instances.begin(),group.instances.end()).swap(group.instances);
    return {groups_.data(),active_};
}
size_t PbrInstanceBatcher::InstanceCapacity() const
{size_t count=0;for(const auto& group:groups_)count+=group.instances.capacity();return count;}
}
