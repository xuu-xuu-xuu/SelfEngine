cbuffer ShadowConstants:register(b0){column_major float4x4 clipFromWorld;};
float4 VSMain(float3 position:POSITION):SV_POSITION{return mul(clipFromWorld,float4(position,1));}
void PSMain(){}
