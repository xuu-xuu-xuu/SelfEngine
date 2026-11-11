cbuffer VisibilityParameters : register(b0)
{
    float4 planes[6];
    uint inputCount;uint outputCapacity;uint2 reserved;
};
#include "VisibilityPredicate.hlsli"
StructuredBuffer<VisibilityItem> inputItems : register(t0);
RWStructuredBuffer<uint> visibleIds : register(u0);
RWByteAddressBuffer counters : register(u1);

[numthreads(1,1,1)]
void Reset(uint3 thread : SV_DispatchThreadID)
{
    counters.Store4(0,uint4(0,0,0,0));
    visibleIds[outputCapacity]=0x13579bdf;
    visibleIds[outputCapacity+1]=0x2468ace0;
}
[numthreads(64,1,1)]
void Cull(uint3 thread : SV_DispatchThreadID)
{
    if(thread.x>=inputCount)return;
    VisibilityItem item=inputItems[thread.x];if(!Visible(item))return;
    uint destination;counters.InterlockedAdd(0,1,destination);
    uint unused;
    if(destination<outputCapacity){visibleIds[destination]=item.id;counters.InterlockedAdd(4,1,unused);}
    else counters.InterlockedOr(8,1,unused);
}
