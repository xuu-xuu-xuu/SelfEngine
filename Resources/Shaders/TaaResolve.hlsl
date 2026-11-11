// G14：TAA 历史解析。与 CPU ResolveTaa 的规则逐项一致：
// 历史重投影 = currentUV + velocity；速度超阈值或深度差超阈值时视为 disocclusion，
// 直接接受当前帧；否则在 YCoCg 3×3 邻域夹取历史，再按静止/动态新帧权重混合。
// 同时把当前深度写入历史深度，供下一帧判定。
cbuffer TaaParameters : register(b0)
{
    float width;
    float height;
    float staticBlend;
    float dynamicBlend;
    float depthThreshold;
    float velocityThreshold;
    float sharpen;
    float historyValid;
};
Texture2D<float4> currentColor : register(t0);
Texture2D<float2> velocityTexture : register(t1);
Texture2D<float> currentDepth : register(t2);
Texture2D<float4> historyColor : register(t3);
Texture2D<float> historyDepth : register(t4);
RWTexture2D<float4> outputColor : register(u0);
RWTexture2D<float> outputDepth : register(u1);
SamplerState linearClamp : register(s0);

float3 RgbToYCoCg(float3 color)
{
    return float3(0.25f * color.r + 0.5f * color.g + 0.25f * color.b,
        0.5f * color.r - 0.5f * color.b,
        -0.25f * color.r + 0.5f * color.g - 0.25f * color.b);
}
float3 YCoCgToRgb(float3 color)
{
    return float3(color.x + color.y - color.z, color.x + color.z, color.x - color.y - color.z);
}
float3 LoadNeighbor(int2 pixel, int2 size)
{
    const int2 clamped = clamp(pixel, int2(0, 0), size - 1);
    return currentColor.Load(int3(clamped, 0)).rgb;
}
float3 ClampHistory(float3 history, int2 pixel, int2 size)
{
    float3 minimum = RgbToYCoCg(currentColor.Load(int3(pixel, 0)).rgb);
    float3 maximum = minimum;
    [unroll] for (int y = -1; y <= 1; ++y)
    {
        [unroll] for (int x = -1; x <= 1; ++x)
        {
            const float3 value = RgbToYCoCg(LoadNeighbor(pixel + int2(x, y), size));
            minimum = min(minimum, value);
            maximum = max(maximum, value);
        }
    }
    float3 value = RgbToYCoCg(history);
    value = clamp(value, minimum, maximum);
    return YCoCgToRgb(value);
}
[numthreads(8, 8, 1)]
void Resolve(uint3 thread : SV_DispatchThreadID)
{
    const int2 size = int2((int)width, (int)height);
    if (thread.x >= (uint)width || thread.y >= (uint)height) return;
    const int2 pixel = int2(thread.xy);
    const float4 current = currentColor.Load(int3(pixel, 0));
    const float depth = currentDepth.Load(int3(pixel, 0));
    outputDepth[pixel] = depth;
    const float2 uv = (float2(pixel) + 0.5f) / float2(width, height);
    const float2 velocity = velocityTexture.Load(int3(pixel, 0));
    const float speed = length(velocity);
    bool valid = historyValid > 0.5f;
    float4 history = float4(0, 0, 0, 1);
    float previousDepth = 0;
    const float2 historyUv = uv + velocity;
    if (valid && (historyUv.x < 0.0f || historyUv.x > 1.0f || historyUv.y < 0.0f || historyUv.y > 1.0f)) valid = false;
    if (valid)
    {
        history = historyColor.SampleLevel(linearClamp, historyUv, 0);
        const int2 historyPixel = clamp(int2(historyUv * float2(size)), int2(0, 0), size - 1);
        previousDepth = historyDepth.Load(int3(historyPixel, 0));
    }
    const bool disoccluded = !valid || speed > velocityThreshold || abs(depth - previousDepth) > depthThreshold;
    float3 result = current.rgb;
    if (!disoccluded)
    {
        const float3 clamped = ClampHistory(history.rgb, pixel, size);
        const float alpha = clamp(speed > velocityThreshold * 0.25f ? dynamicBlend : staticBlend, 0.0f, 1.0f);
        result = lerp(clamped, current.rgb, alpha);
        if (sharpen > 0.0f)
        {
            float3 average = current.rgb;
            [unroll] for (int y = -1; y <= 1; ++y)
                [unroll] for (int x = -1; x <= 1; ++x)
                    if (x || y) average += LoadNeighbor(pixel + int2(x, y), size);
            average /= 9.0f;
            result += (current.rgb - average) * sharpen;
        }
    }
    outputColor[pixel] = float4(max(result, 0.0f), 1.0f);
}
