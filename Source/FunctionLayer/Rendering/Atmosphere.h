#pragma once
#include "CoreLayer/Math/Projection.h"
#include <cstdint>
#include <vector>

namespace se
{
// G11 原型：球面大气的 CPU 参考。透射率 LUT 只在参数变化时重建；天空为 16 段单次散射 + 8 段太阳光程。
struct AtmosphereSettings
{
    Vec3 sunDirection{0,1,0}; // 指向太阳的单位向量。
    float sunIntensity=20.0f;
    float planetRadius=6371000.0f;
    float atmosphereHeight=100000.0f;
    float rayleighScaleHeight=8000.0f;
    float mieScaleHeight=1200.0f;
    float rayleighScattering=5.8e-6f;
    float mieScattering=2.0e-5f;
    float mieExtinction=2.5e-5f;
    float turbidity=1.0f;
};
class Atmosphere
{
public:
    // 参数非法返回 false 且不替换旧状态；参数未变化时复用缓存（RebuildCount 不变）。
    bool Rebuild(const AtmosphereSettings& settings);
    bool Ready() const noexcept {return ready_;}
    float TransmittanceToSun(float altitude,float cosZenith) const;
    Vec3 SkyRadiance(Vec3 viewDirection) const;
    const AtmosphereSettings& Settings() const noexcept {return settings_;}
    std::uint32_t RebuildCount() const noexcept {return rebuilds_;}
    static constexpr std::uint32_t LutWidth=64,LutHeight=32;
private:
    bool Same(const AtmosphereSettings& other) const;
    float Density(float altitude,float rayleigh,float mie) const;
    bool Intersect(float altitude,Vec3 direction,float& nearDistance,float& farDistance) const;
    AtmosphereSettings settings_;
    std::vector<float> transmittance_; // LutWidth*LutHeight，按 altitude/cosZenith 双线性。
    bool ready_=false;
    std::uint32_t rebuilds_=0;
};
// G11：高度雾的解析积分（16 段梯形），返回 [0,1] 透射率；density<=0 时为 1。
struct FogSettings
{
    float density=0.01f,heightFalloff=0.1f,baseHeight=0,anisotropy=0;
    Vec3 color{0.7f,0.8f,0.9f};
};
float FogTransmittance(const FogSettings& settings,float cameraHeight,float distance);
Vec3 IntegrateFog(const FogSettings& settings,Vec3 viewDirection,float cameraHeight,float distance,float& transmittance);
}
