#include "GuiCompositor.h"
#include "FunctionLayer/Application/FrameData.h"
namespace se
{
rhi::Result GuiCompositor::Configure(rhi::Device& device,rhi::CommandContext& commands,const FrameData& f,
    rhi::TextureHandle scene,std::uint32_t width,std::uint32_t height,const std::filesystem::path& shaders)
{
    if(!f.editorFontPixels||f.editorFontWidth<=0||f.editorFontHeight<=0||f.editorFontWidth>8192||f.editorFontHeight>8192
        ||f.editorFontToken!=FontToken||!scene||!width||!height
        ||!IsFinite(f.editorGui.framebufferScale.x)||!IsFinite(f.editorGui.framebufferScale.y)
        ||f.editorGui.framebufferScale.x<=0||f.editorGui.framebufferScale.y<=0)return {rhi::Error::InvalidArgument};
    if(!pipeline_)
    {
        rhi::PipelineDescription p{rhi::PipelineKind::Gui,shaders/"Gui.hlsl",rhi::GuiLayoutVersion};
        p.colorFormat=rhi::ColorFormat::Rgba8Srgb;p.blend=rhi::BlendMode::Alpha;
        auto result=device.CreatePipeline(p,pipeline_);if(!result)return result;
    }
    if(!font_||fontWidth_!=static_cast<unsigned>(f.editorFontWidth)||fontHeight_!=static_cast<unsigned>(f.editorFontHeight)
        ||fontRevision_!=f.editorFontRevision)
    {
        rhi::Texture next;auto result=device.CreateTexture({static_cast<unsigned>(f.editorFontWidth),static_cast<unsigned>(f.editorFontHeight),1,1,rhi::TextureFormat::Rgba8,"GuiFont"},
            {f.editorFontPixels,size_t(f.editorFontWidth)*f.editorFontHeight*4},next);if(!result)return result;
        font_=std::move(next);fontWidth_=f.editorFontWidth;fontHeight_=f.editorFontHeight;fontRevision_=f.editorFontRevision;
    }
    if(!white_)
    {
        const unsigned char pixel[4]={255,255,255,255};
        rhi::Texture next;auto result=device.CreateTexture({1,1,1,1,rhi::TextureFormat::Rgba8,"GuiWhite"},{pixel,4},next);if(!result)return result;white_=std::move(next);
    }
    for(const FrameData::UiImageSource& image : f.editorUiImages)
    {
        if(image.token==0||image.token==FontToken||image.token==SceneToken||image.token==WhiteToken||
            image.width==0||image.height==0||image.width>8192||image.height>8192||!image.rgba8||
            image.bytes!=static_cast<std::size_t>(image.width)*image.height*4)return {rhi::Error::InvalidArgument};
        const auto size=imageSizes_.find(image.token);
        if(size==imageSizes_.end()||size->second.first!=image.width||size->second.second!=image.height)
        {
            rhi::Texture next;
            auto result=device.CreateTexture({image.width,image.height,1,1,rhi::TextureFormat::Rgba8,"GuiImage"},
                {image.rgba8,image.bytes},next);
            if(!result)return result;
            images_[image.token]=std::move(next);
            imageSizes_[image.token]={image.width,image.height};
        }
    }
    if(!composite_||width!=width_||height!=height_)
    {rhi::Texture next;auto result=device.CreateRenderTarget({width,height,false,true},next);if(!result)return result;composite_=std::move(next);width_=width;height_=height;}
    std::vector<rhi::TextureHandle> textures;textures.reserve(f.editorGui.commands.size());
    for(const auto& command:f.editorGui.commands)
    {
        if(command.texture==FontToken)textures.push_back(*font_);
        else if(command.texture==SceneToken)textures.push_back(scene);
        else if(command.texture==WhiteToken)textures.push_back(*white_);
        else if(const auto image=images_.find(command.texture);image!=images_.end())textures.push_back(*image->second);
        else return {rhi::Error::InvalidHandle};
    }
    return commands.ConfigureGui(scene,*composite_,*pipeline_,f.editorGui,textures);
}
}
