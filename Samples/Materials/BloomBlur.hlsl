#include "PostBindings.hlsli"
#include "Fullscreen.hlsli"
Texture2D<float4> inputScene:register(t0);
float4 PSMain(float4 position:SV_POSITION):SV_TARGET
{
    uint width,height;inputScene.GetDimensions(width,height);int2 limit=int2(width-1,height-1),pixel=int2(position.xy),direction=int2(postOptions.yz);
    float weights[5]={0.0625,0.25,0.375,0.25,0.0625};float3 color=0;
    [unroll]for(int index=0;index<5;++index)color+=inputScene.Load(int3(clamp(pixel+(index-2)*direction,int2(0,0),limit),0)).rgb*weights[index];
    return float4(color,1);
}
