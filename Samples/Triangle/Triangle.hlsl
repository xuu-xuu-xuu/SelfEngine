// CPU 直接复制列主序 Mat4.values；T05 参考图传单位矩阵，T11 传 P*V*W。
cbuffer CameraConstants : register(b0)
{
    column_major float4x4 clipFromWorld;
};

struct VertexInput
{
    float3 position : POSITION;
    float4 color : COLOR;
};

struct PixelInput
{
    float4 position : SV_POSITION;
    float4 color : COLOR;
};

PixelInput VSMain(VertexInput input)
{
    PixelInput output;
    output.position = mul(clipFromWorld, float4(input.position, 1.0));
    output.color = input.color;
    return output;
}

float4 PSMain(PixelInput input) : SV_TARGET
{
    return input.color;
}
