// G15：33³ 3D LUT 生成。坐标 0..1 按 size-1 等分，与 CPU ColorLut::Build 的
// lift/gamma/gain 同一公式；只写 display-linear 值，sRGB 编码仍由 ToneMap 唯一完成。
cbuffer ExposureParameters : register(b0)
{
    float4 exposurePercentiles;
    float4 exposureRange;
    float4 exposureAdaptation;
    float4 lutLift;             // rgb=lift, w=lutSize
    float4 lutGamma;            // rgb=gamma
    float4 lutGain;             // rgb=gain
};
RWTexture3D<float4> lutOutput : register(u0);

[numthreads(4,4,4)]
void BuildLut(uint3 thread : SV_DispatchThreadID)
{
    const uint size = (uint)max(floor(lutLift.w + 0.5f), 2.0f);
    if (thread.x >= size || thread.y >= size || thread.z >= size) return;
    const float divisor = (float)max(size - 1u, 1u);
    const float3 coordinate = float3(thread) / divisor;
    const float3 gamma = max(lutGamma.rgb, 1e-4f);
    const float3 graded = pow(max(coordinate * lutGain.rgb + lutLift.rgb, 0.0f), 1.0f / gamma);
    lutOutput[thread] = float4(graded, 1.0f);
}
