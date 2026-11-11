#include "CascadeShadow.h"
#include <algorithm>
#include <cmath>

namespace se
{
namespace
{
constexpr std::array<float,CascadeShadowMaximumCascades> NormalizedSplits{0.10f,0.35f,1.0f};
bool ValidDepth(float value){return std::isfinite(value)&&value>0;}
Vec3 Unproject(const Mat4& inverse,float x,float y,float z)
{
    const Vec4 clip{x,y,z,1};
    const auto world=inverse*clip;
    if(!(std::abs(world.w)>1e-6f)||!std::isfinite(world.w))return {};
    return {world.x/world.w,world.y/world.w,world.z/world.w};
}
}
bool PrepareCascadeShadows(const Mat4& viewProjection,float nearPlane,float farPlane,const Vec3& directionToLight,
    std::uint32_t cascadeCount,std::uint32_t resolution,CascadeShadowSettings& output)
{
    if(cascadeCount<1||cascadeCount>CascadeShadowMaximumCascades||resolution<256||resolution>4096
        ||!ValidDepth(nearPlane)||!ValidDepth(farPlane)||farPlane<=nearPlane||!IsFinite(viewProjection)
        ||!IsFinite(directionToLight))return false;
    const auto inverse=Inverse(viewProjection);if(!inverse)return false;
    const auto z=Normalize(directionToLight);if(!(LengthSquared(z)>0.99f))return false;
    const auto nearCenter=Unproject(*inverse,0,0,0),farCenter=Unproject(*inverse,0,0,1);
    const auto forward=Normalize(farCenter-nearCenter);
    if(!(LengthSquared(forward)>0.99f))return false;
    const auto camera=nearCenter-forward*nearPlane;
    std::array<Vec3,4> farCorners{
        Unproject(*inverse,-1,-1,1),Unproject(*inverse,1,-1,1),Unproject(*inverse,1,1,1),Unproject(*inverse,-1,1,1)};
    for(const auto& corner:farCorners)if(!IsFinite(corner))return false;
    const Vec3 up=std::abs(z.y)>0.95f?Vec3{0,0,1}:Vec3{0,1,0};
    const auto x=Normalize(Cross(up,z)),y=Cross(z,x);
    CascadeShadowSettings candidate;candidate.cascadeCount=cascadeCount;candidate.resolution=resolution;
    float previousSplit=nearPlane;
    for(std::uint32_t cascade=0;cascade<cascadeCount;++cascade)
    {
        const auto split=nearPlane+(farPlane-nearPlane)*NormalizedSplits[cascade];
        std::array<Vec3,8> corners{};
        for(std::size_t index=0;index<farCorners.size();++index)
        {
            const auto direction=farCorners[index]-camera;
            corners[index]=camera+direction*(previousSplit/farPlane);
            corners[index+4]=camera+direction*(split/farPlane);
        }
        Vec3 center{0,0,0};for(const auto& corner:corners)center=center+corner;center=center/8.0f;
        float radius=0;for(const auto& corner:corners)radius=std::max(radius,Length(corner-center));
        if(!(radius>0)||!std::isfinite(radius))return false;
        const double texel=2.0*radius/double(resolution);
        if(!(texel>0))return false;
        const double coordinateX=double(Dot(center,x)),coordinateY=double(Dot(center,y));
        const auto snappedX=float(std::round(coordinateX/texel)*texel);
        const auto snappedY=float(std::round(coordinateY/texel)*texel);
        const auto snappedCenter=center+x*(snappedX-float(coordinateX))+y*(snappedY-float(coordinateY));
        const auto lightDistance=2.0f*radius+1.0f;
        const auto position=snappedCenter+z*lightDistance;
        Mat4 world;
        for(std::size_t row=0;row<3;++row)
        {
            const float a[]{x.x,x.y,x.z},b[]{y.x,y.y,y.z},c[]{z.x,z.y,z.z},p[]{position.x,position.y,position.z};
            world(row,0)=a[row];world(row,1)=b[row];world(row,2)=c[row];world(row,3)=p[row];
        }
        const auto lightInverse=InverseRigid(world);
        const auto projection=OrthographicRH(radius*2,radius*2,0.1f,lightDistance+radius*2);
        if(!lightInverse||!projection)return false;
        candidate.clipFromWorld[cascade]=*projection * *lightInverse;
        if(!IsFinite(candidate.clipFromWorld[cascade]))return false;
        candidate.splitDistances[cascade]=split;
        candidate.sphereCenter[cascade]=snappedCenter;
        candidate.sphereRadius[cascade]=radius;
        previousSplit=split;
    }
    candidate.blendFraction=0.10f;
    output=candidate;return true;
}
CascadeSelection SelectCascade(const CascadeShadowSettings& settings,float viewDepth)
{
    CascadeSelection selection;
    if(settings.cascadeCount==0)return selection;
    const auto last=settings.cascadeCount-1;
    if(!std::isfinite(viewDepth)||viewDepth>=settings.splitDistances[last])
    {selection.first=selection.second=last;return selection;}
    const auto fraction=std::clamp(settings.blendFraction,0.0f,0.5f);
    // 每个级边界两侧各留一半重叠带：split-w .. split+w，w=10% 的级跨度。
    for(std::uint32_t index=0;index<last;++index)
    {
        const auto previous=index?settings.splitDistances[index-1]:0.0f;
        const auto width=(settings.splitDistances[index]-previous)*fraction;
        if(width<=1e-6f)continue;
        if(viewDepth>=settings.splitDistances[index]-width&&viewDepth<=settings.splitDistances[index]+width)
        {
            selection.first=index;selection.second=index+1;
            selection.blend=std::clamp((viewDepth-(settings.splitDistances[index]-width))/(2*width),0.0f,1.0f);
            return selection;
        }
    }
    std::uint32_t index=0;
    while(index<last&&viewDepth>settings.splitDistances[index])++index;
    selection.first=selection.second=index;return selection;
}
}
