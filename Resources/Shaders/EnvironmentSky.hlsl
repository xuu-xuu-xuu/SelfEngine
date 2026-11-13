cbuffer EnvironmentSkyConstants:register(b0)
{
    column_major float4x4 skyFromClip;
    float4 environmentParameters;
};
TextureCube<float4> specularMap:register(t0);
SamplerState environmentSampler:register(s2);
struct PixelInput {float4 position:SV_POSITION;float3 direction:TEXCOORD0;};
PixelInput VSMain(uint vertex:SV_VertexID)
{
    float2 positions[3]={float2(-1,-1),float2(-1,3),float2(3,-1)};
    PixelInput output;output.position=float4(positions[vertex],0,1);
    output.direction=mul(skyFromClip,float4(positions[vertex],1,1)).xyz;return output;
}
float4 PSMain(PixelInput input):SV_TARGET
{
    float3 direction=dot(input.direction,input.direction)>1e-12?normalize(input.direction):float3(0,0,-1);
    direction=float3(environmentParameters.y*direction.x-environmentParameters.z*direction.z,direction.y,
        environmentParameters.z*direction.x+environmentParameters.y*direction.z);
    return float4(min(max(specularMap.SampleLevel(environmentSampler,direction,0).rgb*environmentParameters.x,0),65504),1);
}
