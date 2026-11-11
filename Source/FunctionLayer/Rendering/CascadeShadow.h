#pragma once
#include "CoreLayer/Math/Projection.h"
#include <array>
#include <cstdint>

namespace se
{
// G08 原型：3 级 CSM 的 CPU 拟合。级划分使用计划中的归一化距离 {0.10,0.35,1.0}
// （near+ (far-near)*t，near/far=0.1/100 时为 10/35/100 m）；每级对切片 8 角点拟合包围球，
// 在光空间按 texel 取整后构造正交投影，主 Pass 用 SelectCascade 选级与 10% 重叠混合。
inline constexpr std::uint32_t CascadeShadowMaximumCascades=3;
struct CascadeShadowSettings
{
    std::uint32_t cascadeCount=0,resolution=0;
    std::array<Mat4,CascadeShadowMaximumCascades> clipFromWorld{};
    std::array<float,CascadeShadowMaximumCascades> splitDistances{};
    std::array<Vec3,CascadeShadowMaximumCascades> sphereCenter{};
    std::array<float,CascadeShadowMaximumCascades> sphereRadius{};
    float blendFraction=0.10f;
};
// 失败不替换 output；cascadeCount 只接受 1..3。
bool PrepareCascadeShadows(const Mat4& viewProjection,float nearPlane,float farPlane,const Vec3& directionToLight,
    std::uint32_t cascadeCount,std::uint32_t resolution,CascadeShadowSettings& output);
struct CascadeSelection
{
    std::uint32_t first=0,second=0;
    float blend=0; // 0=只用 first，1=只用 second。
};
// viewDepth 为沿相机前向的正距离；超出最后一级时钳制到最后一级。
CascadeSelection SelectCascade(const CascadeShadowSettings& settings,float viewDepth);
}
