cbuffer IndirectParameters : register(b0)
{
    float4 planes[6];
    uint inputCount;uint outputCapacity;uint indexCount;uint reserved;
    // G06.03：可选 Hi-Z 遮挡。screen = (宽, 高, 深度偏置, mip 级数)。
    column_major float4x4 clipFromWorld;
    float4 screen;
    uint occlusionEnabled;uint3 occlusionReserved;
};
#include "VisibilityPredicate.hlsli"
StructuredBuffer<VisibilityItem> inputItems : register(t0);
ByteAddressBuffer inputInstances : register(t1);
Texture2D<float> hzbTexture : register(t3);
RWStructuredBuffer<uint> localOffsets : register(u0);
RWStructuredBuffer<uint> blockOffsets : register(u1);
RWByteAddressBuffer outputInstances : register(u2);
RWByteAddressBuffer arguments : register(u3);

// 与 CPU TestOcclusion 同一规则：屏幕 AABB、≤2 texel 的 mip、覆盖 texel 的最大深度，
// 只有物体最近深度严格大于最大深度 + 偏置才判遮挡；跨近平面/深度越界/屏幕外/触及
// 向下取整 mip 未覆盖的右/下边缘时一律保留可见。
bool Occluded(VisibilityItem item)
{
    bool occluded=false;
    if(occlusionEnabled!=0)
    {
        const float huge=1e30f;
        float minimumX=huge,maximumX=-huge,minimumY=huge,maximumY=-huge,minimumDepth=1;
        bool valid=true;
        [unroll] for(uint corner=0;corner<8;++corner)
        {
            if(!valid)continue;
            float3 cornerPosition;
            cornerPosition.x=(corner&1)?item.maximum.x:item.minimum.x;
            cornerPosition.y=(corner&2)?item.maximum.y:item.minimum.y;
            cornerPosition.z=(corner&4)?item.maximum.z:item.minimum.z;
            float4 clipPosition=mul(clipFromWorld,float4(cornerPosition,1));
            float x=0,y=0,z=0;
            if(clipPosition.w<=1e-6f)valid=false;
            else
            {
                x=clipPosition.x/clipPosition.w;y=clipPosition.y/clipPosition.w;z=clipPosition.z/clipPosition.w;
                if(!(isfinite(x)&&isfinite(y)&&isfinite(z))||z<-1e-5f||z>1.00001f)valid=false;
            }
            if(valid)
            {
                minimumX=min(minimumX,x);maximumX=max(maximumX,x);
                minimumY=min(minimumY,y);maximumY=max(maximumY,y);
                minimumDepth=min(minimumDepth,z);
            }
        }
        if(valid)
        {
            float left=(minimumX*0.5f+0.5f)*screen.x;
            float right=(maximumX*0.5f+0.5f)*screen.x;
            float top=(0.5f-maximumY*0.5f)*screen.y;
            float bottom=(0.5f-minimumY*0.5f)*screen.y;
            left=clamp(left,0.0f,screen.x);right=clamp(right,0.0f,screen.x);
            top=clamp(top,0.0f,screen.y);bottom=clamp(bottom,0.0f,screen.y);
            // 保守膨胀：吸收 CPU/GPU 浮点求值顺序差异，避免在 texel/mip 边界上比 CPU 少覆盖。
            left=max(0.0f,left-0.01f);right=min(screen.x,right+0.01f);
            top=max(0.0f,top-0.01f);bottom=min(screen.y,bottom+0.01f);
            if(right>left&&bottom>top)
            {
                uint level=0;float scale=1;
                uint levelCount=(uint)screen.w;
                while(level+1<levelCount&&((right-left)/scale>1.99f||(bottom-top)/scale>1.99f)){++level;scale*=2.0f;}
                uint levelWidth=max(1u,(uint)screen.x>>level);
                uint levelHeight=max(1u,(uint)screen.y>>level);
                if(!(right>float(levelWidth)*scale+1e-4f||bottom>float(levelHeight)*scale+1e-4f))
                {
                    uint x0=min((uint)max(0.0f,floor(left/scale)),levelWidth-1);
                    uint y0=min((uint)max(0.0f,floor(top/scale)),levelHeight-1);
                    uint x1=min((uint)max(0.0f,ceil(right/scale)-1.0f),levelWidth-1);
                    uint y1=min((uint)max(0.0f,ceil(bottom/scale)-1.0f),levelHeight-1);
                    float maximum=0;
                    [unroll] for(uint dy=0;dy<4;++dy)
                    {
                        [unroll] for(uint dx=0;dx<4;++dx)
                        {
                            const uint2 texel=uint2(x0+dx,y0+dy);
                            if(texel.x<=x1&&texel.y<=y1)maximum=max(maximum,hzbTexture.Load(int3(texel,level)));
                        }
                    }
                    occluded=minimumDepth>maximum+screen.z;
                }
            }
        }
    }
    return occluded;
}
bool VisibleAndUnoccluded(VisibilityItem item)
{
    return Visible(item)&&!Occluded(item);
}

