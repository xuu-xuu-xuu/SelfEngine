// G15：从 GPU 直方图计算 60%~80% 分位目标并做指数适应。
// 单线程读取 256 桶；公式与 CPU AutoExposure::Update 一致：
// adapted += (target-adapted)*(1-exp(-dt*speed))，首帧直接取目标。
cbuffer ExposureParameters : register(b0)
{
    float4 exposurePercentiles; // x=minLog2, y=maxLog2, z=lowPercentile, w=highPercentile
    float4 exposureRange;       // x=key, y=minExposure, z=maxExposure, w=deltaSeconds
    float4 exposureAdaptation;  // x=brightSpeed, y=darkSpeed, z=initialize, w=unused
    float4 lutLift;             // rgb=lift, w=lutSize
    float4 lutGamma;            // rgb=gamma
    float4 lutGain;             // rgb=gain
};
Buffer<uint> histogramSource : register(t0);
Texture2D<float> exposureHistory : register(t1);
RWTexture2D<float> exposureOutput : register(u0);

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

float BucketLuminance(uint bucket)
{
    return exp2(HistogramMinimum() + ((float)bucket + 0.5f) / 256.0f * HistogramSpan());
}

float PercentileLuminance(uint pixels, float percentile)
{
    const uint target = max(1u, (uint)ceil(min(max(percentile, 0.0f), 1.0f) * (float)pixels));
    uint cumulative = 0;
    [loop] for (uint bucket = 0; bucket < 256; ++bucket)
    {
        cumulative += histogramSource[bucket];
        if (cumulative >= target) return BucketLuminance(bucket);
    }
    return BucketLuminance(255);
}

[numthreads(1,1,1)]
void AdaptExposure(uint3 thread : SV_DispatchThreadID)
{
    if (thread.x || thread.y) return;
    uint pixels = 0;
    [loop] for (uint index = 0; index < 256; ++index) pixels += histogramSource[index];
    float previous = exposureHistory.Load(int3(0, 0, 0));
    if (!Finite(previous) || previous <= 0.0f) previous = 1.0f;
    if (!pixels)
    {
        exposureOutput[int2(0,0)] = previous;
        return;
    }
    const float lowPercentile = min(exposurePercentiles.z, exposurePercentiles.w);
    const float highPercentile = max(exposurePercentiles.z, exposurePercentiles.w);
    float low = PercentileLuminance(pixels, lowPercentile);
    float high = PercentileLuminance(pixels, highPercentile);
    if (!(low > 0.0f)) low = high;
    if (!(high > 0.0f)) high = low;
    const float targetLuminance = sqrt(low * high);
    if (!(targetLuminance > 0.0f) || !Finite(targetLuminance))
    {
        exposureOutput[int2(0,0)] = previous;
        return;
    }
    float minimum = exposureRange.y, maximum = exposureRange.z;
    if (!Finite(minimum) || minimum <= 0.0f) minimum = 1.0f / 1024.0f;
    if (!Finite(maximum) || maximum <= 0.0f) maximum = 64.0f;
    if (minimum > maximum) { const float swap = minimum; minimum = maximum; maximum = swap; }
    float key = exposureRange.x;
    if (!Finite(key) || key <= 0.0f) key = 0.18f;
    const float target = min(max(key / targetLuminance, minimum), maximum);
    const bool initialize = exposureAdaptation.z > 0.5f;
    const float delta = Finite(exposureRange.w) ? min(max(exposureRange.w, 0.0f), 0.5f) : 0.0f;
    float adapted = target;
    if (!initialize)
    {
        const float speed = target > previous ? max(0.0f, exposureAdaptation.x) : max(0.0f, exposureAdaptation.y);
        adapted = previous + (target - previous) * (1.0f - exp(-delta * speed));
    }
    exposureOutput[int2(0,0)] = min(max(adapted, minimum), maximum);
}
