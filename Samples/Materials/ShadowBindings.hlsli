Texture2D<float> directionalShadow:register(t3);
SamplerComparisonState shadowSampler:register(s1);
float ShadowVisibility(float3 position,float3 normal,float3 lightDirection)
{
    float visibility=1;
    if(framePointCount.y>0.5&&options.w>0.5)
    {
        float4 clip=mul(shadowClipFromWorld,float4(position+normal*shadowParameters.w,1));
        if(clip.w>0)
        {
            float3 coordinate=clip.xyz/clip.w;
            float2 uv=float2(coordinate.x*0.5+0.5,0.5-coordinate.y*0.5);
            if(all(uv>=0)&&all(uv<=1)&&coordinate.z>=0&&coordinate.z<=1)
            {
                float reference=coordinate.z-shadowParameters.y-shadowParameters.z*(1-saturate(dot(normal,lightDirection)));
                if(framePointCount.z<0.5)visibility=directionalShadow.SampleCmpLevelZero(shadowSampler,uv,reference);
                else
                {
                    float filtered=0;
                    [unroll]for(int y=-1;y<=1;++y)[unroll]for(int x=-1;x<=1;++x)
                        filtered+=directionalShadow.SampleCmpLevelZero(shadowSampler,uv+float2(x,y)*shadowParameters.x,reference);
                    visibility=filtered/9;
                }
            }
        }
    }
    return visibility;
}
