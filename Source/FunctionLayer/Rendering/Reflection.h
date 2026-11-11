#pragma once
#include "FunctionLayer/Rendering/AmbientOcclusion.h"
#include "CoreLayer/Math/Projection.h"
#include <span>
#include <vector>

namespace se
{
// G12 原型：反射探针（OBB box projection、最近 2 探针混合、GGX 预滤波）与屏幕空间反射 CPU 参考。
struct ReflectionProbe
{
    Vec3 center{},extents{1,1,1};
    float radius=1.0f,blendDistance=0.5f;
    std::vector<Vec3> environment; // equirect RGB，width*height。
    std::uint32_t width=0,height=0;
};
// 把反射方向修正到探针盒表面；位置在盒内/盒外均可，退化盒返回 false。
bool BoxProjectReflection(Vec3 center,Vec3 extents,Vec3 position,Vec3 reflectDirection,Vec3& output);
// 按影响半径选最近 2 个探针并混合；都不影响时返回 false（调用方回退全局环境）。
bool BlendReflectionProbes(std::span<const ReflectionProbe> probes,Vec3 position,Vec3 reflectDirection,
    Vec3& color,std::uint32_t& first,std::uint32_t& second,float& blend);
// GGX 重要性采样预滤波 equirect 环境；roughness 0 接近源，1 更平滑；常量环境保持常量。
bool PrefilterGgx(std::span<const Vec3> equirect,std::uint32_t width,std::uint32_t height,float roughness,
    std::uint32_t samples,std::vector<Vec3>& output);
struct SsrSettings
{
    std::uint32_t steps=32;
    float thickness=0.05f,bias=0.02f,maxDistance=50.0f;
};
struct SsrHit
{
    bool hit=false;
    float u=0,v=0,depth=0,confidence=0;
};
// 视空间反射射线在屏幕空间步进；命中返回屏幕 UV 与置信度，任何越界/背面返回未命中。
bool TraceScreenSpaceReflection(const ScreenSpaceFrame& frame,const SsrSettings& settings,std::uint32_t pixelX,
    std::uint32_t pixelY,Vec3 reflectDirectionView,SsrHit& hit);
}
