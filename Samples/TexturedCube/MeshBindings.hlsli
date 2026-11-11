#ifndef SELF_ENGINE_MESH_BINDINGS
#define SELF_ENGINE_MESH_BINDINGS
// 与 PlatformLayer/Graphics/MaterialLayout.h 对应，由构建时反射校验。
cbuffer DrawConstants : register(b0)
{
    column_major float4x4 clipFromLocal;
    column_major float4x4 normalFromLocal;
    float4 tint;
    float4 options;
};
Texture2D<float4> baseColor : register(t0);
SamplerState textureSampler : register(s0);
float3 EncodeSrgb(float3 linearColor)
{
    float3 value=saturate(linearColor);
    return lerp(value*12.92,1.055*pow(value,1.0/2.4)-0.055,step(0.0031308,value));
}
#endif
