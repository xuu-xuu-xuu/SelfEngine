#pragma once
#include "CoreLayer/Math/Bounds.h"
#include <cstddef>
#include <cstdint>
#include <span>
#include <vector>

namespace se
{
inline constexpr std::uint32_t MaximumVisibilityItems=65536;
struct VisibilityItem
{
    Vec3 minimum;std::uint32_t id=0;
    Vec3 maximum;std::uint32_t reserved=0;
};
static_assert(sizeof(VisibilityItem)==32&&offsetof(VisibilityItem,id)==12&&offsetof(VisibilityItem,maximum)==16);
struct VisibilityFrustum {std::array<std::array<float,4>,6> planes;};
bool ValidVisibilityFrustum(const VisibilityFrustum& value);
bool QuantizeVisibilityFrustum(const Frustum& source,VisibilityFrustum& output);
// 失败不替换 output；成功按输入次序输出，GPU列表以多重集合与其比较。
bool CullVisibilityReference(std::span<const VisibilityItem> items,const VisibilityFrustum& frustum,std::vector<std::uint32_t>& output);
}
