#ifndef SELF_ENGINE_PBR_BINDINGS
#define SELF_ENGINE_PBR_BINDINGS
// 布局 2：与 MaterialLayout.h 的 PbrDrawConstants 精确对应。
cbuffer PbrConstants : register(b0)
{
    column_major float4x4 clipFromLocal;
    column_major float4x4 worldFromLocal;
    column_major float4x4 normalFromLocal;
    float4 baseColor;
    float4 cameraPosition;
    float4 directionToLight;
    float4 radiance;
    float4 parameters; // metallic、roughness、normalScale、世界线性行列式符号。
    float4 options; // normalMapping、unlit、doubleSided；w 为 receivesShadow，G05.08 起非整数时表示 LOD dither。
};
Texture2D<float4> baseColorMap : register(t0);
Texture2D<float4> normalMap : register(t1);
Texture2D<float4> metallicRoughnessMap : register(t2);
SamplerState materialSampler : register(s0);
float3 UnitVector(float3 value,float3 fallback)
{
    float squared=dot(value,value);
    return squared>1e-12&&isfinite(squared)?value*rsqrt(squared):fallback;
}
// G05.08：与 CPU LodSelect::Bayer4x4Threshold 完全一致的 4×4 Bayer 阈值（(index+0.5)/16）。
float LodBayerThreshold(uint2 pixel)
{
    const float bayer[16]={0,8,2,10,12,4,14,6,3,11,1,9,15,7,13,5};
    return (bayer[(pixel.y&3)*4+(pixel.x&3)]+0.5)/16.0;
}
float3 OutputSrgb(float3 color)
{
    float3 value=saturate(color);
    return lerp(value*12.92,1.055*pow(value,1.0/2.4)-0.055,step(0.0031308,value));
}
float3 DirectBrdf(float3 color,float metal,float rough,float3 n,float3 v,float3 l)
{
    const float pi=3.14159265358979323846;
    float nl=saturate(dot(n,l));
    float nv=max(saturate(dot(n,v)),1e-4);
    float3 h=UnitVector(v+l,n);float nh=saturate(dot(n,h)),vh=saturate(dot(v,h));
    float alpha=rough*rough,a2=alpha*alpha;
    float divisor=nh*nh*(a2-1)+1;
    float distribution=a2/max(pi*divisor*divisor,1e-12);
    float visibility=0.5/max(nl*sqrt(nv*nv*(1-a2)+a2)+nv*sqrt(nl*nl*(1-a2)+a2),1e-8);
    float3 f0=lerp(float3(0.04,0.04,0.04),color,metal);
    float grazing=1-vh;float grazing5=grazing*grazing*grazing*grazing*grazing;
    float3 fresnel=f0+(1-f0)*grazing5;
    float3 specular=distribution*visibility*fresnel;
    float3 diffuse=(1-fresnel)*(1-metal)*color/pi;
    return (diffuse+specular)*nl;
}
#endif
