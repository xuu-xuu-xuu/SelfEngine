#define SE_SHADOWS 1
#include "PbrBindings.hlsli"
#include "ForwardBindings.hlsli"
#include "ShadowBindings.hlsli"

struct VertexInput {float3 position:POSITION;float3 normal:NORMAL;float2 uv:TEXCOORD0;float4 tangent:TANGENT;};
struct PixelInput
{
    float4 position:SV_POSITION;
    float3 worldPosition:TEXCOORD1;
    float3 normal:NORMAL;
    float2 uv:TEXCOORD0;
    float4 tangent:TANGENT;
};

PixelInput VSMain(VertexInput input)
{
    PixelInput output;
    output.position=mul(clipFromLocal,float4(input.position,1));
    output.worldPosition=mul(worldFromLocal,float4(input.position,1)).xyz;
    output.normal=mul(normalFromLocal,float4(input.normal,0)).xyz;
    output.uv=input.uv;
    output.tangent=input.tangent;
    return output;
}

float3 DirectLight(float3 albedo,float metal,float rough,float3 n,float3 v,float3 l)
{
    const float pi=3.14159265358979323846;
    float3 f0=lerp(float3(0.04,0.04,0.04),albedo,metal);
    float3 h=UnitVector(v+l,n);
    float nl=saturate(dot(n,l));
    float nh=saturate(dot(n,h));
    float vh=saturate(dot(v,h));
    float3 fresnel=f0+(1-f0)*pow(1-vh,5);
    float exponent=clamp(2/pow(max(rough,0.045),4)-2,2,2048);
    float distribution=(exponent+8)/(8*pi)*pow(nh,exponent);
    float3 diffuse=albedo*(1-metal)*(1-fresnel)/pi;
    float3 specular=fresnel*distribution;
    return (diffuse+specular)*nl;
}

float4 PSMain(PixelInput input,bool frontFace:SV_IsFrontFace):SV_TARGET
{
    // 保留切线输入以满足 4 输入反射；阶段 1 不重建 TBN，法线贴图不走此路径。
    if(!isfinite(input.tangent.w))discard;
    float3 n=UnitVector(input.normal,float3(0,0,1));
    if(!frontFace&&options.z<0.5)discard;
    if(!frontFace)n=-n;
    float3 albedo=baseColorMap.Sample(materialSampler,input.uv).rgb*baseColor.rgb;
    // 保留法线贴图 SRV 以复用 PBR 布局；阶段 1 不使用切线空间重建。
    const float4 unusedNormal=normalMap.Sample(materialSampler,input.uv);
    if(!isfinite(unusedNormal.x))discard;
    if(options.y>0.5)return float4(min(max(albedo,0),65504),1);
    float2 mr=metallicRoughnessMap.Sample(materialSampler,input.uv).gb;
    float metal=saturate(parameters.x*mr.y);
    float rough=max(0.045,saturate(parameters.y*mr.x));
    float3 v=UnitVector(cameraPosition.xyz-input.worldPosition,float3(0,0,1));
    float3 color=albedo*frameSkyExposure.rgb*0.35;
    if(frameDirection.w>0.5)
    {
        float3 l=UnitVector(frameDirection.xyz,float3(0,0,1));
        float visibility=options.w>0.5?ShadowVisibility(input.worldPosition,n,l):1;
        color+=DirectLight(albedo,metal,rough,n,v,l)*frameRadiance.rgb*visibility;
    }
    [loop] for(uint index=0;index<(uint)framePointCount.x;++index)
    {
        float3 delta=pointPositionRadius[index].xyz-input.worldPosition;
        float squared=dot(delta,delta);
        float radius=pointPositionRadius[index].w;
        float ratio=squared/(radius*radius),window=saturate(1-ratio*ratio);
        float attenuation=window*window/max(squared,0.0025);
        color+=DirectLight(albedo,metal,rough,n,v,UnitVector(delta,n))*pointRadiance[index].rgb*attenuation;
    }
    return float4(min(max(color,0),65504),1);
}
