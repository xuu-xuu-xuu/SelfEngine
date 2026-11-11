// G14：RG16F 屏幕空间速度。顶点着色器用当前/上一帧 clip-from-local 分别投影，
// 像素阶段输出 previousUV-currentUV（左上原点）；没有上一帧矩阵的绘制 previous=current，
// 速度为 0。只用于不透明网格；透明/UI 不写入速度缓冲。
cbuffer VelocityConstants : register(b0)
{
    float4x4 currentClipFromLocal;
    float4x4 previousClipFromLocal;
};
struct VelocityPixel
{
    float4 position : SV_POSITION;
    float2 currentUv : TEXCOORD0;
    float2 previousUv : TEXCOORD1;
};
float2 ProjectUv(float4 clip)
{
    // 不使用提前 return：D3DCompiler 在 /Od 流分析下会把提前返回误报为未初始化。
    const float w = clip.w > 1e-6f ? clip.w : 1e-6f;
    const float2 ndc = clip.xy / w;
    return float2(ndc.x * 0.5f + 0.5f, 0.5f - ndc.y * 0.5f);
}
VelocityPixel VSMain(float4 position : POSITION)
{
    const float4 local = float4(position.xyz, 1.0f);
    const float4 current = mul(currentClipFromLocal, local);
    VelocityPixel output;
    output.position = current;
    output.currentUv = ProjectUv(current);
    output.previousUv = ProjectUv(mul(previousClipFromLocal, local));
    return output;
}
float2 PSMain(VelocityPixel input) : SV_TARGET
{
    return input.previousUv - input.currentUv;
}
