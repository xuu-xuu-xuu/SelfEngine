#include "PbrBindings.hlsli"
#ifdef SE_INSTANCED
#include "InstancingBindings.hlsli"
#endif
#ifdef SE_FORWARD
#include "ForwardBindings.hlsli"
#ifdef SE_SHADOWS
#include "ShadowBindings.hlsli"
#endif
#ifdef SE_ENVIRONMENT
#include "EnvironmentBindings.hlsli"
#endif
#endif
#ifdef SE_VERTEX_COMPRESSED
struct VertexInput {float4 position:POSITION;float2 normal:NORMAL;float2 uv:TEXCOORD0;float4 tangent:TANGENT;};
float3 DecodeOctahedral(float2 encoded)
{
    float2 e=encoded*2.0-1.0;
    float3 n=float3(e,1.0-abs(e.x)-abs(e.y));
    if(n.z<0)n.xy=(1.0-abs(n.yx))*float2(e.x<0?-1.0:1.0,e.y<0?-1.0:1.0);
    return normalize(n);
}
#else
struct VertexInput {float3 position:POSITION;float3 normal:NORMAL;float2 uv:TEXCOORD0;float4 tangent:TANGENT;};
#endif
struct PixelInput
{
    float4 position:SV_POSITION;
    float3 worldPosition:TEXCOORD1;
    float3 normal:NORMAL;
    float2 uv:TEXCOORD0;
    float4 tangent:TANGENT;
#ifdef SE_INSTANCED
    nointerpolation uint instanceIndex:TEXCOORD2;
#endif
};
PixelInput VSMain(VertexInput input
#ifdef SE_INSTANCED
    ,uint instanceIndex:SV_InstanceID
#endif
)
{
    PixelInput output;
#ifdef SE_VERTEX_COMPRESSED
    float3 localPosition=input.position.xyz;
    float3 localNormal=DecodeOctahedral(input.normal);
    float4 localTangent=float4(input.tangent.xyz*2.0-1.0,input.tangent.w*2.0-1.0);
#else
    float3 localPosition=input.position;
    float3 localNormal=input.normal;
    float4 localTangent=input.tangent;
#endif
#ifdef SE_INSTANCED
    uint start=instanceIndex*instanceStrideBytes;
    float4x4 instanceClip=InstanceMatrix(start),instanceWorld=InstanceMatrix(start+64),instanceNormal=InstanceMatrix(start+128);
    output.position=mul(instanceClip,float4(localPosition,1));
    output.worldPosition=mul(instanceWorld,float4(localPosition,1)).xyz;
    output.normal=mul(instanceNormal,float4(localNormal,0)).xyz;
    output.tangent=float4(mul(instanceWorld,float4(localTangent.xyz,0)).xyz,localTangent.w);
    output.instanceIndex=instanceIndex;
#else
    output.position=mul(clipFromLocal,float4(localPosition,1));
    output.worldPosition=mul(worldFromLocal,float4(localPosition,1)).xyz;
    output.normal=mul(normalFromLocal,float4(localNormal,0)).xyz;
    output.tangent=float4(mul(worldFromLocal,float4(localTangent.xyz,0)).xyz,localTangent.w);
#endif
    output.uv=input.uv;return output;
}
float4 PSMain(PixelInput input,bool frontFace:SV_IsFrontFace):SV_TARGET
{
#ifdef SE_INSTANCED
    uint start=input.instanceIndex*instanceStrideBytes;
    float4 materialBaseColor=asfloat(instanceData.Load4(start+192)),materialParameters=asfloat(instanceData.Load4(start+208)),materialOptions=asfloat(instanceData.Load4(start+224));
#else
    float4 materialBaseColor=baseColor,materialParameters=parameters,materialOptions=options;
#endif
    // G05.08：LOD 过渡像素 dither。|options.w|<1 时按 4×4 Bayer 阈值二分：
    // 负值绘制旧级（丢弃“已切到新级”的像素），正值绘制新级（丢弃“仍保留旧级”的像素），两组像素互不重叠。
    const float lodDither=materialOptions.w;
    if(lodDither!=0.0&&abs(lodDither)<1.0)
    {
        const bool usesPrevious=abs(lodDither)<=LodBayerThreshold(uint2(input.position.xy));
        if((lodDither<0.0)!=usesPrevious)discard;
    }
    bool logicalFront=frontFace==(materialParameters.w>0);
    if(!logicalFront&&materialOptions.z<0.5)discard;
    float3 color=baseColorMap.Sample(materialSampler,input.uv).rgb*materialBaseColor.rgb;
    float3 n=UnitVector(input.normal,float3(0,0,1));
    float3 geometricNormal=n;
    if(materialOptions.x>0.5&&abs(materialParameters.z)>1e-8)
    {
        float3 fallback=abs(n.y)<0.99?UnitVector(cross(float3(0,1,0),n),float3(1,0,0)):UnitVector(cross(float3(1,0,0),n),float3(0,0,1));
        float3 t=UnitVector(input.tangent.xyz-n*dot(n,input.tangent.xyz),fallback);
        float3 b=cross(n,t)*input.tangent.w*materialParameters.w;
#ifdef SE_NORMAL_BC5
        // BC5 只存法线 XY，Z 由单位向量约束重建；与 RGBA8/BC1 的 xyz 路径等价。
        float2 mappedXY=normalMap.Sample(materialSampler,input.uv).rg*2-1;
        float3 mapped=float3(mappedXY,sqrt(saturate(1-dot(mappedXY,mappedXY))));
#else
        float3 mapped=normalMap.Sample(materialSampler,input.uv).xyz*2-1;
#endif
        mapped.xy*=materialParameters.z;mapped=UnitVector(mapped,float3(0,0,1));
        n=UnitVector(t*mapped.x+b*mapped.y+n*mapped.z,n);
    }
    if(!logicalFront){n=-n;geometricNormal=-geometricNormal;}
    float2 mr=metallicRoughnessMap.Sample(materialSampler,input.uv).gb;
    float metal=saturate(materialParameters.x*mr.y),rough=max(0.045,saturate(materialParameters.y*mr.x));
    float3 v=UnitVector(cameraPosition.xyz-input.worldPosition,float3(0,0,1));
    float3 l=UnitVector(directionToLight.xyz,float3(0,0,1));
#ifdef SE_FORWARD
    float3 lit=float3(0,0,0);
    if(frameDirection.w>0.5)
    {
        float3 direction=UnitVector(frameDirection.xyz,float3(0,0,1));
        float visibility=1;
#ifdef SE_SHADOWS
        visibility=ShadowVisibility(input.worldPosition,geometricNormal,direction);
#endif
        lit+=DirectBrdf(color,metal,rough,n,v,direction)*frameRadiance.rgb*visibility;
    }
    [loop] for(uint index=0;index<(uint)framePointCount.x;++index)
    {
        float3 delta=pointPositionRadius[index].xyz-input.worldPosition;float squared=dot(delta,delta);
        float radius=pointPositionRadius[index].w;
        float ratio=squared/(radius*radius),window=saturate(1-ratio*ratio);
        float attenuation=window*window/max(squared,0.0025);
        lit+=DirectBrdf(color,metal,rough,n,v,UnitVector(delta,n))*pointRadiance[index].rgb*attenuation;
    }
#ifdef SE_ENVIRONMENT
    lit+=EnvironmentBrdf(color,metal,rough,n,v);
#endif
    return float4(min(max(materialOptions.y>0.5?color:lit,0),65504),1);
#else
    float3 lit=DirectBrdf(color,metal,rough,n,v,l)*radiance.rgb;
    return float4(OutputSrgb(materialOptions.y>0.5?color:lit),1);
#endif
}
