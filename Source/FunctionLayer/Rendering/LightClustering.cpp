#include "LightClustering.h"
#include <algorithm>
#include <cmath>

namespace se
{
namespace
{
bool Finite(Vec3 value){return std::isfinite(value.x)&&std::isfinite(value.y)&&std::isfinite(value.z);}
Vec3 ToVec3(const std::array<float,3>& value){return {value[0],value[1],value[2]};}
}
bool LightClusterZSlice(float viewDepth,float nearPlane,float farPlane,std::uint32_t& slice)
{
    if(!std::isfinite(viewDepth)||!std::isfinite(nearPlane)||!std::isfinite(farPlane)
        ||nearPlane<=0||farPlane<=nearPlane)return false;
    if(viewDepth<=nearPlane){slice=0;return true;}
    if(viewDepth>=farPlane){slice=LightClusterZ-1;return true;}
    const auto ratio=std::log(viewDepth/nearPlane)/std::log(farPlane/nearPlane);
    slice=std::min(LightClusterZ-1,static_cast<std::uint32_t>(ratio*LightClusterZ));
    return true;
}
bool LightClusterIndex(float ndcX,float ndcY,float viewDepth,float nearPlane,float farPlane,std::uint32_t& index)
{
    if(!std::isfinite(ndcX)||!std::isfinite(ndcY)||ndcX< -1||ndcX>1||ndcY< -1||ndcY>1)return false;
    std::uint32_t slice=0;
    if(!LightClusterZSlice(viewDepth,nearPlane,farPlane,slice))return false;
    const auto x=std::min(LightClusterX-1,static_cast<std::uint32_t>((ndcX*0.5f+0.5f)*LightClusterX));
    const auto y=std::min(LightClusterY-1,static_cast<std::uint32_t>((0.5f-ndcY*0.5f)*LightClusterY));
    index=x+y*LightClusterX+slice*LightClusterX*LightClusterY;
    return true;
}
bool BuildLightClusters(std::span<const rhi::PointLight> lights,const LightClusterProjection& matrices,
    float nearPlane,float farPlane,LightClusterData& output,std::uint32_t capacity)
{
    if(!std::isfinite(nearPlane)||!std::isfinite(farPlane)||nearPlane<=0||farPlane<=nearPlane||!IsFinite(matrices.view)||!IsFinite(matrices.clipFromView))
        return false;
    const auto inverse=Inverse(matrices.clipFromView);if(!inverse)return false;
    if(capacity==0)capacity=LightClusterCapacity;
    for(const auto& light:lights)
        if(!Finite(light.position)||!std::isfinite(light.range)||light.range<0||!Finite(light.radiance))return false;
    std::vector<Vec3> viewCenters(lights.size());std::vector<float> radii(lights.size());
    for(std::size_t index=0;index<lights.size();++index)
    {
        const auto& light=lights[index];
        const auto center=matrices.view*Vec4{light.position.x,light.position.y,light.position.z,1};
        viewCenters[index]={center.x,center.y,center.z};radii[index]=light.range;
    }
    std::vector<std::vector<std::uint32_t>> assignments(LightClusterCount);
    const auto unproject=[](const Mat4& inverseMatrix,float x,float y,float z)
    {
        const auto clip=inverseMatrix*Vec4{x,y,z,1};
        if(!(std::abs(clip.w)>1e-8f))return Vec3{};
        return Vec3{clip.x/clip.w,clip.y/clip.w,clip.z/clip.w};
    };
    for(std::uint32_t z=0;z<LightClusterZ;++z)
    {
        const auto startDepth=nearPlane*std::pow(farPlane/nearPlane,float(z)/LightClusterZ);
        const auto endDepth=nearPlane*std::pow(farPlane/nearPlane,float(z+1)/LightClusterZ);
        const auto depthToNdc=[&](float depth)
        {
            return (farPlane*(depth-nearPlane)/(farPlane-nearPlane))/depth;
        };
        for(std::uint32_t y=0;y<LightClusterY;++y)
        {
            for(std::uint32_t x=0;x<LightClusterX;++x)
            {
                const auto ndcX0=-1.0f+2.0f*float(x)/LightClusterX,ndcX1=-1.0f+2.0f*float(x+1)/LightClusterX;
                const auto ndcY0=1.0f-2.0f*float(y+1)/LightClusterY,ndcY1=1.0f-2.0f*float(y)/LightClusterY;
                std::array<Vec3,8> corners{};
                std::size_t corner=0;
                for(const auto ndcZ:{depthToNdc(startDepth),depthToNdc(endDepth)})
                    for(const auto nx:{ndcX0,ndcX1})
                        for(const auto ny:{ndcY0,ndcY1})corners[corner++]=unproject(*inverse,nx,ny,ndcZ);
                Vec3 minimum{1e30f,1e30f,1e30f},maximum{-1e30f,-1e30f,-1e30f};
                bool valid=true;
                for(const auto& point:corners)
                {
                    if(!Finite(point)){valid=false;break;}
                    minimum={std::min(minimum.x,point.x),std::min(minimum.y,point.y),std::min(minimum.z,point.z)};
                    maximum={std::max(maximum.x,point.x),std::max(maximum.y,point.y),std::max(maximum.z,point.z)};
                }
                if(!valid)continue;
                auto& bucket=assignments[x+y*LightClusterX+z*LightClusterX*LightClusterY];
                for(std::size_t light=0;light<lights.size();++light)
                {
                    const auto& center=viewCenters[light];
                    const auto closest=Vec3{std::clamp(center.x,minimum.x,maximum.x),
                        std::clamp(center.y,minimum.y,maximum.y),std::clamp(center.z,minimum.z,maximum.z)};
                    if(Length(closest-center)<=radii[light])bucket.push_back(static_cast<std::uint32_t>(light));
                }
            }
        }
    }
    std::uint64_t total=0;std::uint32_t maxPerCluster=0;
    for(const auto& bucket:assignments){total+=bucket.size();maxPerCluster=std::max(maxPerCluster,static_cast<std::uint32_t>(bucket.size()));}
    LightClusterData candidate;candidate.clusters.assign(LightClusterCount,{});
    if(total>capacity)
    {
        candidate.overflow=true;candidate.maxPerCluster=maxPerCluster;
        output=std::move(candidate);return true;
    }
    candidate.indices.reserve(static_cast<std::size_t>(total));
    for(std::uint32_t index=0;index<LightClusterCount;++index)
    {
        candidate.clusters[index].offset=static_cast<std::uint32_t>(candidate.indices.size());
        candidate.clusters[index].count=static_cast<std::uint32_t>(assignments[index].size());
        for(const auto light:assignments[index])candidate.indices.push_back(light);
    }
    candidate.maxPerCluster=maxPerCluster;output=std::move(candidate);return true;
}
void FillLightClustersGlobal(std::uint32_t lightCount,LightClusterData& output)
{
    output.clusters.assign(LightClusterCount,LightClusterRange{0,lightCount});
    output.indices.clear();output.overflow=false;output.maxPerCluster=lightCount;
}
}
