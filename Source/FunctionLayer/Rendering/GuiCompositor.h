#pragma once
#include "PlatformLayer/Graphics/Rhi.h"
#include <cstdint>
#include <unordered_map>
#include <utility>
namespace se
{
struct FrameData;
// 只负责 GUI 字体、合成目标和最终 GUI 合成；3D 场景目标由 EditorViewportTarget 拥有。
class GuiCompositor
{
public:
    static constexpr std::uint64_t FontToken=1,SceneToken=2,WhiteToken=3;
    rhi::Result Configure(rhi::Device& device,rhi::CommandContext& commands,const FrameData& frame,
        rhi::TextureHandle scene,std::uint32_t width,std::uint32_t height,const std::filesystem::path& shaders);
    void Clear(){font_.reset();composite_.reset();white_.reset();pipeline_.reset();images_.clear();imageSizes_.clear();fontWidth_=fontHeight_=width_=height_=0;fontRevision_=0;}
private:
    rhi::Texture font_,composite_,white_;
    std::unordered_map<std::uint64_t, rhi::Texture> images_;
    std::unordered_map<std::uint64_t, std::pair<std::uint32_t, std::uint32_t>> imageSizes_;
    rhi::Pipeline pipeline_;
    std::uint32_t fontWidth_=0,fontHeight_=0,width_=0,height_=0;
    std::uint64_t fontRevision_=0;
};
}
