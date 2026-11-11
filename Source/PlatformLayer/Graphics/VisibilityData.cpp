#include "VisibilityData.h"
#include <immintrin.h>
#include <cmath>
#include <limits>

namespace se
{
namespace
{
struct FpScope
{
    unsigned saved=_mm_getcsr();
    FpScope(){_mm_setcsr((saved&~_MM_ROUND_MASK)|_MM_ROUND_NEAREST|_MM_FLUSH_ZERO_ON|_MM_DENORMALS_ZERO_ON|_MM_MASK_MASK);}
    ~FpScope(){_mm_setcsr(saved);}
};
}
bool ValidVisibilityFrustum(const VisibilityFrustum& value)
{
    for(const auto& p:value.planes)
    {
        for(float x:p)if(!std::isfinite(x))return false;
        const double squared=double(p[0])*p[0]+double(p[1])*p[1]+double(p[2])*p[2];
        if(squared<1e-40)return false;
    }
    return true;
}
bool QuantizeVisibilityFrustum(const Frustum& source,VisibilityFrustum& output)
{
    FpScope fp;
    VisibilityFrustum candidate;
    for(std::size_t i=0;i<6;++i)
    {
        const auto& p=source.planes[i];
        candidate.planes[i]={static_cast<float>(p.x),static_cast<float>(p.y),static_cast<float>(p.z),static_cast<float>(p.d)};
    }
    if(!ValidVisibilityFrustum(candidate))return false;output=candidate;return true;
}
namespace
{
bool Visible(const VisibilityItem& item,const VisibilityFrustum& frustum)
{
    if(!IsValid(Aabb{item.minimum,item.maximum}))return true;
    for(const auto& p:frustum.planes)
    {
        const float x=p[0]*(p[0]>=0?item.maximum.x:item.minimum.x);
        const float y=p[1]*(p[1]>=0?item.maximum.y:item.minimum.y);
        const float z=p[2]*(p[2]>=0?item.maximum.z:item.minimum.z);
        float distance=x+y;distance=distance+z;distance=distance+p[3];
        float magnitude=std::abs(x)+std::abs(y);magnitude=magnitude+std::abs(z);magnitude=magnitude+std::abs(p[3]);
        const float error=(8*std::numeric_limits<float>::epsilon())*magnitude;
        const float margin=1e-5f+error;
        if(std::isfinite(distance)&&std::isfinite(margin)&&distance < -margin)return false;
    }
    return true;
}
}
bool CullVisibilityReference(std::span<const VisibilityItem> items,const VisibilityFrustum& frustum,std::vector<std::uint32_t>& output)
{
    // 与SM5浮点算术一致；验证和分配失败也恢复调用线程原始控制字。
    FpScope fp;
    if(items.size()>MaximumVisibilityItems||!ValidVisibilityFrustum(frustum))return false;
    // reserve失败保持旧内容；有效输入随后不会再分配，正常基准复用CPU容量。
    output.reserve(items.size());output.clear();
    for(const auto& item:items)if(Visible(item,frustum))output.push_back(item.id);
    return true;
}
}
