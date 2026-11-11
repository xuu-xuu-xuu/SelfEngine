#pragma once
#include "PlatformLayer/Graphics/Rhi.h"
#include "ResourceLayer/Fonts/FontCache.h"
#include "ResourceLayer/Assets/AssetStore.h"
#include <unordered_map>
#include <unordered_set>

namespace se
{
struct FrameData;
struct GameUiRenderStatistics
{
    std::size_t quads=0,batches=0,fontPages=0,layoutEntries=0;
    std::uint64_t layoutMisses=0,atlasUploads=0,imageUploads=0;
};
// 消费 UiVisual 值，不借用树或编辑器。CPU 准备也可独立验证；GPU 所有者只在 Configure 创建。
class GameUIRenderer
{
public:
    bool Prepare(std::span<const UiVisual> visuals,unsigned width,unsigned height,Vec2 framebufferScale,
        AssetStore* assets,std::vector<AssetDiagnostic>& diagnostics);
    // overlays 是场景 UI 之外的调试/运行时覆盖层（如 G06.09 的遮挡 HUD），
    // 按传入顺序追加在 frame.ui 之后绘制；不复制或修改 FrameData。
    rhi::Result Configure(rhi::Device& device,rhi::CommandContext& commands,const FrameData& frame,
        unsigned width,unsigned height,const std::filesystem::path& shaders,std::vector<AssetDiagnostic>& diagnostics,
        std::span<const UiVisual> overlays={});
    const GuiGeometry& Geometry()const{return geometry_;}
    GameUiRenderStatistics Statistics()const{return statistics_;}
    void Clear()noexcept;
private:
    enum class TextureKind {White,Missing,Font,Image};
    struct Binding {TextureKind kind;unsigned page=0;std::string image;};
    struct LayoutEntry {TextLayout layout;std::uint64_t used=0;};
    struct FontTexture {std::uint64_t revision=0;rhi::Texture texture;};
    struct ImageTexture {std::shared_ptr<const ImageData> source;rhi::Texture texture;};
    bool Quad(UiRect rectangle,UiRect clip,UiRect uv,UiColor color,std::uint64_t texture,bool data);
    bool Append(std::array<GuiVertex,4> vertices,UiRect clip,std::uint64_t texture,bool data);
    bool Line(UiPoint first,UiPoint last,float width,UiRect clip,UiColor color,std::uint64_t texture);
    std::uint64_t Bind(std::string key,Binding binding);
    std::unique_ptr<FontCache> fonts_;
    std::unordered_map<std::string,LayoutEntry> layouts_;
    std::vector<FontTexture> pages_;
    std::unordered_map<std::string,ImageTexture> images_;
    std::unordered_map<std::string,std::uint64_t> tokens_;
    std::vector<Binding> bindings_;
    GuiGeometry geometry_;
    rhi::Texture white_,missing_;
    rhi::Pipeline pipeline_;
    GameUiRenderStatistics statistics_;
    std::uint64_t frame_=0;
};
}
