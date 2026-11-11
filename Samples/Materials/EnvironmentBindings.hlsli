TextureCube<float4> irradianceMap:register(t4);
TextureCube<float4> specularMap:register(t5);
Texture2D<float4> brdfMap:register(t6);
SamplerState environmentSampler:register(s2);
float3 EnvironmentDirection(float3 direction)
{
    return float3(environmentParameters.y*direction.x-environmentParameters.z*direction.z,direction.y,
        environmentParameters.z*direction.x+environmentParameters.y*direction.z);
}
float3 EnvironmentBrdf(float3 color,float metal,float rough,float3 n,float3 v)
{
    float nv=saturate(dot(n,v));float3 f0=lerp(0.04.xxx,color,metal);
    float3 fresnel=f0+(1-f0)*pow(1-nv,5);
    float3 diffuse=irradianceMap.SampleLevel(environmentSampler,EnvironmentDirection(n),0).rgb*color*(1-metal)*(1-fresnel)/3.141592653589793;
    float3 reflected=reflect(-v,n);
    float3 radiance=specularMap.SampleLevel(environmentSampler,EnvironmentDirection(reflected),rough*environmentParameters.w).rgb;
    float2 integrated=brdfMap.SampleLevel(environmentSampler,float2(nv,rough),0).rg;
    return (diffuse+radiance*(f0*integrated.x+integrated.y))*environmentParameters.x;
}
