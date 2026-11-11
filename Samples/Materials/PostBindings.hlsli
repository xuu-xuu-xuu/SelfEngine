cbuffer PostConstants:register(b0)
{
    float4 postParameters; // 曝光、阈值、强度、色调映射开关。
    float4 postResolution; // 输出宽高、输入宽高倒数。
    float4 postOptions; // G15：x=使用 GPU 自动曝光，y=使用颜色 LUT；FXAA/模糊仍用各自槽位。
};
float3 EncodeSrgb(float3 value)
{
    value=saturate(value);
    return lerp(value*12.92,1.055*pow(value,1.0/2.4)-0.055,step(0.0031308,value));
}
float3 DecodeSrgb(float3 value)
{
    value=saturate(value);
    return lerp(value/12.92,pow((value+0.055)/1.055,2.4),step(0.04045,value));
}
