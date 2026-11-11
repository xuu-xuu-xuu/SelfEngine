#include "AutoExposure.h"
#include <algorithm>
#include <cmath>

namespace se
{
namespace
{
float ClampRange(float value,float low,float high){return std::clamp(value,low,high);}
std::uint32_t BucketFor(float luminance,const ExposureSettings& settings)
{
    const auto minimum=std::min(settings.histogramMinLog2,settings.histogramMaxLog2);
    const auto maximum=std::max(settings.histogramMinLog2,settings.histogramMaxLog2);
    const auto span=std::max(1e-6f,maximum-minimum);
    const auto safe=std::isfinite(luminance)?std::max(luminance,1e-9f):1e-9f;
    const auto logValue=ClampRange(std::log2(safe),minimum,maximum);
    const auto normalized=(logValue-minimum)/span;
    const auto index=static_cast<std::uint32_t>(normalized*static_cast<float>(ExposureHistogramBins));
    return std::min(ExposureHistogramBins-1,index);
}
float BucketLuminance(std::uint32_t bucket,const ExposureSettings& settings)
{
    const auto minimum=std::min(settings.histogramMinLog2,settings.histogramMaxLog2);
    const auto maximum=std::max(settings.histogramMinLog2,settings.histogramMaxLog2);
    const auto span=std::max(1e-6f,maximum-minimum);
    const auto logValue=minimum+(static_cast<float>(bucket)+0.5f)/static_cast<float>(ExposureHistogramBins)*span;
    return std::exp2(logValue);
}
}
ExposureHistogram BuildExposureHistogram(std::span<const float> luminance,const ExposureSettings& settings)
{
    ExposureHistogram histogram;
    for(const auto value:luminance)
    {
        if(!std::isfinite(value))continue;
        ++histogram.bins[BucketFor(value,settings)];
        ++histogram.pixels;
    }
    return histogram;
}
float PercentileLuminance(const ExposureHistogram& histogram,float percentile,const ExposureSettings& settings)
{
    if(!histogram.pixels||!std::isfinite(percentile))return 0.0f;
    const auto target=std::max<std::uint64_t>(1,static_cast<std::uint64_t>(
        std::ceil(static_cast<double>(std::clamp(percentile,0.0f,1.0f))*histogram.pixels)));
    std::uint64_t cumulative=0;
    for(std::uint32_t bucket=0;bucket<ExposureHistogramBins;++bucket)
    {
        cumulative+=histogram.bins[bucket];
        if(cumulative>=target)return BucketLuminance(bucket,settings);
    }
    return BucketLuminance(ExposureHistogramBins-1,settings);
}
float TargetLuminance(const ExposureHistogram& histogram,const ExposureSettings& settings)
{
    const auto low=PercentileLuminance(histogram,std::min(settings.lowPercentile,settings.highPercentile),settings);
    const auto high=PercentileLuminance(histogram,std::max(settings.lowPercentile,settings.highPercentile),settings);
    if(!(low>0.0f))return high;
    if(!(high>0.0f))return low;
    return std::sqrt(low*high);
}
void AutoExposure::Reset(float exposure) noexcept
{
    adapted_=std::isfinite(exposure)&&exposure>0.0f?exposure:1.0f;
    target_=adapted_;
    initialized_=false;
}
bool AutoExposure::Update(const ExposureHistogram& histogram,float deltaSeconds,const ExposureSettings& settings,float& exposureOut) noexcept
{
    if(histogram.pixels==0)
    {
        exposureOut=adapted_;
        return false;
    }
    const auto targetLuminance=TargetLuminance(histogram,settings);
    if(!(targetLuminance>0.0f)||!std::isfinite(targetLuminance))
    {
        exposureOut=adapted_;
        return false;
    }
    const auto minimum=std::isfinite(settings.minimumExposure)&&settings.minimumExposure>0.0f?settings.minimumExposure:1.0f/1024.0f;
    const auto maximum=std::isfinite(settings.maximumExposure)&&settings.maximumExposure>0.0f?settings.maximumExposure:64.0f;
    const auto low=std::min(minimum,maximum),high=std::max(minimum,maximum);
    const auto key=std::isfinite(settings.keyValue)&&settings.keyValue>0.0f?settings.keyValue:0.18f;
    target_=ClampRange(key/targetLuminance,low,high);
    if(!initialized_)
    {
        adapted_=target_;
        initialized_=true;
        exposureOut=adapted_;
        return true;
    }
    const auto delta=std::isfinite(deltaSeconds)?std::clamp(deltaSeconds,0.0f,0.5f):0.0f;
    const auto speed=target_>adapted_?std::max(0.0f,settings.brightAdaptation):std::max(0.0f,settings.darkAdaptation);
    adapted_+=(target_-adapted_)*(1.0f-std::exp(-delta*speed));
    adapted_=ClampRange(adapted_,low,high);
    exposureOut=adapted_;
    return true;
}
}
