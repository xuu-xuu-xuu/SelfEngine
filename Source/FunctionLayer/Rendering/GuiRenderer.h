#pragma once
#include "GuiCompositor.h"
namespace se
{
struct FrameData;
class GuiRenderer
{
public:
    static constexpr std::uint64_t FontToken=GuiCompositor::FontToken,SceneToken=GuiCompositor::SceneToken;
    rhi::Result Configure(rhi::Device& device,rhi::CommandContext& commands,const FrameData& frame,
        std::uint32_t width,std::uint32_t height,const std::filesystem::path& shaders);
    void Clear(){compositor_.Clear();scene_.reset();sceneWidth_=sceneHeight_=0;}
private:
    GuiCompositor compositor_;
    rhi::Texture scene_;
    std::uint32_t sceneWidth_=0,sceneHeight_=0;
};
}
