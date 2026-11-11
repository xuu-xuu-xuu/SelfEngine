#pragma once
#include "FunctionLayer/GameUI/UiTree.h"
#include "PlatformLayer/Graphics/Rhi.h"
#include <cstdint>
#include <vector>

class Localization;

namespace se
{
// G06.09：运行时遮挡 HUD 的纯 CPU 组帧。输入只读 RHI 已完成的 OcclusionCounters，
// 输出 UiVisual；不接触 Device/CommandContext/GameUIRenderer，便于独立测试与复用到其它宿主。
// 计数尚未回读时显示等待文案；可见/剔除比例条只表达回读结果，不用 CPU 估算替代。
std::vector<UiVisual> BuildOcclusionHud(const rhi::OcclusionCounters& counters,const Localization& text,
    std::uint32_t surfaceWidth,std::uint32_t surfaceHeight);
}
