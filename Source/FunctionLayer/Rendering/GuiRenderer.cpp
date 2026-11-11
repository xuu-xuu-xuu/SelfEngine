#include "GuiRenderer.h"
#include "FunctionLayer/Application/FrameData.h"
#include "PlatformLayer/Windows/ExecutablePath.h"
#include <cmath>
namespace se
{
rhi::Result GuiRenderer::Configure(rhi::Device& device,rhi::CommandContext& commands,const FrameData& f,std::uint32_t width,std::uint32_t height,const std::filesystem::path& shaders)
{
    if(!f.editorFontPixels||f.editorFontWidth<=0||f.editorFontHeight<=0||f.editorFontWidth>8192||f.editorFontHeight>8192||f.editorFontToken!=FontToken
        ||!IsFinite(f.sceneViewport.width)||!IsFinite(f.sceneViewport.height)||f.sceneViewport.width<1||f.sceneViewport.height<1
        ||f.sceneViewport.width>8192||f.sceneViewport.height>8192
        ||!IsFinite(f.editorGui.framebufferScale.x)||!IsFinite(f.editorGui.framebufferScale.y)||f.editorGui.framebufferScale.x<=0||f.editorGui.framebufferScale.y<=0
        ||f.sceneViewport.width*f.editorGui.framebufferScale.x>8192||f.sceneViewport.height*f.editorGui.framebufferScale.y>8192)return {rhi::Error::InvalidArgument};
    const auto sw=static_cast<unsigned>(std::max(1l,std::lround(f.sceneViewport.width*f.editorGui.framebufferScale.x)));
    const auto sh=static_cast<unsigned>(std::max(1l,std::lround(f.sceneViewport.height*f.editorGui.framebufferScale.y)));
    if(!scene_||sw!=sceneWidth_||sh!=sceneHeight_)
    {rhi::Texture next;auto result=device.CreateRenderTarget({sw,sh,true,false},next);if(!result)return result;scene_=std::move(next);sceneWidth_=sw;sceneHeight_=sh;}
    // 托管宿主同样使用 toolkit 的白纹理和预览图 token，复用唯一的资源解析与合成契约。
    return compositor_.Configure(device,commands,f,*scene_,width,height,shaders);
}
}
