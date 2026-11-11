#pragma once
#include "CoreLayer/Math/Projection.h"
#include <cstdint>
#include <span>
#include <vector>

namespace se
{
// G10 原型：CPU 屏幕空间环境光遮蔽与接触阴影参考。输入为视空间深度（正距离）与视空间法线，
// 输出 [0,1] 的 AO/可见度；GTAO 只调制间接光、接触阴影只调制直接光，二者可独立关闭。
struct GtaoSettings
{
    float radius=0.6f,intensity=0.8f,bias=0.02f;
    std::uint32_t slices=4,steps=8;
};
struct ContactShadowSettings
{
    float length=0.5f,bias=0.02f,fade=0.1f;
    std::uint32_t steps=12;
};
struct ScreenSpaceFrame
{
    std::uint32_t width=0,height=0;
    float tanHalfFovY=0.57735026f,aspect=1.0f;
    std::vector<float> viewDepth;   // 正距离。
    std::vector<Vec3> viewNormal;
};
// 失败（尺寸/深度不一致、非法参数）返回 false 且不替换 output。
bool ComputeGtao(const ScreenSpaceFrame& frame,const GtaoSettings& settings,std::vector<float>& output);
// 12 步屏幕空间接触阴影；lightDirectionView 指向光源（视空间，非零）。
bool ComputeContactShadows(const ScreenSpaceFrame& frame,const ContactShadowSettings& settings,
    const Vec3& lightDirectionView,std::vector<float>& output);
// 5×5 深度/法线双边升采样：half 为半分辨率 AO，full 提供全分辨率深度/法线。
bool UpsampleBilateral(const std::vector<float>& half,std::uint32_t halfWidth,std::uint32_t halfHeight,
    const ScreenSpaceFrame& full,std::vector<float>& output);
// 合成：间接光乘 AO，直接光乘接触可见度；两者为 1 时结果不变。
void ComposeScreenSpaceOcclusion(std::span<const float> ao,std::span<const float> contact,std::vector<float>& output);
}
