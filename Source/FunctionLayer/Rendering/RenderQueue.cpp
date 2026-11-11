#include "RenderQueue.h"
#include <algorithm>
#include <bit>

namespace se
{
OpaqueSortKey BuildOpaqueSortKey(const rhi::DrawIndexed& draw)
{
    OpaqueSortKey key;size_t index=0;key.words[index++]=static_cast<std::uint64_t>(rhi::PassKind::Opaque);
    const auto handle=[&](const auto& value){key.words[index++]=value.device;key.words[index++]=(std::uint64_t(value.index)<<32)|value.generation;};
    const auto scalar=[&](float value){key.words[index++]=std::bit_cast<std::uint32_t>(value);};
    handle(draw.pipeline);handle(draw.texture);handle(draw.normalTexture);handle(draw.metallicRoughnessTexture);
    scalar(draw.tint.x);scalar(draw.tint.y);scalar(draw.tint.z);scalar(draw.tint.w);
    scalar(draw.pbr.metallic);scalar(draw.pbr.roughness);scalar(draw.pbr.normalScale);
    key.words[index++]=(draw.textureIsData?1u:0u)|(draw.unlit?2u:0u)|(draw.pbr.normalMapping?4u:0u)|(draw.pbr.doubleSided?8u:0u)|(draw.pbr.receivesShadow?16u:0u);
    handle(draw.vertices);handle(draw.indices);return key;
}
void SortOpaqueDraws(std::vector<rhi::DrawIndexed>& draws)
{
    struct Item{OpaqueSortKey key;rhi::DrawIndexed draw;};std::vector<Item> items;items.reserve(draws.size());
    for(const auto& draw:draws)items.push_back({BuildOpaqueSortKey(draw),draw});
    std::stable_sort(items.begin(),items.end(),[](const auto& a,const auto& b){return a.key<b.key;});
    for(size_t index=0;index<items.size();++index)draws[index]=std::move(items[index].draw);
}
}
