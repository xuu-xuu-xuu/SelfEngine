#pragma once
#include "CoreLayer/Math/Math.h"
#include <cstdint>
#include <vector>

namespace se
{
struct MaterialThumbnail
{
    std::uint32_t width = 0, height = 0;
    std::vector<std::uint8_t> rgba8;
};

// 用固定方向光生成一个 64x64 的材质球预览；纯 CPU，不依赖 RHI/窗口。
// baseColor 为线性 RGBA；metallic/roughness 取 [0,1]。
MaterialThumbnail RenderMaterialThumbnail(const Vec4& baseColor, float metallic, float roughness,
    std::uint32_t size = 64);
} // namespace se
