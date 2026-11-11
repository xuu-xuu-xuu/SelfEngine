#pragma once
#include "ResourceLayer/Assets/HlodBake.h"

namespace se
{
// G05.09：HLOD 单元选择只依赖“单元误差 → 屏幕像素误差”，不绑定渲染实现。
struct HlodView
{
    Vec3 position{};
    float verticalFov=Pi/3;
    float viewportHeight=1080;
    float nearPlane=0.1f;
};
float ProjectedHlodError(const HlodCell& cell,const HlodView& view);
// 1.25× 迟滞：未用代理时需要误差 ≤ threshold/1.25，已用代理时误差 > threshold×1.25 才切回实例。
// 每个单元独立判定；实例只属于一个单元，因此相邻单元切换时机不同也不会产生重叠或空洞。
bool SelectHlodProxy(const HlodCell& cell,const HlodView& view,float thresholdPixels,bool currentlyProxy);
}
