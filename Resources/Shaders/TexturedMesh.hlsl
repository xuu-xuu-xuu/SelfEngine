#include "MeshBindings.hlsli"
struct VertexInput { float3 position:POSITION; float3 normal:NORMAL; float2 uv:TEXCOORD0; };
struct PixelInput { float4 position:SV_POSITION; float3 normal:NORMAL; float2 uv:TEXCOORD0; };
PixelInput VSMain(VertexInput input)
{
    PixelInput output;
    output.position=mul(clipFromLocal,float4(input.position,1));
    output.normal=mul(normalFromLocal,float4(input.normal,0)).xyz;
    output.uv=input.uv;
    return output;
}
float4 PSMain(PixelInput input):SV_TARGET
{
    float4 sampled=baseColor.Sample(textureSampler,input.uv)*tint;
    float lighting=options.x>0.5?1.0:0.25+0.75*saturate(dot(normalize(input.normal),normalize(float3(0.3,0.5,0.8))));
    return float4(EncodeSrgb(sampled.rgb*lighting),sampled.a);
}
