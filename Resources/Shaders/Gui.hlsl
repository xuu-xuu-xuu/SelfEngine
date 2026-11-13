cbuffer GuiConstants : register(b0) { float4 guiScaleTranslate; };
Texture2D<float4> guiTexture : register(t0);
SamplerState guiSampler : register(s0);
struct Input {float2 position:POSITION;float2 uv:TEXCOORD0;float4 color:COLOR0;};
struct Varying {float4 position:SV_Position;float2 uv:TEXCOORD0;float4 color:COLOR0;};
float3 ToLinear(float3 c) {return lerp(c/12.92,pow(abs((c+0.055)/1.055),2.4),step(0.04045,c));}
Varying VSMain(Input v)
{
    Varying o;o.position=float4(v.position*guiScaleTranslate.xy+guiScaleTranslate.zw,0,1);
    o.uv=v.uv;o.color=float4(ToLinear(v.color.rgb),v.color.a);return o;
}
float4 PSMain(Varying v):SV_Target {return v.color*guiTexture.Sample(guiSampler,v.uv);}
