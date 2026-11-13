cbuffer OverlayConstants : register(b0)
{
    column_major float4x4 clipFromLocal;
    float4 options; // x: 隐藏线；y: 轮廓宽度（物理像素）。
    column_major float4x4 worldFromClip; // 网格：反投影；options.x 是 DPI，zw 是目标尺寸倒数。
};
// R 保存不考虑场景遮挡的模型完整投影，G 保存通过场景深度测试的像素。
Texture2D<float4> selectionMask : register(t0);
// 与 GUI 顶点颜色共用 sRGB 约定；场景 LDR 在最终 GUI 合成前存储线性值。
float3 ToLinear(float3 c) {return lerp(c/12.92,pow(abs((c+.055)/1.055),2.4),step(.04045,c));}

float4 VSMask(float3 position : POSITION) : SV_POSITION
{
    return mul(clipFromLocal, float4(position, 1));
}
float4 PSMask() : SV_TARGET { return 1; }

struct LineInput {float4 position : POSITION;float4 color : COLOR0;float hiddenAlpha : TEXCOORD0;};
struct LineOutput {float4 position : SV_POSITION;float4 color : COLOR0;float hiddenAlpha : TEXCOORD0;};
LineOutput VSLine(LineInput input)
{
    LineOutput output;output.position=input.position;output.color=input.color;output.hiddenAlpha=input.hiddenAlpha;return output;
}
float4 PSLine(LineOutput input) : SV_TARGET
{
    float4 color=input.color;color.a*=options.x>.5?input.hiddenAlpha:1;
    clip(color.a-.001);color.rgb=ToLinear(color.rgb);return color;
}
float4 VSOutline(uint id : SV_VertexID) : SV_POSITION
{
    return float4(id==1?3:-1,id==2?-3:1,0,1);
}
// 固定的 1 米格线和 10 米主线；相机只改变投影，不改变世界空间间距。
float GridCoverage(float2 worldXZ,float2 footprint,float spacing)
{
    float2 distance=abs(frac(worldXZ/spacing+.5)-.5)*spacing/(footprint*options.x);
    float2 coverage=(1-smoothstep(.5,1.5,distance))*saturate(1-footprint/spacing);
    return max(coverage.x,coverage.y);
}
struct GridOutput {float4 color : SV_TARGET;float depth : SV_DEPTH;};
GridOutput PSGrid(float4 position : SV_POSITION)
{
    float2 ndc=position.xy*options.zw*float2(2,-2)+float2(-1,1);
    float4 nearH=mul(worldFromClip,float4(ndc,0,1));
    float4 farH=mul(worldFromClip,float4(ndc,1,1));
    float3 nearPoint=nearH.xyz/nearH.w,farPoint=farH.xyz/farH.w;
    float dy=farPoint.y-nearPoint.y;
    float t=-nearPoint.y/(abs(dy)>1e-7?dy:1e-7);
    float3 gridPoint=nearPoint+t*(farPoint-nearPoint);
    float2 footprint=max(fwidth(gridPoint.xz),1e-5);
    float minor=GridCoverage(gridPoint.xz,footprint,1),major=GridCoverage(gridPoint.xz,footprint,10);
    float2 axes=1-smoothstep(.5,1.5,abs(gridPoint.xz)/(footprint*options.x));
    float alpha=max(minor*.35,major*.5);
    float3 color=float3(.45,.48,.5);
    // X 轴红、Z 轴蓝，与变换手柄约定一致。
    color=lerp(color,float3(.95,.25,.25),axes.y);alpha=max(alpha,axes.y*.8);
    color=lerp(color,float3(.3,.6,1),axes.x);alpha=max(alpha,axes.x*.8);
    alpha*=1-smoothstep(.3,1,t); // 远裁剪前渐隐，避免暴露裁剪边界。
    float4 clipPoint=mul(clipFromLocal,float4(gridPoint,1));
    GridOutput output;output.depth=clipPoint.z/clipPoint.w-1e-6;
    if(abs(dy)<1e-7||t<=0||t>=1||output.depth<0||output.depth>1||alpha<.001)discard;
    output.color=float4(ToLinear(color),alpha);return output;
}
float4 PSOutline(float4 position : SV_POSITION) : SV_TARGET
{
    uint width,height;selectionMask.GetDimensions(width,height);
    int2 p=int2(position.xy);if(selectionMask.Load(int3(p,0)).g<.5)discard;
    int radius=(int)ceil(options.y);bool edge=false;
    [loop] for(int y=-radius;y<=radius;++y)
        [loop] for(int x=-radius;x<=radius;++x)
        {
            if(x*x+y*y>radius*radius)continue;
            int2 q=p+int2(x,y);
            // 视口裁剪也不是模型轮廓；不能把纹理外侧当成背景，给屏幕边框描边。
            if(any(q<0)||q.x>=(int)width||q.y>=(int)height)continue;
            if(selectionMask.Load(int3(q,0)).r<.5)edge=true;
        }
    if(!edge)discard;
    return float4(ToLinear(float3(1,.6,.08)),1);
}
