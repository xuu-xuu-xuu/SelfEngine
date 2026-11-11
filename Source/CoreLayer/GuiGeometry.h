#pragma once
#include "Math/Projection.h"
#include <cstdint>
#include <vector>

namespace se
{
// 自有 CPU 绘制数据；颜色是 sRGB RGB + 线性 alpha，纹理标识由宿主解析。
struct GuiVertex {Vec2 position,uv;std::uint32_t color=0xffffffff;};
static_assert(sizeof(GuiVertex)==20);
struct GuiCommand
{
    Vec4 clip; // 左、上、右、下，使用 displayPosition 所在坐标系。
    std::uint64_t texture=0;
    std::uint32_t indexOffset=0,indexCount=0,vertexOffset=0;
    bool textureIsData=false;
};
struct GuiGeometry
{
    Vec2 displayPosition{},displaySize{},framebufferScale{1,1};
    std::vector<GuiVertex> vertices;
    std::vector<std::uint32_t> indices;
    std::vector<GuiCommand> commands;
};
}
