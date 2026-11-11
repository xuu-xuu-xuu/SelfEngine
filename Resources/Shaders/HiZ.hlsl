// G06.03：GPU Hi-Z 构建。mip0 为深度 1:1 复制，其后每级对上一级做 2×2 最大值归约，
// 与 CPU HiZBuffer::Build 的尺寸取整（(n+1)/2）和越界忽略规则一致；值只做 max/复制，
// 因此可以逐位对照。归约在 UAV 上链式进行，级间由记录方插入 UAV 屏障。
cbuffer HiZParameters : register(b0)
{
    uint sourceWidth;
    uint sourceHeight;
    uint destinationWidth;
    uint destinationHeight;
};
Texture2D<float> depthSource : register(t0);
RWTexture2D<float> sourceLevel : register(u0);
RWTexture2D<float> destinationLevel : register(u1);

[numthreads(8,8,1)]
void Copy(uint3 thread : SV_DispatchThreadID)
{
    if(thread.x>=destinationWidth||thread.y>=destinationHeight)return;
    destinationLevel[thread.xy]=depthSource.Load(int3(thread.xy,0));
}

[numthreads(8,8,1)]
void Reduce(uint3 thread : SV_DispatchThreadID)
{
    if(thread.x>=destinationWidth||thread.y>=destinationHeight)return;
    const uint2 origin=thread.xy*2;
    float maximum=0;
    [unroll] for(uint y=0;y<2;++y)
    {
        [unroll] for(uint x=0;x<2;++x)
        {
            const uint2 texel=origin+uint2(x,y);
            if(texel.x<sourceWidth&&texel.y<sourceHeight)maximum=max(maximum,sourceLevel[texel]);
        }
    }
    destinationLevel[thread.xy]=maximum;
}
