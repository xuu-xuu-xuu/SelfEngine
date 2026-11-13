#include "PostBindings.hlsli"
#include "Fullscreen.hlsli"
Texture2D<float4> inputScene:register(t0);
SamplerState linearClamp:register(s0);
float Luma(float3 color){return dot(color,float3(0.299,0.587,0.114));}
float3 Fetch(float2 uv){return inputScene.SampleLevel(linearClamp,uv,0).rgb;}
float4 PSMain(float4 position:SV_POSITION):SV_TARGET
{
    float2 reciprocal=postResolution.zw,uv=position.xy*reciprocal;float3 center=Fetch(uv),filtered=center;
    if(postOptions.x>0.5)
    {
        float nw=Luma(Fetch(uv+float2(-1,-1)*reciprocal)),ne=Luma(Fetch(uv+float2(1,-1)*reciprocal));
        float sw=Luma(Fetch(uv+float2(-1,1)*reciprocal)),se=Luma(Fetch(uv+float2(1,1)*reciprocal)),middle=Luma(center);
        float minimum=min(middle,min(min(nw,ne),min(sw,se))),maximum=max(middle,max(max(nw,ne),max(sw,se)));
        if(maximum-minimum>=max(0.0312,maximum*0.125))
        {
            float2 direction=float2(-((nw+ne)-(sw+se)),(nw+sw)-(ne+se));
            float reduction=max((nw+ne+sw+se)*(0.25/8),1.0/128);
            direction=clamp(direction/(min(abs(direction.x),abs(direction.y))+reduction),-8,8)*reciprocal;
            float3 a=0.5*(Fetch(uv+direction*(-1.0/6))+Fetch(uv+direction*(1.0/6)));
            float3 b=a*0.5+0.25*(Fetch(uv+direction*(-0.5))+Fetch(uv+direction*0.5));
            float luma=Luma(b);filtered=luma<minimum||luma>maximum?a:b;
        }
    }
    return float4(DecodeSrgb(filtered),1);
}
