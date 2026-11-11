#pragma once
#include "FunctionLayer/Rendering/StaticModelRender.h"
#include "FunctionLayer/Rendering/MaterialVariants.h"
#include <map>
#include <memory>

namespace se
{
// G02：材质实例是 CPU 侧“父材质 + 覆盖值”，解析后得到新的只读 PbrRenderMaterial；
// 父材质不被修改，纹理由实例与父材质共享。
struct MaterialInstance
{
    std::shared_ptr<const PbrRenderMaterial> parent;
    Vec4 baseColor{1,1,1,1};
    float metallic=0,roughness=1,normalScale=1;
    bool normalMapping=false,doubleSided=false;
    std::array<rhi::Texture,3> textures{}; // 空槽沿用父材质
    std::uint32_t addFeatures=0,removeFeatures=0;
};

inline std::uint32_t FeatureMaskFor(const PbrRenderMaterial& material) noexcept
{
    std::uint32_t mask=0;
    if(material.baseColor)mask|=MaterialFeatureMask(MaterialFeature::AlbedoMap);
    if(material.normal)mask|=MaterialFeatureMask(MaterialFeature::NormalMap);
    if(material.metallicRoughness)mask|=MaterialFeatureMask(MaterialFeature::MetallicRoughnessMap);
    if(material.doubleSided)mask|=MaterialFeatureMask(MaterialFeature::DoubleSided);
    return mask;
}

inline std::shared_ptr<const PbrRenderMaterial> ResolveMaterialInstance(const MaterialInstance& instance)
{
    if(!instance.parent)return {};
    auto resolved=std::make_shared<PbrRenderMaterial>(*instance.parent);
    resolved->color=instance.baseColor;
    resolved->metallic=instance.metallic;
    resolved->roughness=instance.roughness;
    resolved->normalScale=instance.normalScale;
    resolved->normalMapping=instance.normalMapping;
    resolved->doubleSided=instance.doubleSided;
    if(instance.textures[0])resolved->baseColor=instance.textures[0];
    if(instance.textures[1])resolved->normal=instance.textures[1];
    if(instance.textures[2])resolved->metallicRoughness=instance.textures[2];
    resolved->featureMask=(instance.parent->featureMask|instance.addFeatures)&~instance.removeFeatures;
    return resolved;
}

// G02：场景内实际出现的变体清单；只统计，不创建 PSO。
class VariantInventory
{
public:
    bool Add(std::uint32_t mask,std::uint32_t count=1)
    {
        const bool fresh=counts_.find(mask)==counts_.end();
        counts_[mask]+=count;
        return fresh;
    }
    std::size_t Distinct() const noexcept {return counts_.size();}
    std::uint32_t Total() const noexcept
    {
        std::uint32_t total=0;
        for(const auto& entry:counts_)total+=entry.second;
        return total;
    }
    const std::map<std::uint32_t,std::uint32_t>& Counts() const noexcept {return counts_;}
private:
    std::map<std::uint32_t,std::uint32_t> counts_;
};
}
