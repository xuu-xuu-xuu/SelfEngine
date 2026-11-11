#pragma once

#include "PlatformLayer/Graphics/Rhi.h"

namespace se
{
struct FrameData;
// 场景渲染路径只描述需要的资源与 Pass 组合，不解释编辑器面板/UI。
enum class SceneRenderPath { PbrPreview,ForwardShadowed,Environment };
struct SceneRenderRequest
{
    const FrameData* frame=nullptr;        // 只在本帧有效。
    const rhi::SceneTargets* targets=nullptr; // 由调用方保活到 Render 返回。
    SceneRenderPath path=SceneRenderPath::ForwardShadowed;
};
}
