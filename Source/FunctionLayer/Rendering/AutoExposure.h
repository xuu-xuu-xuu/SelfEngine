#pragma once
#include <array>
#include <cstdint>
#include <span>

namespace se
{
// G15：自动曝光的 CPU 参考。256 桶 log2 亮度直方图、60%~80% 分位目标、
// 亮/暗两条适应速度与上下限钳制；全黑/全白/非有限输入保持稳定。
inline constexpr std::uint32_t ExposureHistogramBins=256;

struct ExposureSettings
{
    float histogramMinLog2=-12.0f,histogramMaxLog2=4.0f;
    float lowPercentile=0.60f,highPercentile=0.80f;
    float brightAdaptation=3.0f,darkAdaptation=1.0f; // 每秒
    float minimumExposure=1.0f/1024.0f,maximumExposure=64.0f;
    float keyValue=0.18f;
};

struct ExposureHistogram
{
    std::array<std::uint32_t,ExposureHistogramBins> bins{};
    std::uint32_t pixels=0;
};

// 非有限或 <=0 的亮度进入最低桶；每像素一次，避免多 Pass 重复统计。
ExposureHistogram BuildExposureHistogram(std::span<const float> luminance,const ExposureSettings& settings);
// 返回给定累计分位对应的亮度（桶中心反 log2）；空直方图返回 0。
float PercentileLuminance(const ExposureHistogram& histogram,float percentile,const ExposureSettings& settings);
// 返回 low/high 分位之间的几何中点，作为 60%~80% 目标亮度。
float TargetLuminance(const ExposureHistogram& histogram,const ExposureSettings& settings);

class AutoExposure
{
public:
    void Reset(float exposure=1.0f) noexcept;
    // 返回是否用有效直方图更新；空/非法直方图只做钳制并保持稳定。
    bool Update(const ExposureHistogram& histogram,float deltaSeconds,const ExposureSettings& settings,float& exposureOut) noexcept;
    float Adapted() const noexcept {return adapted_;}
    float Target() const noexcept {return target_;}
    bool Initialized() const noexcept {return initialized_;}
private:
    float adapted_=1.0f,target_=1.0f;
    bool initialized_=false;
};
}
