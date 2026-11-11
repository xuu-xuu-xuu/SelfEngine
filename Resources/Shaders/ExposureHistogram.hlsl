// G15：GPU 自动曝光的 256 桶 log2 亮度直方图。
// 与 CPU AutoExposure 使用同一公式；每线程处理 4×4 像素，组内共享原子累加后
// 每线程合并一个桶。亮度为 Rec.709 线性亮度，非有限像素直接跳过。
cbuffer ExposureParameters : register(b0)
{
    float4 exposurePercentiles; // x=minLog2, y=maxLog2, z=lowPercentile, w=highPercentile
    float4 exposureRange;       // x=key, y=minExposure, z=maxExposure, w=deltaSeconds
    float4 exposureAdaptation;  // x=brightSpeed, y=darkSpeed, z=initialize, w=unused
    float4 lutLift;             // rgb=lift, w=lutSize
    float4 lutGamma;            // rgb=gamma
    float4 lutGain;             // rgb=gain
};
Texture2D<float4> exposureSource : register(t0);
RWBuffer<uint> histogramTarget : register(u0);
groupshared uint sharedHistogram[256];

bool Finite(float value)
{
    // Release 优化会假定浮点有限；用指数位判断 NaN/Inf，避免 isfinite/自比较被折叠。
    return (asuint(value) & 0x7f800000u) != 0x7f800000u;
}

float HistogramMinimum()
{
    return min(exposurePercentiles.x, exposurePercentiles.y);
}

float HistogramMaximum()
{
    return max(exposurePercentiles.x, exposurePercentiles.y);
}

float HistogramSpan()
{
    return max(1e-6f, HistogramMaximum() - HistogramMinimum());
}

// 与 AutoExposure.cpp 的 BucketFor 逐项一致：非有限按最小亮度、<=0 进入最低桶。
uint BucketFor(float luminance)
{
    const float minimum = HistogramMinimum();
    const float safe = Finite(luminance) ? max(luminance, 1e-9f) : 1e-9f;
    const float logValue = min(max(log2(safe), minimum), HistogramMaximum());
    const uint index = (uint)((logValue - minimum) / HistogramSpan() * 256.0f);
    return min(255u, index);
}

// 256 个桶用 64 个线程各清 4 个；单独一次 dispatch，避免组间缺少全局屏障。
[numthreads(64,1,1)]
void ClearHistogram(uint3 thread : SV_DispatchThreadID)
{
    if (thread.x >= 64) return;
    const uint base = thread.x * 4;
    histogramTarget[base + 0] = 0;
    histogramTarget[base + 1] = 0;
    histogramTarget[base + 2] = 0;
    histogramTarget[base + 3] = 0;
}

// 每个线程覆盖 4×4 像素；组 16×16 = 256 线程覆盖 64×64 像素。
[numthreads(16,16,1)]
void BuildHistogram(uint3 thread : SV_GroupThreadID, uint3 dispatch : SV_DispatchThreadID)
{
    const uint bin = thread.y * 16 + thread.x;
    sharedHistogram[bin] = 0;
    GroupMemoryBarrierWithGroupSync();
    uint width = 0, height = 0;
    exposureSource.GetDimensions(width, height);
    const uint2 origin = dispatch.xy * 4;
    [unroll] for (uint y = 0; y < 4; ++y)
    {
        [unroll] for (uint x = 0; x < 4; ++x)
        {
            const uint2 pixel = origin + uint2(x, y);
            if (pixel.x >= width || pixel.y >= height) continue;
            const float4 value = exposureSource.Load(int3(int2(pixel), 0));
            if (!Finite(value.x) || !Finite(value.y) || !Finite(value.z)) continue;
            const float luminance = dot(max(value.rgb, 0.0f), float3(0.2126f, 0.7152f, 0.0722f));
            if (!Finite(luminance)) continue;
            InterlockedAdd(sharedHistogram[BucketFor(luminance)], 1u);
        }
    }
    GroupMemoryBarrierWithGroupSync();
    const uint count = sharedHistogram[bin];
    if (count) InterlockedAdd(histogramTarget[bin], count);
}
