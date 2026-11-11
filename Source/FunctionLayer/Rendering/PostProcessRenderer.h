#pragma once
#include "PlatformLayer/Graphics/Rhi.h"
#include "FunctionLayer/GameUI/UiTree.h"
#include "ResourceLayer/Assets/StaticModel.h"

namespace se
{
class PostProcessRenderer
{
public:
    bool Initialize(rhi::Device& device,const std::filesystem::path& shaders,bool acesToneMap=false);
    rhi::PostProcessPipelines Bindings() const;
    void Clear(){pipelines_={};}
private:
    std::array<rhi::Pipeline,6> pipelines_;
};
rhi::PostProcessSettings PreparePostProcessSettings(const rhi::PostProcessSettings& source,std::vector<AssetDiagnostic>& diagnostics);
std::vector<rhi::OverlayQuad> PreparePostProcessOverlays(std::span<const UiVisual> visuals,std::vector<AssetDiagnostic>& diagnostics);
rhi::Result RecordPostProcessPasses(rhi::CommandContext& commands,const rhi::PostProcessSettings& settings,std::span<const rhi::OverlayQuad> overlays);
}
