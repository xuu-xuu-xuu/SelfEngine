#include "Reflection.h"
#include <algorithm>
#include <cmath>

namespace se
{
namespace
{
bool Finite(Vec3 value){return std::isfinite(value.x)&&std::isfinite(value.y)&&std::isfinite(value.z);}
Vec3 NormalizeOr(Vec3 value,Vec3 fallback)
{
    const auto length=Length(value);
    return length>1e-6f?value/length:fallback;
}
Vec3 SampleEquirect(std::span<const Vec3> equirect,std::uint32_t width,std::uint32_t height,Vec3 direction)
{
    const auto length=Length(direction);
    if(!(length>1e-6f))return {};
    direction=direction/length;
    const auto u=(std::atan2(direction.z,direction.x)/(2.0f*Pi))+0.5f;
    const auto v=std::acos(std::clamp(direction.y,-1.0f,1.0f))/Pi;
    const auto x=std::min(width-1,static_cast<std::uint32_t>(std::clamp(u,0.0f,0.999999f)*width));
    const auto y=std::min(height-1,static_cast<std::uint32_t>(std::clamp(v,0.0f,0.999999f)*height));
    return equirect[std::size_t{y}*width+x];
}
Vec3 TangentSample(Vec3 normal,float x,float y,float z)
{
    const auto up=std::abs(normal.y)>0.999f?Vec3{0,0,1}:Vec3{0,1,0};
    const auto tangent=Normalize(Cross(up,normal));
    const auto bitangent=Cross(normal,tangent);
    return Normalize(tangent*x+bitangent*y+normal*z);
}
float RadicalInverse(std::uint32_t bits)
{
    bits=(bits<<16u)|(bits>>16u);bits=((bits&0x55555555u)<<1u)|((bits&0xAAAAAAAAu)>>1u);
    bits=((bits&0x33333333u)<<2u)|((bits&0xCCCCCCCCu)>>2u);bits=((bits&0x0F0F0F0Fu)<<4u)|((bits&0xF0F0F0F0u)>>4u);
    bits=((bits&0x00FF00FFu)<<8u)|((bits&0xFF00FF00u)>>8u);
    return float(bits)*2.3283064365386963e-10f;
}
Vec3 ImportanceSampleGgx(float x,float y,float roughness,Vec3 normal)
{
    const auto a=roughness*roughness;
    const auto phi=2.0f*Pi*x;
    const auto cosTheta=std::sqrt((1.0f-y)/(1.0f+(a*a-1.0f)*y));
    const auto sinTheta=std::sqrt(std::max(0.0f,1.0f-cosTheta*cosTheta));
    return TangentSample(normal,std::cos(phi)*sinTheta,std::sin(phi)*sinTheta,cosTheta);
}
Vec3 ViewPosition(const ScreenSpaceFrame& frame,std::uint32_t x,std::uint32_t y)
{
    const auto depth=frame.viewDepth[std::size_t{y}*frame.width+x];
    const auto ndcX=(float(x)+0.5f)/frame.width*2.0f-1.0f;
    const auto ndcY=1.0f-(float(y)+0.5f)/frame.height*2.0f;
    return {ndcX*depth*frame.tanHalfFovY*frame.aspect,ndcY*depth*frame.tanHalfFovY,-depth};
}
bool Project(const ScreenSpaceFrame& frame,Vec3 viewPosition,float& pixelX,float& pixelY,float& depth)
{
    if(!(viewPosition.z<-1e-4f))return false;
    depth=-viewPosition.z;
    const auto ndcX=viewPosition.x/(depth*frame.tanHalfFovY*frame.aspect);
    const auto ndcY=viewPosition.y/(depth*frame.tanHalfFovY);
    if(!std::isfinite(ndcX)||!std::isfinite(ndcY))return false;
    pixelX=(ndcX*0.5f+0.5f)*frame.width;pixelY=(0.5f-ndcY*0.5f)*frame.height;
    return true;
}
}
bool BoxProjectReflection(Vec3 center,Vec3 extents,Vec3 position,Vec3 reflectDirection,Vec3& output)
{
    if(!Finite(center)||!Finite(extents)||!Finite(position)||!Finite(reflectDirection)
        ||extents.x<=0||extents.y<=0||extents.z<=0)return false;
    const auto length=Length(reflectDirection);
    if(!(length>1e-6f))return false;
    const auto direction=reflectDirection/length;
    const auto local=position-center;
    // 从位置沿方向与盒的出口面求交；盒内取最近的出口，盒外取最近的正向入射。
    float best=1e30f;
    bool found=false;
    for(int axis=0;axis<3;++axis)
    {
        const auto p=axis==0?local.x:axis==1?local.y:local.z;
        const auto d=axis==0?direction.x:axis==1?direction.y:direction.z;
        const auto extent=axis==0?extents.x:axis==1?extents.y:extents.z;
        if(std::abs(d)<1e-8f)continue;
        const auto t=(d>0?extent:-extent-p)/d;
        const auto sign=d>0?1.0f:-1.0f;
        const auto hit=(p+d*t)*sign;
        if(t>1e-6f&&hit<=extent+1e-4f&&t<best){best=t;found=true;}
    }
    if(!found)return false;
    auto hit=local+direction*best;
    hit={std::clamp(hit.x,-extents.x,extents.x),std::clamp(hit.y,-extents.y,extents.y),std::clamp(hit.z,-extents.z,extents.z)};
    const auto projected=hit-local;
    if(!(Length(projected)>1e-6f))return false;
    output=Normalize(projected);return true;
}
bool BlendReflectionProbes(std::span<const ReflectionProbe> probes,Vec3 position,Vec3 reflectDirection,
    Vec3& color,std::uint32_t& first,std::uint32_t& second,float& blend)
{
    if(!Finite(position)||!Finite(reflectDirection)||probes.empty())return false;
    struct Candidate{std::uint32_t index;float weight;};
    std::vector<Candidate> candidates;
    for(std::uint32_t index=0;index<probes.size();++index)
    {
        const auto& probe=probes[index];
        if(!Finite(probe.center)||!std::isfinite(probe.radius)||probe.radius<=0)continue;
        const auto distance=Length(position-probe.center);
        const auto outer=probe.radius+std::max(0.0f,probe.blendDistance);
        if(distance>outer)continue;
        const auto weight=distance<=probe.radius?1.0f:std::clamp(1.0f-(distance-probe.radius)/std::max(1e-6f,probe.blendDistance),0.0f,1.0f);
        if(weight>0)candidates.push_back({index,weight});
    }
    if(candidates.empty())return false;
    std::sort(candidates.begin(),candidates.end(),[](const auto& a,const auto& b){return a.weight>b.weight;});
    candidates.resize(std::min<std::size_t>(2,candidates.size()));
    first=candidates[0].index;second=candidates[0].index;blend=0;
    Vec3 accumulated{0,0,0};auto total=0.0f;
    for(const auto& candidate:candidates)
    {
        const auto& probe=probes[candidate.index];
        if(probe.environment.empty()||probe.width==0||probe.height==0
            ||probe.environment.size()!=std::size_t{probe.width}*probe.height)continue;
        Vec3 direction=reflectDirection;
        if(!BoxProjectReflection(probe.center,probe.extents,position,reflectDirection,direction))direction=reflectDirection;
        accumulated=accumulated+SampleEquirect(probe.environment,probe.width,probe.height,direction)*candidate.weight;
        total+=candidate.weight;
    }
    if(!(total>1e-6f))return false;
    color=accumulated/total;
    if(candidates.size()>1){second=candidates[1].index;blend=candidates[1].weight/(candidates[0].weight+candidates[1].weight);}
    return true;
}
bool PrefilterGgx(std::span<const Vec3> equirect,std::uint32_t width,std::uint32_t height,float roughness,
    std::uint32_t samples,std::vector<Vec3>& output)
{
    if(width==0||height==0||equirect.size()!=std::size_t{width}*height||!std::isfinite(roughness)
        ||roughness<0||roughness>1||samples==0||samples>1024)return false;
    std::vector<Vec3> filtered(equirect.size(),Vec3{0,0,0});
    for(std::uint32_t y=0;y<height;++y)
    {
        for(std::uint32_t x=0;x<width;++x)
        {
            const auto u=(float(x)+0.5f)/width,v=(float(y)+0.5f)/height;
            const auto phi=2.0f*Pi*u;
            const auto normal=Vec3{std::sin(Pi*v)*std::cos(phi),std::cos(Pi*v),std::sin(Pi*v)*std::sin(phi)};
            Vec3 sum{0,0,0};
            for(std::uint32_t sample=0;sample<samples;++sample)
            {
                const auto xiX=(float(sample)+0.5f)/float(samples),xiY=RadicalInverse(sample);
                const auto halfway=ImportanceSampleGgx(xiX,xiY,roughness,normal);
                const auto light=halfway*(2.0f*Dot(normal,halfway))-normal;
                sum=sum+SampleEquirect(equirect,width,height,light);
            }
            filtered[std::size_t{y}*width+x]=sum/float(samples);
        }
    }
    output=std::move(filtered);return true;
}
bool TraceScreenSpaceReflection(const ScreenSpaceFrame& frame,const SsrSettings& settings,std::uint32_t pixelX,
    std::uint32_t pixelY,Vec3 reflectDirectionView,SsrHit& hit)
{
    hit={};
    if(frame.width==0||frame.height==0||pixelX>=frame.width||pixelY>=frame.height
        ||frame.viewDepth.size()!=std::size_t{frame.width}*frame.height||settings.steps==0||settings.steps>256
        ||!std::isfinite(settings.thickness)||settings.thickness<=0||!std::isfinite(settings.bias)||settings.bias<0
        ||!std::isfinite(settings.maxDistance)||settings.maxDistance<=0||!Finite(reflectDirectionView))return false;
    const auto depth=frame.viewDepth[std::size_t{pixelY}*frame.width+pixelX];
    if(!(depth>0))return false;
    const auto normal=NormalizeOr(frame.viewNormal[std::size_t{pixelY}*frame.width+pixelX],{0,0,1});
    const auto direction=Normalize(reflectDirectionView);
    if(!(Length(direction)>0.5f))return false;
    const auto origin=ViewPosition(frame,pixelX,pixelY)+normal*settings.bias;
    for(std::uint32_t step=1;step<=settings.steps;++step)
    {
        const auto distance=settings.maxDistance*float(step)/float(settings.steps);
        const auto samplePosition=origin+direction*distance;
        if(!(samplePosition.z<-1e-4f))break;
        float sampleX=0,sampleY=0,rayDepth=0;
        if(!Project(frame,samplePosition,sampleX,sampleY,rayDepth))break;
        if(sampleX<0||sampleY<0||sampleX>=frame.width||sampleY>=frame.height)break;
        const auto xi=static_cast<std::uint32_t>(sampleX),yi=static_cast<std::uint32_t>(sampleY);
        const auto sceneDepth=frame.viewDepth[std::size_t{yi}*frame.width+xi];
        if(!std::isfinite(sceneDepth)||sceneDepth<=0)continue;
        if(sceneDepth<rayDepth-settings.thickness)
        {
            const auto edgeDistance=std::min({sampleX,float(frame.width)-sampleX,sampleY,float(frame.height)-sampleY});
            hit.hit=true;hit.u=(sampleX+0.5f)/frame.width;hit.v=(sampleY+0.5f)/frame.height;hit.depth=sceneDepth;
            hit.confidence=std::clamp(1.0f-float(step)/float(settings.steps),0.0f,1.0f)*std::clamp(edgeDistance/8.0f,0.0f,1.0f);
            return true;
        }
    }
    return true;
}
}
