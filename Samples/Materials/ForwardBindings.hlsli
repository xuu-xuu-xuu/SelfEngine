cbuffer ForwardConstants:register(b1)
{
    float4 frameDirection;
    float4 frameRadiance;
    float4 frameSkyExposure;
    float4 framePointCount;
    float4 pointPositionRadius[8];
    float4 pointRadiance[8];
#ifdef SE_SHADOWS
    column_major float4x4 shadowClipFromWorld;
    float4 shadowParameters;
#endif
#ifdef SE_ENVIRONMENT
    float4 environmentParameters;
#endif
};
