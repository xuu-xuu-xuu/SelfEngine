#pragma once
#include "ResourceLayer/Assets/AssetStore.h"
#include "FunctionLayer/Runtime/Camera.h"
#include "PlatformLayer/Graphics/Rhi.h"

namespace se
{
struct RenderEnvironment
{
    std::shared_ptr<const EnvironmentAsset> source;
    std::array<rhi::Texture,3> textures;
};
// 仅保留当前有效版本；命令包负责使被替换的 GPU 版本活到自身 Fence。
class EnvironmentRenderCache
{
public:
    std::shared_ptr<const RenderEnvironment> Load(AssetStore& assets,const AssetId& id,rhi::Device& device,std::vector<AssetDiagnostic>& diagnostics);
    bool Upload(std::shared_ptr<const EnvironmentAsset> source,rhi::Device& device,std::shared_ptr<const RenderEnvironment>& output,std::vector<AssetDiagnostic>& diagnostics);
    std::shared_ptr<const RenderEnvironment> Fallback(rhi::Device& device,std::vector<AssetDiagnostic>& diagnostics);
    void Clear(){active_.reset();fallback_.reset();}
private:
    std::shared_ptr<const RenderEnvironment> active_,fallback_;
};
rhi::EnvironmentBindings PrepareEnvironmentBindings(const RenderEnvironment& environment,const CameraSnapshot& camera,float intensity,float yaw,
    std::vector<AssetDiagnostic>& diagnostics);
}
