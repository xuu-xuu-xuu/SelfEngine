// G06.10：Forward 实例化。实例数据（t7）提供逐实例矩阵/材质，b1 提供前向灯与曝光；
// 布局 30 的根签名同时保留 b0（相机位置）与纹理表。当前只组合 SE_FORWARD，
// 阴影/环境仍走非实例化 Forward 路径；需要时再扩 ShadowBindings/EnvironmentBindings。
#define SE_INSTANCED 1
#define SE_FORWARD 1
#include "Pbr.hlsl"
