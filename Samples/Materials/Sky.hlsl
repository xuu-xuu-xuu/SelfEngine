cbuffer SkyConstants:register(b0)
{
    float4x4 skyFromClip;
    float4 zenithColor;
    float4 horizonColor;
    float4 groundColor;
    float4 sunDirectionIntensity; // xyz 指向太阳，w 强度
    float4 sunColor;
    float4 parameters;            // x=width, y=cosOuter, z=cosInner, w=height
};
#include "Fullscreen.hlsli"
float4 PSMain(float4 position:SV_POSITION):SV_TARGET
{
    const float2 ndc=float2(position.x/max(parameters.x,1.0)*2.0-1.0,
        1.0-position.y/max(parameters.w,1.0)*2.0);
    float3 direction=normalize(mul(skyFromClip,float4(ndc,1.0,1.0)).xyz);
    float3 sky=direction.y>=0.0
        ?lerp(horizonColor.rgb,zenithColor.rgb,pow(saturate(direction.y),0.55))
        :lerp(horizonColor.rgb,groundColor.rgb,pow(saturate(-direction.y),0.35));
    const float3 sunDirection=normalize(sunDirectionIntensity.xyz);
    const float cosine=saturate(dot(direction,sunDirection));
    const float disk=smoothstep(parameters.y,parameters.z,cosine);
    const float halo=pow(cosine,64.0)*0.15;
    const float intensity=sunDirectionIntensity.w;
    const float3 sun=sunColor.rgb*(intensity*disk*disk+halo*intensity);
    return float4(min(sky+sun,65504.0),1.0);
}
