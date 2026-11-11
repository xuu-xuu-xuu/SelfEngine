#pragma once
#include "PlatformLayer/Graphics/Rhi.h"
#include <cstdint>
#include <string>
#include <vector>

namespace se
{
// G02：材质变体位。位一旦发布不得复用；新增位只能追加，并同步登记到名称表。
enum class MaterialFeature : std::uint32_t
{
    AlbedoMap = 1u << 0,
    NormalMap = 1u << 1,
    MetallicRoughnessMap = 1u << 2,
    Emissive = 1u << 3,
    AlphaTest = 1u << 4,
    Skinned = 1u << 5,
    ReceiveShadow = 1u << 6,
    Instanced = 1u << 7,
    VertexCompressed = 1u << 8,
    DoubleSided = 1u << 9,
};

struct MaterialFeatureName
{
    MaterialFeature feature;
    const char* macro;
};

inline constexpr MaterialFeatureName MaterialFeatureNames[] =
{
    {MaterialFeature::AlbedoMap,"SE_FEATURE_ALBEDO_MAP"},
    {MaterialFeature::NormalMap,"SE_FEATURE_NORMAL_MAP"},
    {MaterialFeature::MetallicRoughnessMap,"SE_FEATURE_METALLIC_ROUGHNESS_MAP"},
    {MaterialFeature::Emissive,"SE_FEATURE_EMISSIVE"},
    {MaterialFeature::AlphaTest,"SE_FEATURE_ALPHA_TEST"},
    {MaterialFeature::Skinned,"SE_FEATURE_SKINNED"},
    {MaterialFeature::ReceiveShadow,"SE_FEATURE_RECEIVE_SHADOW"},
    {MaterialFeature::Instanced,"SE_FEATURE_INSTANCED"},
    {MaterialFeature::VertexCompressed,"SE_FEATURE_VERTEX_COMPRESSED"},
    {MaterialFeature::DoubleSided,"SE_FEATURE_DOUBLE_SIDED"},
};

inline constexpr std::uint32_t MaterialFeatureMask(MaterialFeature feature) noexcept
{
    return static_cast<std::uint32_t>(feature);
}

// 返回按名称表顺序排列的宏；未设置的位不产生宏，重复调用结果稳定。
inline std::vector<rhi::ShaderMacro> VariantMacros(std::uint32_t mask)
{
    std::vector<rhi::ShaderMacro> macros;
    for(const auto& entry:MaterialFeatureNames)
        if(mask&MaterialFeatureMask(entry.feature))macros.push_back({entry.macro,"1"});
    return macros;
}

// 稳定的变体标识：用于日志、缓存键和统计，不参与 Shader 语义。
inline std::string VariantId(std::uint32_t mask)
{
    static constexpr char digits[]="0123456789abcdef";
    std::string id(8,'0');
    for(int index=7;index>=0;--index){id[static_cast<std::size_t>(index)]=digits[mask&0xF];mask>>=4;}
    return id;
}
}
