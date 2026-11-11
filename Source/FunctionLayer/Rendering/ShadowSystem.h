#pragma once
#include "FunctionLayer/Application/FrameData.h"
#include "PlatformLayer/Graphics/Rhi.h"
#include "ResourceLayer/Assets/StaticModel.h"

namespace se
{
// 使用 Forward 已选择的方向，不重新选择灯光；失败不替换上次有效配置。
bool PrepareDirectionalShadow(const DirectionalShadowView& view,const rhi::ForwardLighting& lighting,
    rhi::ShadowSettings& output,std::vector<AssetDiagnostic>& diagnostics);
}
