// 布局 7；每条记录与 MaterialLayout.h 的 InstanceGpuData 对应，字节偏移固定。
ByteAddressBuffer instanceData : register(t7);
static const uint instanceStrideBytes=240;
float4x4 InstanceMatrix(uint offset)
{
    return transpose(float4x4(asfloat(instanceData.Load4(offset)),asfloat(instanceData.Load4(offset+16)),
        asfloat(instanceData.Load4(offset+32)),asfloat(instanceData.Load4(offset+48))));
}