// 只按现有InstanceGpuData的240字节步长复制，不定义第二套材质/矩阵布局。
static const uint InstanceStride=240;
groupshared uint localScan[64];
groupshared uint blockScan[1024];

[numthreads(64,1,1)]
void LocalScan(uint3 thread : SV_DispatchThreadID,uint3 group : SV_GroupID,uint lane : SV_GroupIndex)
{
    uint visible=0;
    if(thread.x<inputCount)visible=VisibleAndUnoccluded(inputItems[thread.x])?1:0;
    localScan[lane]=visible;
    GroupMemoryBarrierWithGroupSync();
    [unroll] for(uint offset=1;offset<64;offset*=2)
    {
        uint previous=0;if(lane>=offset)previous=localScan[lane-offset];
        GroupMemoryBarrierWithGroupSync();
        localScan[lane]+=previous;
        GroupMemoryBarrierWithGroupSync();
    }
    if(thread.x<inputCount)localOffsets[thread.x]=visible!=0?0x80000000u|(localScan[lane]-1):0;
    if(lane==63)blockOffsets[group.x]=localScan[63];
}

[numthreads(1024,1,1)]
void GroupScan(uint lane : SV_GroupIndex)
{
    uint blocks=max(1u,(inputCount+63)/64);
    uint total=0;if(lane<blocks)total=blockOffsets[lane];
    blockScan[lane]=total;
    GroupMemoryBarrierWithGroupSync();
    [unroll] for(uint offset=1;offset<1024;offset*=2)
    {
        uint previous=0;if(lane>=offset)previous=blockScan[lane-offset];
        GroupMemoryBarrierWithGroupSync();
        blockScan[lane]+=previous;
        GroupMemoryBarrierWithGroupSync();
    }
    if(lane<blocks)blockOffsets[lane]=blockScan[lane]-total;
    if(lane==0)
    {
        uint count=blockScan[1023],written=min(count,outputCapacity);
        arguments.Store4(0,uint4(indexCount,written,0,0));
        arguments.Store4(16,uint4(0,written!=0?1:0,count,written));
        arguments.Store4(32,uint4(count>outputCapacity?1:0,0,0x13579bdf,0x2468ace0));
        outputInstances.Store2(outputCapacity*InstanceStride,uint2(0x13579bdf,0x2468ace0));
    }
}

[numthreads(64,1,1)]
void Gather(uint3 thread : SV_DispatchThreadID,uint3 group : SV_GroupID)
{
    if(thread.x>=inputCount)return;
    uint packed=localOffsets[thread.x];if((packed&0x80000000u)==0)return;
    uint destination=blockOffsets[group.x]+(packed&0x7fffffffu);
    if(destination>=outputCapacity)return;
    [unroll] for(uint offset=0;offset<InstanceStride;offset+=16)
        outputInstances.Store4(destination*InstanceStride+offset,inputInstances.Load4(thread.x*InstanceStride+offset));
}
