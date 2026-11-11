#pragma once
#include "CoreLayer/Math/Projection.h"
#include "ResourceLayer/Assets/LodBake.h"

namespace se
{
// G05：LOD 选择只依赖“世界误差 → 屏幕像素误差”，不绑定相机/渲染实现。
struct LodView
{
    Vec3 position{};
    float verticalFov=Pi/3;
    float viewportHeight=1080;
    float nearPlane=0.1f;
};
// 屏幕误差（像素）：worldError * (viewportHeight/2) / (tan(fov/2) * distance)。
float ProjectedLodError(float worldError,const Vec3& meshCenter,const LodView& view);
// 选“误差仍可接受的最粗级”；1.25× 迟滞：变粗需 ≤threshold/1.25，变细需当前 >threshold×1.25。
std::uint32_t SelectLodLevel(const LodChain& chain,const Vec3& meshCenter,const LodView& view,
    float thresholdPixels,std::uint32_t currentLevel);

// G05.02：LOD 切换的 dither 时间轴。渲染端按 blend 与 4×4 Bayer 阈值做像素级混合。
struct LodTransition
{
    std::uint32_t current=0,previous=0;
    float elapsed=0,duration=0.15f;
    bool active=false;
};
// 选择目标级别并驱动过渡；返回当前显示的级别（过渡期间不立刻切换）。
std::uint32_t UpdateLodTransition(const LodChain& chain,const Vec3& meshCenter,const LodView& view,
    float thresholdPixels,float deltaSeconds,LodTransition& state);
// 0=旧级别、1=新级别；过渡结束后恒为 1。
float LodBlendWeight(const LodTransition& state);
// G05.05：世界变换后的模型选择——局部包围球中心经 world 变换后用于屏幕误差。
std::uint32_t UpdateModelLod(const LodChain& chain,const Vec3& localCenter,const Mat4& world,
    const LodView& view,float thresholdPixels,float deltaSeconds,LodTransition& state);
// G05.03：4×4 Bayer 阈值（(index+0.5)/16）与逐像素选择；true 表示该像素仍取旧级。
float Bayer4x4Threshold(std::uint32_t x,std::uint32_t y) noexcept;
bool LodDitherUsesPrevious(float blend,std::uint32_t x,std::uint32_t y) noexcept;
}
