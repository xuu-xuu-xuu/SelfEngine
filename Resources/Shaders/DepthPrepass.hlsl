// G06.05：PBR 实例化路径的深度专用 prepass。只读实例数据中的 clipFromWorld*world，
// 与 PbrInstanced.hlsl 的 VSMain 使用同一 240 字节布局和矩阵乘法顺序；无像素着色器。
ByteAddressBuffer instanceData : register(t7);
static const uint instanceStrideBytes=240;
float4x4 InstanceMatrix(uint offset)
{
    return transpose(float4x4(asfloat(instanceData.Load4(offset)),asfloat(instanceData.Load4(offset+16)),
        asfloat(instanceData.Load4(offset+32)),asfloat(instanceData.Load4(offset+48))));
}
float4 VSMain(float3 position:POSITION,uint instanceIndex:SV_InstanceID):SV_POSITION
{
    return mul(InstanceMatrix(instanceIndex*instanceStrideBytes),float4(position,1));
}
void PSMain(){}
