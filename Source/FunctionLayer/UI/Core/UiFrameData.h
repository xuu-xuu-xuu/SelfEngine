#pragma once

#include "CoreLayer/GuiGeometry.h"

#include <cstdint>

namespace se::ui
{

enum class UiLayerRole : std::uint8_t
{
    Editor = 0,
    Game,
};

// 按帧借用：geometry 由 UI 线程拥有，FrameData 不跨帧持有指针。
struct UiFrameData
{
    GuiGeometry geometry;
    UiLayerRole role = UiLayerRole::Editor;
};

} // namespace se::ui
