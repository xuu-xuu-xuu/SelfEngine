#include "EnvironmentRender.h"
#include "ResourceLayer/Assets/AssetIO.h"
#include <algorithm>

namespace se
{
namespace
{
struct Batch
{
    rhi::Device& device;bool ended=false;
    ~Batch(){if(!ended){rhi::CompletionToken ignored;device.EndUploadBatch(ignored);}}
    rhi::Result Finish(){ended=true;rhi::CompletionToken token;return device.EndUploadBatch(token);}
};
bool UploadTextures(const std::array<EnvironmentTexture,3>& textures,rhi::Device& device,RenderEnvironment& output,std::vector<AssetDiagnostic>& diagnostics)
{
    auto result=device.BeginUploadBatch();if(!result){asset_io::Diagnostic(diagnostics,result.Key(),"Environment");return false;}Batch batch{device};
    for(size_t index=0;index<textures.size();++index)
    {
        const auto& texture=textures[index];std::vector<rhi::TextureSubresource> subresources;subresources.reserve(texture.subresources.size());
        for(const auto& bytes:texture.subresources)subresources.push_back({{reinterpret_cast<const std::uint8_t*>(bytes.data()),bytes.size()*sizeof(std::uint16_t)},0});
        const rhi::TextureDescription description{texture.size,texture.size,texture.mipLevels,1,rhi::TextureFormat::Rgba16Float,"Environment."+std::to_string(index),texture.cube?6u:1u,texture.cube};
        result=device.CreateTextureSubresources(description,subresources,output.textures[index]);
        if(!result){asset_io::Diagnostic(diagnostics,result.Key(),description.name);return false;}
    }
    result=batch.Finish();if(!result)asset_io::Diagnostic(diagnostics,result.Key(),"Environment");return bool(result);
}
}
bool EnvironmentRenderCache::Upload(std::shared_ptr<const EnvironmentAsset> source,rhi::Device& device,std::shared_ptr<const RenderEnvironment>& output,std::vector<AssetDiagnostic>& diagnostics)
{
    if(!source||!ValidateEnvironment(*source)){asset_io::Diagnostic(diagnostics,"Environment.ContainerInvalid",{});return false;}
    auto candidate=std::make_shared<RenderEnvironment>();candidate->source=std::move(source);
    if(!UploadTextures(candidate->source->textures,device,*candidate,diagnostics))return false;
    output=std::move(candidate);return true;
}
std::shared_ptr<const RenderEnvironment> EnvironmentRenderCache::Load(AssetStore& assets,const AssetId& id,rhi::Device& device,std::vector<AssetDiagnostic>& diagnostics)
{
    auto source=assets.LoadEnvironment(id,diagnostics);if(!source)return active_;
    if(active_&&active_->source==source)return active_;
    std::shared_ptr<const RenderEnvironment> candidate;if(!Upload(source,device,candidate,diagnostics))return active_;
    active_=std::move(candidate);return active_;
}
std::shared_ptr<const RenderEnvironment> EnvironmentRenderCache::Fallback(rhi::Device& device,std::vector<AssetDiagnostic>& diagnostics)
{
    if(fallback_)return fallback_;auto candidate=std::make_shared<RenderEnvironment>();std::array<EnvironmentTexture,3> textures;
    for(size_t index=0;index<textures.size();++index)
    {auto& texture=textures[index];texture.size=1;texture.cube=index<2;texture.subresources.assign(index<2?6:1,{0,0,0,0x3c00});}
    if(!UploadTextures(textures,device,*candidate,diagnostics))return {};fallback_=std::move(candidate);return fallback_;
}
rhi::EnvironmentBindings PrepareEnvironmentBindings(const RenderEnvironment& environment,const CameraSnapshot& camera,float intensity,float yaw,std::vector<AssetDiagnostic>& diagnostics)
{
    bool limited=false;if(!IsFinite(intensity)){intensity=0;limited=true;}if(!IsFinite(yaw)){yaw=0;limited=true;}
    const float bounded=std::clamp(intensity,0.0f,64.0f);limited|=bounded!=intensity;
    if(limited)asset_io::Diagnostic(diagnostics,"Rendering.EnvironmentClamped",{},true);
    auto rotation=camera.inverseView;rotation(0,3)=rotation(1,3)=rotation(2,3)=0;
    return {*environment.textures[0],*environment.textures[1],*environment.textures[2],rotation*camera.inverseProjection,bounded,std::remainder(yaw,2*Pi)};
}
}
