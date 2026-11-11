#include "LodSelect.h"
#include <algorithm>
#include <cmath>

namespace se
{
float ProjectedLodError(float worldError,const Vec3& meshCenter,const LodView& view)
{
    const auto distance=(std::max)(Length(view.position-meshCenter),view.nearPlane);
    const auto tangent=(std::max)(std::tan(view.verticalFov*0.5f),1e-4f);
    return worldError*(view.viewportHeight*0.5f)/(tangent*distance);
}
std::uint32_t SelectLodLevel(const LodChain& chain,const Vec3& meshCenter,const LodView& view,
    float thresholdPixels,std::uint32_t currentLevel)
{
    if(chain.levels.empty())return 0;
    const auto maximum=static_cast<std::uint32_t>(chain.levels.size()-1);
    const auto current=(std::min)(currentLevel,maximum);
    // 误差随级别单调增大：从最粗向下找第一个满足阈值的级别。
    std::uint32_t target=0;
    for(std::uint32_t level=maximum;level>0;--level)
    {
        if(ProjectedLodError(chain.levels[level].worldError,meshCenter,view)<=thresholdPixels)
        {
            target=level;
            break;
        }
    }
    if(target>current)
    {
        // 变粗要更保守：新误差必须 ≤ threshold/1.25，避免在边界来回切换。
        const auto error=ProjectedLodError(chain.levels[target].worldError,meshCenter,view);
        return error*1.25f<=thresholdPixels?target:current;
    }
    if(target<current)
    {
        // 变细要等当前级别明显超标，防止刚切粗又立刻切细。
        const auto currentError=ProjectedLodError(chain.levels[current].worldError,meshCenter,view);
        return currentError>thresholdPixels*1.25f?target:current;
    }
    return current;
}
float LodBlendWeight(const LodTransition& state)
{
    if(!state.active||state.duration<=0)return 1;
    return (std::clamp)(state.elapsed/state.duration,0.0f,1.0f);
}
std::uint32_t UpdateModelLod(const LodChain& chain,const Vec3& localCenter,const Mat4& world,
    const LodView& view,float thresholdPixels,float deltaSeconds,LodTransition& state)
{
    const auto center=TransformPoint(world,localCenter);
    return UpdateLodTransition(chain,center,view,thresholdPixels,deltaSeconds,state);
}
std::uint32_t UpdateLodTransition(const LodChain& chain,const Vec3& meshCenter,const LodView& view,
    float thresholdPixels,float deltaSeconds,LodTransition& state)
{
    const auto target=SelectLodLevel(chain,meshCenter,view,thresholdPixels,state.current);
    if(target!=state.current)
    {
        state.previous=state.current;state.current=target;
        state.elapsed=0;state.active=true;
    }
    else if(state.active)
    {
        state.elapsed+=(std::max)(deltaSeconds,0.0f);
        if(state.elapsed>=state.duration){state.elapsed=state.duration;state.active=false;}
    }
    return state.current;
}
float Bayer4x4Threshold(std::uint32_t x,std::uint32_t y) noexcept
{
    static constexpr std::uint8_t matrix[16]={0,8,2,10,12,4,14,6,3,11,1,9,15,7,13,5};
    return (static_cast<float>(matrix[(y&3u)*4+(x&3u)])+0.5f)/16.0f;
}
bool LodDitherUsesPrevious(float blend,std::uint32_t x,std::uint32_t y) noexcept
{
    return blend<=Bayer4x4Threshold(x,y);
}
}
