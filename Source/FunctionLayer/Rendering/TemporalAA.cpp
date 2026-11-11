#include "TemporalAA.h"
#include "CoreLayer/Math/Projection.h"
#include <algorithm>
#include <cmath>

namespace se
{
namespace
{
bool Finite(Vec3 value){return std::isfinite(value.x)&&std::isfinite(value.y)&&std::isfinite(value.z);}
float Halton(std::uint32_t index,std::uint32_t base)
{
    float fraction=1.0f,result=0.0f;
    while(index>0)
    {
        fraction/=static_cast<float>(base);
        result+=fraction*static_cast<float>(index%base);
        index/=base;
    }
    return result;
}
Vec3 Lerp(Vec3 first,Vec3 second,float weight){return first*(1.0f-weight)+second*weight;}
}
TaaJitter HaltonJitter(std::uint32_t frameIndex)
{
    const auto phase=frameIndex%8u;
    return {Halton(phase+1u,2u)-0.5f,Halton(phase+1u,3u)-0.5f};
}
Mat4 ApplyProjectionJitter(Mat4 clipFromWorld,TaaJitter jitter,std::uint32_t width,std::uint32_t height)
{
    if(!width||!height||!std::isfinite(jitter.x)||!std::isfinite(jitter.y)
        ||jitter.x<-1.0f||jitter.x>1.0f||jitter.y<-1.0f||jitter.y>1.0f)return clipFromWorld;
    const auto offsetX=2.0f*jitter.x/static_cast<float>(width);
    const auto offsetY=2.0f*jitter.y/static_cast<float>(height);
    for(std::size_t column=0;column<4;++column)
    {
        clipFromWorld(0,column)+=offsetX*clipFromWorld(3,column);
        clipFromWorld(1,column)+=offsetY*clipFromWorld(3,column);
    }
    return clipFromWorld;
}
TaaVelocity ReprojectVelocity(const Mat4& clipFromWorld,const Mat4& previousClipFromWorld,Vec3 worldPosition)
{
    if(!Finite(worldPosition)||!IsFinite(clipFromWorld)||!IsFinite(previousClipFromWorld))return {};
    const Vec4 current=clipFromWorld*Vec4{worldPosition.x,worldPosition.y,worldPosition.z,1.0f};
    const Vec4 previous=previousClipFromWorld*Vec4{worldPosition.x,worldPosition.y,worldPosition.z,1.0f};
    if(!(current.w>1e-6f)||!(previous.w>1e-6f)||!IsFinite(current)||!IsFinite(previous))return {};
    const auto currentX=current.x/current.w,currentY=current.y/current.w;
    const auto previousX=previous.x/previous.w,previousY=previous.y/previous.w;
    // 屏幕 UV 使用左上原点，v 随 NDC y 反向。
    const auto currentU=currentX*0.5f+0.5f,currentV=0.5f-currentY*0.5f;
    const auto previousU=previousX*0.5f+0.5f,previousV=0.5f-previousY*0.5f;
    TaaVelocity velocity;velocity.valid=std::isfinite(currentU)&&std::isfinite(currentV)&&std::isfinite(previousU)&&std::isfinite(previousV);
    velocity.du=previousU-currentU;velocity.dv=previousV-currentV;
    return velocity;
}
Vec3 RgbToYCoCg(Vec3 color)
{
    return {0.25f*color.x+0.5f*color.y+0.25f*color.z,0.5f*color.x-0.5f*color.z,-0.25f*color.x+0.5f*color.y-0.25f*color.z};
}
Vec3 YCoCgToRgb(Vec3 color)
{
    return {color.x+color.y-color.z,color.x+color.z,color.x-color.y-color.z};
}
void YCoCgNeighborhood(std::span<const Vec3,9> neighborhood,Vec3& minimum,Vec3& maximum)
{
    minimum=maximum=RgbToYCoCg(neighborhood[0]);
    for(std::size_t index=1;index<neighborhood.size();++index)
    {
        const auto value=RgbToYCoCg(neighborhood[index]);
        minimum={std::min(minimum.x,value.x),std::min(minimum.y,value.y),std::min(minimum.z,value.z)};
        maximum={std::max(maximum.x,value.x),std::max(maximum.y,value.y),std::max(maximum.z,value.z)};
    }
}
Vec3 ClampToNeighborhood(Vec3 history,std::span<const Vec3,9> neighborhood)
{
    Vec3 minimum{},maximum{};
    YCoCgNeighborhood(neighborhood,minimum,maximum);
    auto value=RgbToYCoCg(history);
    value.x=std::clamp(value.x,minimum.x,maximum.x);
    value.y=std::clamp(value.y,minimum.y,maximum.y);
    value.z=std::clamp(value.z,minimum.z,maximum.z);
    return YCoCgToRgb(value);
}
TaaResolveResult ResolveTaa(const TaaResolveInput& input,const TaaSettings& settings)
{
    TaaResolveResult result;result.color=input.neighborhood[4];
    if(!settings.enabled||input.transparentOrUi)return result; // 关闭 TAA 时由 FXAA 回退；透明/UI 不进入历史。
    if(!input.historyValid||!input.velocity.valid)
    {
        result.disoccluded=true;
        return result;
    }
    const auto speed=std::sqrt(input.velocity.du*input.velocity.du+input.velocity.dv*input.velocity.dv);
    if(speed>settings.velocityThreshold||std::abs(input.currentDepth-input.historyDepth)>settings.depthThreshold)
    {
        result.disoccluded=true;
        return result;
    }
    const auto clamped=ClampToNeighborhood(input.history,input.neighborhood);
    const auto alpha=std::clamp(speed>settings.velocityThreshold*0.25f?settings.dynamicBlend:settings.staticBlend,0.0f,1.0f);
    result.color=Lerp(clamped,input.neighborhood[4],alpha);
    result.historyWeight=1.0f-alpha;
    if(settings.sharpen>0.0f)
    {
        Vec3 average{};
        for(const auto& sample:input.neighborhood)average=average+sample;
        average=average*(1.0f/9.0f);
        result.color=result.color+(input.neighborhood[4]-average)*settings.sharpen;
    }
    return result;
}
void TaaHistory::Reset() noexcept
{
    width_=height_=0;
    valid_=false;
}
void TaaHistory::Resize(std::uint32_t width,std::uint32_t height) noexcept
{
    width_=width;
    height_=height;
    valid_=false;
}
void TaaHistory::Invalidate() noexcept {valid_=false;}
void TaaHistory::Confirm() noexcept {valid_=width_!=0&&height_!=0;}
bool TaaHistory::Usable(std::uint32_t width,std::uint32_t height) const noexcept
{
    return valid_&&width==width_&&height==height_&&width_!=0&&height_!=0;
}
}
