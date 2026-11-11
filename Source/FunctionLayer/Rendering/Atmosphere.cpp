#include "Atmosphere.h"
#include <algorithm>
#include <cmath>

namespace se
{
namespace
{
bool Finite(Vec3 value){return std::isfinite(value.x)&&std::isfinite(value.y)&&std::isfinite(value.z);}
bool Valid(const AtmosphereSettings& settings)
{
    return Finite(settings.sunDirection)&&Length(settings.sunDirection)>1e-6f
        &&std::isfinite(settings.sunIntensity)&&settings.sunIntensity>=0
        &&std::isfinite(settings.planetRadius)&&settings.planetRadius>1000
        &&std::isfinite(settings.atmosphereHeight)&&settings.atmosphereHeight>0&&settings.atmosphereHeight<settings.planetRadius
        &&std::isfinite(settings.rayleighScaleHeight)&&settings.rayleighScaleHeight>0
        &&std::isfinite(settings.mieScaleHeight)&&settings.mieScaleHeight>0
        &&std::isfinite(settings.rayleighScattering)&&settings.rayleighScattering>=0
        &&std::isfinite(settings.mieScattering)&&settings.mieScattering>=0
        &&std::isfinite(settings.mieExtinction)&&settings.mieExtinction>=0
        &&std::isfinite(settings.turbidity)&&settings.turbidity>=0&&settings.turbidity<=10;
}
float RayleighPhase(float cosine)
{
    constexpr float pi=3.14159265358979323846f;
    return 3.0f/(16.0f*pi)*(1.0f+cosine*cosine);
}
float MiePhase(float cosine)
{
    constexpr float pi=3.14159265358979323846f;
    constexpr float g=0.76f;
    const auto g2=g*g;
    return 3.0f/(8.0f*pi)*((1.0f-g2)*(1.0f+cosine*cosine))/((2.0f+g2)*std::pow(1.0f+g2-2.0f*g*cosine,1.5f));
}
}
bool Atmosphere::Same(const AtmosphereSettings& other) const
{
    return settings_.sunDirection.x==other.sunDirection.x&&settings_.sunDirection.y==other.sunDirection.y
        &&settings_.sunDirection.z==other.sunDirection.z&&settings_.sunIntensity==other.sunIntensity
        &&settings_.planetRadius==other.planetRadius&&settings_.atmosphereHeight==other.atmosphereHeight
        &&settings_.rayleighScaleHeight==other.rayleighScaleHeight&&settings_.mieScaleHeight==other.mieScaleHeight
        &&settings_.rayleighScattering==other.rayleighScattering&&settings_.mieScattering==other.mieScattering
        &&settings_.mieExtinction==other.mieExtinction&&settings_.turbidity==other.turbidity;
}
float Atmosphere::Density(float altitude,float rayleigh,float mie) const
{
    return rayleigh*std::exp(-altitude/settings_.rayleighScaleHeight)
        +mie*std::exp(-altitude/settings_.mieScaleHeight);
}
bool Atmosphere::Intersect(float altitude,Vec3 direction,float& nearDistance,float& farDistance) const
{
    const auto radius=settings_.planetRadius;
    const auto top=radius+settings_.atmosphereHeight;
    const auto origin=Vec3{0,radius+altitude,0};
    const auto b=Dot(origin,direction);
    const auto topDiscriminant=b*b-(Dot(origin,origin)-top*top);
    if(!(topDiscriminant>0))return false;
    farDistance=-b+std::sqrt(topDiscriminant);
    nearDistance=0;
    const auto planetDiscriminant=b*b-(Dot(origin,origin)-radius*radius);
    if(planetDiscriminant>0)
    {
        const auto planet=-b-std::sqrt(planetDiscriminant);
        if(planet>0){nearDistance=planet;return farDistance>nearDistance;}
    }
    return farDistance>0;
}
bool Atmosphere::Rebuild(const AtmosphereSettings& settings)
{
    if(!Valid(settings))return false;
    if(ready_&&Same(settings))return true;
    std::vector<float> lut(std::size_t{LutWidth}*LutHeight,0.0f);
    const auto sun=Normalize(settings.sunDirection);
    for(std::uint32_t y=0;y<LutHeight;++y)
    {
        const auto cosZenith=-1.0f+2.0f*float(y)/float(LutHeight-1);
        for(std::uint32_t x=0;x<LutWidth;++x)
        {
            const auto altitude=settings.atmosphereHeight*float(x)/float(LutWidth-1);
            const auto sinZenith=std::sqrt(std::max(0.0f,1.0f-cosZenith*cosZenith));
            const auto direction=Vec3{sinZenith,cosZenith,0};
            float nearDistance=0,farDistance=0;
            if(!Intersect(altitude,direction,nearDistance,farDistance)){lut[std::size_t{y}*LutWidth+x]=0;continue;}
            constexpr std::uint32_t samples=32;
            const auto step=(farDistance-nearDistance)/float(samples);
            auto opticalDepth=0.0f;
            for(std::uint32_t sample=0;sample<samples;++sample)
            {
                const auto distance=nearDistance+(float(sample)+0.5f)*step;
                const auto height=altitude+direction.y*distance;
                opticalDepth+=Density(height,settings.rayleighScattering,settings.mieExtinction*settings.turbidity)*step;
            }
            lut[std::size_t{y}*LutWidth+x]=std::exp(-opticalDepth);
        }
    }
    settings_=settings;settings_.sunDirection=sun;transmittance_=std::move(lut);ready_=true;++rebuilds_;
    return true;
}
float Atmosphere::TransmittanceToSun(float altitude,float cosZenith) const
{
    if(!ready_)return 0;
    const auto clampedAltitude=std::clamp(altitude,0.0f,settings_.atmosphereHeight);
    const auto x=clampedAltitude/settings_.atmosphereHeight*float(LutWidth-1);
    const auto y=(std::clamp(cosZenith,-1.0f,1.0f)+1.0f)*0.5f*float(LutHeight-1);
    const auto x0=static_cast<std::uint32_t>(std::min(x,float(LutWidth-1)));
    const auto y0=static_cast<std::uint32_t>(std::min(y,float(LutHeight-1)));
    const auto x1=std::min(x0+1,LutWidth-1),y1=std::min(y0+1,LutHeight-1);
    const auto fx=x-float(x0),fy=y-float(y0);
    const auto sample=[this](std::uint32_t sx,std::uint32_t sy){return transmittance_[std::size_t{sy}*LutWidth+sx];};
    const auto top=sample(x0,y0)*(1-fx)+sample(x1,y0)*fx;
    const auto bottom=sample(x0,y1)*(1-fx)+sample(x1,y1)*fx;
    return top*(1-fy)+bottom*fy;
}
Vec3 Atmosphere::SkyRadiance(Vec3 viewDirection) const
{
    if(!ready_)return {};
    const auto direction=Normalize(viewDirection);
    if(!Finite(direction)||Length(direction)<0.5f)return {};
    float nearDistance=0,farDistance=0;
    if(!Intersect(0.0f,direction,nearDistance,farDistance))return {};
    const auto sun=settings_.sunDirection;
    const auto cosine=std::clamp(Dot(direction,sun),-1.0f,1.0f);
    const auto phaseR=RayleighPhase(cosine),phaseM=MiePhase(cosine)*settings_.turbidity;
    constexpr std::uint32_t viewSamples=16,sunSamples=8;
    const auto step=(farDistance-nearDistance)/float(viewSamples);
    Vec3 radiance{0,0,0};
    auto viewOpticalDepth=0.0f;
    for(std::uint32_t sample=0;sample<viewSamples;++sample)
    {
        const auto distance=nearDistance+(float(sample)+0.5f)*step;
        const auto height=std::max(0.0f,direction.y*distance);
        const auto densityR=settings_.rayleighScattering*std::exp(-height/settings_.rayleighScaleHeight);
        const auto densityM=settings_.mieScattering*settings_.turbidity*std::exp(-height/settings_.mieScaleHeight);
        // 太阳方向光程：从采样点沿太阳方向再做 8 段积分（保守、无阴影）。
        auto sunOpticalDepth=0.0f;
        for(std::uint32_t sunSample=0;sunSample<sunSamples;++sunSample)
        {
            const auto sunDistance=settings_.atmosphereHeight*(float(sunSample)+0.5f)/float(sunSamples);
            const auto sunHeight=std::max(0.0f,height+sun.y*sunDistance);
            sunOpticalDepth+=(settings_.rayleighScattering*std::exp(-sunHeight/settings_.rayleighScaleHeight)
                +settings_.mieExtinction*settings_.turbidity*std::exp(-sunHeight/settings_.mieScaleHeight))
                *(settings_.atmosphereHeight/float(sunSamples));
        }
        const auto sunTransmittance=std::exp(-sunOpticalDepth);
        const auto viewTransmittance=std::exp(-viewOpticalDepth);
        radiance=radiance+(Vec3{densityR*phaseR,densityR*phaseR,densityR*phaseR}
            +Vec3{densityM*phaseM,densityM*phaseM,densityM*phaseM})*(sunTransmittance*viewTransmittance*settings_.sunIntensity*step);
        viewOpticalDepth+=(densityR+settings_.mieExtinction*settings_.turbidity*std::exp(-height/settings_.mieScaleHeight))*step;
    }
    return {std::max(0.0f,radiance.x),std::max(0.0f,radiance.y),std::max(0.0f,radiance.z)};
}
float FogTransmittance(const FogSettings& settings,float cameraHeight,float distance)
{
    if(!std::isfinite(settings.density)||settings.density<=0||!std::isfinite(distance)||distance<=0)return 1.0f;
    if(!(settings.heightFalloff>0)||!std::isfinite(settings.heightFalloff))
        return std::exp(-settings.density*distance);
    constexpr std::uint32_t steps=16;
    const auto step=distance/float(steps);
    auto opticalDepth=0.0f;
    for(std::uint32_t sample=0;sample<steps;++sample)
    {
        const auto height=cameraHeight+(float(sample)+0.5f)*step;
        opticalDepth+=settings.density*std::exp(-(height-settings.baseHeight)/settings.heightFalloff)*step;
    }
    return std::exp(-std::max(0.0f,opticalDepth));
}
Vec3 IntegrateFog(const FogSettings& settings,Vec3 viewDirection,float cameraHeight,float distance,float& transmittance)
{
    (void)viewDirection; // 高度雾只按相机高度与距离积分；方向斜率作为后续增强。
    transmittance=FogTransmittance(settings,cameraHeight,distance);
    const auto amount=std::clamp(1.0f-transmittance,0.0f,1.0f);
    return settings.color*amount;
}
}
