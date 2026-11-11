#include "AmbientOcclusion.h"
#include <algorithm>
#include <cmath>

namespace se
{
namespace
{
bool ValidFrame(const ScreenSpaceFrame& frame)
{
    return frame.width>0&&frame.height>0&&frame.width<=8192&&frame.height<=8192
        &&frame.viewDepth.size()==std::size_t{frame.width}*frame.height
        &&frame.viewNormal.size()==frame.viewDepth.size()&&std::isfinite(frame.tanHalfFovY)
        &&frame.tanHalfFovY>0.01f&&std::isfinite(frame.aspect)&&frame.aspect>0.01f;
}
float InterleavedGradientNoise(std::uint32_t x,std::uint32_t y)
{
    const float first=std::fmod(0.06711056f*float(x)+0.00583715f*float(y),1.0f);
    return std::fmod(52.9829189f*first,1.0f);
}
Vec3 NormalizeOr(Vec3 value,Vec3 fallback)
{
    const auto length=Length(value);
    return length>1e-6f?value/length:fallback;
}
bool Project(const ScreenSpaceFrame& frame,Vec3 viewPosition,float& pixelX,float& pixelY,float& depth)
{
    if(!(viewPosition.z<-1e-4f))return false;
    depth=-viewPosition.z;
    const auto ndcX=viewPosition.x/(depth*frame.tanHalfFovY*frame.aspect);
    const auto ndcY=viewPosition.y/(depth*frame.tanHalfFovY);
    if(!std::isfinite(ndcX)||!std::isfinite(ndcY))return false;
    pixelX=(ndcX*0.5f+0.5f)*frame.width;
    pixelY=(0.5f-ndcY*0.5f)*frame.height;
    return true;
}
bool SampleDepth(const ScreenSpaceFrame& frame,float pixelX,float pixelY,float& depth)
{
    const auto x=static_cast<std::uint32_t>(pixelX),y=static_cast<std::uint32_t>(pixelY);
    if(x>=frame.width||y>=frame.height)return false;
    depth=frame.viewDepth[std::size_t{y}*frame.width+x];
    return std::isfinite(depth)&&depth>0;
}
Vec3 ViewPosition(const ScreenSpaceFrame& frame,std::uint32_t x,std::uint32_t y)
{
    const auto depth=frame.viewDepth[std::size_t{y}*frame.width+x];
    const auto ndcX=(float(x)+0.5f)/frame.width*2.0f-1.0f;
    const auto ndcY=1.0f-(float(y)+0.5f)/frame.height*2.0f;
    return {ndcX*depth*frame.tanHalfFovY*frame.aspect,ndcY*depth*frame.tanHalfFovY,-depth};
}
bool ValidSettings(const GtaoSettings& settings)
{
    return std::isfinite(settings.radius)&&settings.radius>0&&settings.radius<100
        &&std::isfinite(settings.intensity)&&settings.intensity>=0&&settings.intensity<=4
        &&std::isfinite(settings.bias)&&settings.bias>=0&&settings.bias<1&&settings.slices>=1&&settings.slices<=32
        &&settings.steps>=1&&settings.steps<=64;
}
bool ValidContact(const ContactShadowSettings& settings,const Vec3& light)
{
    return std::isfinite(settings.length)&&settings.length>0&&settings.length<100
        &&std::isfinite(settings.bias)&&settings.bias>=0&&settings.bias<1&&settings.steps>=1&&settings.steps<=64
        &&IsFinite(light)&&Length(light)>1e-6f;
}
}
bool ComputeGtao(const ScreenSpaceFrame& frame,const GtaoSettings& settings,std::vector<float>& output)
{
    if(!ValidFrame(frame)||!ValidSettings(settings))return false;
    std::vector<float> ao(frame.viewDepth.size(),1.0f);
    const auto radius=settings.radius;
    for(std::uint32_t y=0;y<frame.height;++y)
    {
        for(std::uint32_t x=0;x<frame.width;++x)
        {
            const auto index=std::size_t{y}*frame.width+x;
            const auto depth=frame.viewDepth[index];
            if(!std::isfinite(depth)||depth<=0){continue;}
            const auto normal=NormalizeOr(frame.viewNormal[index],{0,0,1});
            const auto position=ViewPosition(frame,x,y);
            const auto up=std::abs(normal.z)>0.95f?Vec3{1,0,0}:Vec3{0,0,1};
            const auto tangent=NormalizeOr(Cross(up,normal),{1,0,0});
            const auto bitangent=Cross(normal,tangent);
            const auto rotation=InterleavedGradientNoise(x,y)*(2.0f*Pi);
            float occlusion=0;
            for(std::uint32_t slice=0;slice<settings.slices;++slice)
            {
                const auto angle=rotation+2.0f*Pi*float(slice)/float(settings.slices);
                const auto direction=tangent*std::cos(angle)+bitangent*std::sin(angle);
                for(std::uint32_t step=1;step<=settings.steps;++step)
                {
                    const auto fraction=float(step)/float(settings.steps);
                    const auto samplePosition=position+direction*(radius*fraction)+normal*(settings.bias*fraction);
                    float pixelX=0,pixelY=0,sampleRayDepth=0;
                    if(!Project(frame,samplePosition,pixelX,pixelY,sampleRayDepth))continue;
                    float sampledDepth=0;
                    if(!SampleDepth(frame,pixelX,pixelY,sampledDepth))continue;
                    // 采样表面比射线更近才遮挡；距离越远权重越低。
                    if(sampledDepth<sampleRayDepth-settings.bias)
                        occlusion+=(1.0f-fraction)*(1.0f-fraction);
                }
            }
            const auto samples=float(settings.slices*settings.steps);
            const auto normalized=std::clamp(occlusion/samples*settings.intensity*4.0f,0.0f,1.0f);
            ao[index]=std::clamp(1.0f-normalized,0.0f,1.0f);
        }
    }
    output=std::move(ao);return true;
}
bool ComputeContactShadows(const ScreenSpaceFrame& frame,const ContactShadowSettings& settings,
    const Vec3& lightDirectionView,std::vector<float>& output)
{
    if(!ValidFrame(frame)||!ValidContact(settings,lightDirectionView))return false;
    const auto light=Normalize(lightDirectionView);
    std::vector<float> visibility(frame.viewDepth.size(),1.0f);
    for(std::uint32_t y=0;y<frame.height;++y)
    {
        for(std::uint32_t x=0;x<frame.width;++x)
        {
            const auto index=std::size_t{y}*frame.width+x;
            const auto depth=frame.viewDepth[index];
            if(!std::isfinite(depth)||depth<=0)continue;
            const auto normal=NormalizeOr(frame.viewNormal[index],{0,0,1});
            const auto position=ViewPosition(frame,x,y)+normal*settings.bias;
            float shadow=0;
            for(std::uint32_t step=1;step<=settings.steps;++step)
            {
                const auto fraction=float(step)/float(settings.steps);
                const auto samplePosition=position+light*(settings.length*fraction);
                float pixelX=0,pixelY=0,rayDepth=0;
                if(!Project(frame,samplePosition,pixelX,pixelY,rayDepth))continue;
                float sampledDepth=0;
                if(!SampleDepth(frame,pixelX,pixelY,sampledDepth))continue;
                if(sampledDepth<rayDepth-settings.bias)
                {
                    const auto fade=settings.fade>0?std::clamp(1.0f-fraction/settings.fade,0.0f,1.0f):(1.0f-fraction);
                    shadow=std::max(shadow,fade);
                    break;
                }
            }
            visibility[index]=std::clamp(1.0f-shadow,0.0f,1.0f);
        }
    }
    output=std::move(visibility);return true;
}
bool UpsampleBilateral(const std::vector<float>& half,std::uint32_t halfWidth,std::uint32_t halfHeight,
    const ScreenSpaceFrame& full,std::vector<float>& output)
{
    if(!ValidFrame(full)||halfWidth==0||halfHeight==0||half.size()!=std::size_t{halfWidth}*halfHeight)return false;
    std::vector<float> upsampled(full.viewDepth.size(),1.0f);
    const auto scaleX=float(full.width)/float(halfWidth),scaleY=float(full.height)/float(halfHeight);
    for(std::uint32_t y=0;y<full.height;++y)
    {
        for(std::uint32_t x=0;x<full.width;++x)
        {
            const auto fullIndex=std::size_t{y}*full.width+x;
            const auto centerX=static_cast<std::int32_t>(float(x)/scaleX);
            const auto centerY=static_cast<std::int32_t>(float(y)/scaleY);
            const auto centerDepth=full.viewDepth[fullIndex];
            const auto centerNormal=NormalizeOr(full.viewNormal[fullIndex],{0,0,1});
            float weightSum=0,valueSum=0;
            for(std::int32_t dy=-2;dy<=2;++dy)
            {
                for(std::int32_t dx=-2;dx<=2;++dx)
                {
                    const auto sx=centerX+dx,sy=centerY+dy;
                    if(sx<0||sy<0||sx>=static_cast<std::int32_t>(halfWidth)||sy>=static_cast<std::int32_t>(halfHeight))continue;
                    const auto fx=std::clamp(static_cast<std::int32_t>(float(sx)*scaleX),0,static_cast<std::int32_t>(full.width)-1);
                    const auto fy=std::clamp(static_cast<std::int32_t>(float(sy)*scaleY),0,static_cast<std::int32_t>(full.height)-1);
                    const auto fullSampleIndex=static_cast<std::size_t>(fy)*full.width+static_cast<std::size_t>(fx);
                    const auto depthDifference=std::abs(full.viewDepth[fullSampleIndex]-centerDepth);
                    const auto normalDifference=Length(full.viewNormal[fullSampleIndex]-centerNormal);
                    if(depthDifference>0.2f*std::max(1.0f,centerDepth)&&normalDifference>0.5f)continue;
                    const auto spatialWeight=1.0f/(1.0f+float(dx*dx+dy*dy));
                    const auto depthWeight=1.0f/(1.0f+depthDifference*8.0f);
                    const auto normalWeight=1.0f/(1.0f+normalDifference*2.0f);
                    const auto weight=spatialWeight*depthWeight*normalWeight;
                    valueSum+=half[static_cast<std::size_t>(sy)*halfWidth+static_cast<std::size_t>(sx)]*weight;weightSum+=weight;
                }
            }
            upsampled[fullIndex]=weightSum>1e-6f?valueSum/weightSum:1.0f;
        }
    }
    output=std::move(upsampled);return true;
}
void ComposeScreenSpaceOcclusion(std::span<const float> ao,std::span<const float> contact,std::vector<float>& output)
{
    const auto count=std::min(ao.size(),contact.size());
    output.resize(count);
    for(std::size_t index=0;index<count;++index)output[index]=std::clamp(ao[index],0.0f,1.0f)*std::clamp(contact[index],0.0f,1.0f);
}
}
