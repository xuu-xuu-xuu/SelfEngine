#include "MeshBindings.hlsli"
// G05.12：16 视角 Impostor 四边形。options.y/z 为相邻视角序号，options.w 为插值权重；
// 图集为 4×4 块，alpha 通道做 alpha test。
struct VertexInput { float3 position:POSITION; float3 normal:NORMAL; float2 uv:TEXCOORD0; };
struct PixelInput { float4 position:SV_POSITION; float2 uv:TEXCOORD0; };
PixelInput VSMain(VertexInput input)
{
    PixelInput output;
    output.position=mul(clipFromLocal,float4(input.position,1));
    output.uv=input.uv;
    return output;
}
float4 PSMain(PixelInput input):SV_TARGET
{
    const uint first=(uint)round(options.y);
    const uint second=(uint)round(options.z);
    const float blend=saturate(options.w);
    const float2 tileUv=input.uv*0.25;
    const float2 origin0=float2(first%4,first/4)*0.25;
    const float2 origin1=float2(second%4,second/4)*0.25;
    const float4 sampled=lerp(baseColor.Sample(textureSampler,origin0+tileUv),
        baseColor.Sample(textureSampler,origin1+tileUv),blend);
    clip(sampled.a-0.5);
    return float4(EncodeSrgb(sampled.rgb),1);
}
