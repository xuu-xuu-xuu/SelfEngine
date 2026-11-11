cbuffer OutputConstants:register(b0){float4 exposureAndUnused;};
Texture2D<float4> hdrScene:register(t0);
#ifdef SE_SHADOW_PREVIEW
Texture2D<float> shadowPreview:register(t1);
#endif
// G15：输出可选的显示线性 33³ LUT strip。strip 为 1089×33：
// x = slice*33 + red，y = green；纹理可为 Rgba8（兼容）或 Rgba16Float（保留 >1 的显示线性值与精度）。
// 禁用时绑定 1×1 占位纹理并通过尺寸跳过采样。
#ifdef SE_OUTPUT_LUT
Texture2D<float4> outputLut:register(t2);
float3 LutTexel(float slice,float red,float green)
{
    return outputLut.Load(int3(int(slice*33.0+red),int(green),0)).rgb;
}
float3 LutSlice(float slice,float2 coordinate)
{
    // 与 CPU ColorLut::Sample 一致：坐标 0..1 映射到 size-1 等分，三角/双线性插值。
    const float scale=32.0;
    float2 position=coordinate*scale;
    float2 low=floor(position);
    float2 high=min(low+1.0,scale);
    float2 weight=position-low;
    const float3 c00=LutTexel(slice,low.x,low.y);
    const float3 c10=LutTexel(slice,high.x,low.y);
    const float3 c01=LutTexel(slice,low.x,high.y);
    const float3 c11=LutTexel(slice,high.x,high.y);
    return lerp(lerp(c00,c10,weight.x),lerp(c01,c11,weight.x),weight.y);
}
float3 SampleOutputLut(float3 value)
{
    const float3 color=saturate(value);
    const float blue=color.b*32.0;
    const float low=floor(blue);
    const float high=min(low+1.0,32.0);
    return lerp(LutSlice(low,color.rg),LutSlice(high,color.rg),blue-low);
}
#endif
#include "Fullscreen.hlsli"
float4 PSMain(float4 position:SV_POSITION):SV_TARGET
{
#ifdef SE_SHADOW_PREVIEW
    if(exposureAndUnused.y>0.5)
    {
        uint width,height;shadowPreview.GetDimensions(width,height);
        float2 uv=position.xy/exposureAndUnused.zw;
        float depth=shadowPreview.Load(int3(min(int2(uv*float2(width,height)),int2(width-1,height-1)),0));
        return float4(depth,depth,depth,1);
    }
#endif
    float3 radiance=max(hdrScene.Load(int3(int2(position.xy),0)).rgb,0)*exposureAndUnused.x;
    float3 mapped=saturate(radiance/(1+radiance));
#ifdef SE_OUTPUT_LUT
    uint lutWidth,lutHeight;outputLut.GetDimensions(lutWidth,lutHeight);
    // Rgba8 strip 烘焙时已饱和到 [0,1]；Rgba16F 保留 >1 值，交给下面的显示编码与目标格式钳制。
    // 仍需保证非负，否则 pow 在负值上是未定义的（Shader 警告按错误处理）。
    if(lutWidth>1u)mapped=max(SampleOutputLut(mapped),0.0);
#endif
    float3 encoded=lerp(mapped*12.92,1.055*pow(mapped,1.0/2.4)-0.055,step(0.0031308,mapped));
    return float4(encoded,1);
}
