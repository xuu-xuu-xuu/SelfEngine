#include "PostBindings.hlsli"
#include "Fullscreen.hlsli"
Texture2D<float4> inputScene:register(t0);
float4 PSMain(float4 position:SV_POSITION):SV_TARGET
{
    uint width,height;inputScene.GetDimensions(width,height);int2 limit=int2(width-1,height-1),start=int2(position.xy)*2;
    float3 color=0;
    [unroll]for(int y=0;y<2;++y)[unroll]for(int x=0;x<2;++x)
        color+=max(inputScene.Load(int3(min(start+int2(x,y),limit),0)).rgb,0)*0.25;
    float brightness=max(color.r,max(color.g,color.b));
    return float4(color*max(brightness-postParameters.y,0)/max(brightness,1e-6),1);
}
