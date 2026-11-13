#include "PostBindings.hlsli"
#include "Fullscreen.hlsli"
Texture2D<float4> inputScene:register(t0);
Texture2D<float4> bloomScene:register(t1);
// G15：自动曝光结果（1×1 R32_FLOAT）与 33³ display-linear LUT。
Texture2D<float> exposureTexture:register(t2);
Texture3D<float4> colorLut:register(t3);
SamplerState linearClamp:register(s0);
#ifdef SE_TONEMAP_ACES
// G15：ACES RRT+ODT 的 Hill 拟合；矩阵用逐行点积，避免 HLSL 矩阵布局歧义。
float3 AcesRrtAndOdt(float3 value)
{
    const float3 numerator=value*(value+0.0245786)-0.000090537;
    const float3 denominator=value*(0.983729*value+0.4329510)+0.238081;
    return numerator/max(denominator,1e-8);
}
float3 TonemapAces(float3 color)
{
    const float3 inputRed=float3(0.59719,0.35458,0.04823);
    const float3 inputGreen=float3(0.07600,0.90834,0.01566);
    const float3 inputBlue=float3(0.02840,0.13383,0.83777);
    const float3 fitted=float3(dot(color,inputRed),dot(color,inputGreen),dot(color,inputBlue));
    const float3 mapped=AcesRrtAndOdt(max(fitted,0));
    const float3 outputRed=float3(1.60475,-0.53108,-0.07367);
    const float3 outputGreen=float3(-0.10208,1.10813,-0.00605);
    const float3 outputBlue=float3(-0.00327,-0.07276,1.07602);
    return saturate(float3(dot(mapped,outputRed),dot(mapped,outputGreen),dot(mapped,outputBlue)));
}
#endif
// 33³ LUT 的晶格语义与 CPU ColorLut::Sample 一致：坐标 0..1 映射到 0..size-1 的
// 晶格点，纹理线性过滤完成三线性插值；只在 tonemap 之后、sRGB 编码之前采样。
float3 SampleColorLut(float3 displayLinear)
{
    const float size=33.0;
    const float3 coordinate=saturate(displayLinear)*(size-1.0)/size+0.5/size;
    return colorLut.SampleLevel(linearClamp,coordinate,0).rgb;
}
float4 PSMain(float4 position:SV_POSITION):SV_TARGET
{
    float3 radiance=max(inputScene.Load(int3(int2(position.xy),0)).rgb,0);
    if(postParameters.z>0)radiance+=bloomScene.SampleLevel(linearClamp,position.xy/postResolution.xy,0).rgb*postParameters.z;
    // postOptions.x 选择 GPU 自动曝光（1×1 纹理）或既有 CPU 曝光常量；默认走常量路径。
    radiance*=(postOptions.x>0.5?exposureTexture.Load(int3(0,0,0)):postParameters.x);
#ifdef SE_TONEMAP_ACES
    float3 mapped=TonemapAces(radiance);
#else
    float3 mapped=postParameters.w>0.5?radiance/(1+radiance):saturate(radiance);
#endif
    if(postOptions.y>0.5)mapped=SampleColorLut(mapped);
    return float4(EncodeSrgb(mapped),1);
}
