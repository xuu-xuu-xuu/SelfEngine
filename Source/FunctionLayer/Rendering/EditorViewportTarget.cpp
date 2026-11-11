#include "EditorViewportTarget.h"

namespace se
{
bool NullEditorViewportTarget::Resize(std::uint32_t width,std::uint32_t height,std::string& errorKey)
{
    errorKey="Rendering.EditorTargetUnavailable";
    return width!=0&&height!=0&&targets_.width==width&&targets_.height==height;
}
EditorViewportTargetResource::EditorViewportTargetResource(rhi::Device& device,std::uint32_t width,std::uint32_t height)
    :device_(&device)
{
    std::string ignored;Resize(width,height,ignored);
}
bool EditorViewportTargetResource::Resize(std::uint32_t width,std::uint32_t height,std::string& errorKey)
{
    errorKey.clear();
    if(!device_||!width||!height||width>8192||height>8192){errorKey="Rendering.EditorTargetInvalid";return false;}
    if(targets_.width==width&&targets_.height==height&&Ready())return true;
    rhi::Texture hdr,ldr;
    auto result=device_->CreateRenderTarget({width,height,true,false,rhi::TextureFormat::Rgba16Float},hdr);
    if(!result){errorKey="Rendering.EditorTargetUnavailable";return false;}
    result=device_->CreateRenderTarget({width,height,true,false,rhi::TextureFormat::Rgba8},ldr);
    if(!result){errorKey="Rendering.EditorTargetUnavailable";return false;}
    hdr_=std::move(hdr);ldr_=std::move(ldr);
    targets_.hdrColor=*hdr_;targets_.ldrColor=*ldr_;targets_.depth=*ldr_;targets_.width=width;targets_.height=height;
    return true;
}
}
