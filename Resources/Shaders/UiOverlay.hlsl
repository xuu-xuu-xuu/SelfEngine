cbuffer OverlayConstants:register(b0)
{
    float4 uiRectangle;
    float4 uiColor;
    float4 uiViewport;
};
float4 VSMain(uint vertex:SV_VertexID):SV_POSITION
{
    float2 corners[6]={float2(0,0),float2(0,1),float2(1,0),float2(1,0),float2(0,1),float2(1,1)};
    float2 pixel=uiRectangle.xy+corners[vertex]*uiRectangle.zw;
    return float4(pixel/uiViewport.xy*float2(2,-2)+float2(-1,1),0,1);
}
float4 PSMain(float4 position:SV_POSITION):SV_TARGET{return uiColor;}
