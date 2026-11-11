float4 VSMain(uint vertex:SV_VertexID):SV_POSITION
{
    float2 positions[3]={float2(-1,-1),float2(-1,3),float2(3,-1)};
    return float4(positions[vertex],0,1);
}
