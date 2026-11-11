#include "GameUIRenderer.h"
#include "FunctionLayer/Application/FrameData.h"
#include "FunctionLayer/GameUI/UiTextPresentation.h"
#include "FunctionLayer/GameUI/UiControlGeometry.h"
#include "FunctionLayer/GameUI/UiEventSystem.h"
#include <algorithm>
#include <cmath>

namespace se
{
namespace
{
float Srgb(float x){x=std::clamp(x,0.0f,1.0f);return x<=.0031308f?12.92f*x:1.055f*std::pow(x,1/2.4f)-.055f;}
std::uint32_t Color(UiColor c)
{
    const auto byte=[](float x){return std::uint32_t(std::lround(std::clamp(x,0.0f,1.0f)*255));};
    return byte(Srgb(c.r))|(byte(Srgb(c.g))<<8)|(byte(Srgb(c.b))<<16)|(byte(c.a)<<24);
}
bool Finite(UiRect r){return IsFinite(r.x)&&IsFinite(r.y)&&IsFinite(r.width)&&IsFinite(r.height)&&IsFinite(r.x+r.width)&&IsFinite(r.y+r.height)&&r.width>=0&&r.height>=0;}
UiRect Intersect(UiRect a,UiRect b)
{
    const float x=std::max(a.x,b.x),y=std::max(a.y,b.y);
    return {x,y,std::max(0.0f,std::min(a.x+a.width,b.x+b.width)-x),std::max(0.0f,std::min(a.y+a.height,b.y+b.height)-y)};
}
bool Same(Vec4 a,Vec4 b){return a.x==b.x&&a.y==b.y&&a.z==b.z&&a.w==b.w;}
}
std::uint64_t GameUIRenderer::Bind(std::string key,Binding binding)
{
    if(auto found=tokens_.find(key);found!=tokens_.end())return found->second;
    const auto token=static_cast<std::uint64_t>(bindings_.size());bindings_.push_back(std::move(binding));tokens_.emplace(std::move(key),token);return token;
}
bool GameUIRenderer::Quad(UiRect r,UiRect clip,UiRect uv,UiColor color,std::uint64_t texture,bool data)
{
    if(color.a<=0||r.width<=0||r.height<=0)return true;const auto packed=Color(color);
    return Append({GuiVertex{{r.x,r.y},{uv.x,uv.y},packed},GuiVertex{{r.x+r.width,r.y},{uv.x+uv.width,uv.y},packed},
        GuiVertex{{r.x+r.width,r.y+r.height},{uv.x+uv.width,uv.y+uv.height},packed},GuiVertex{{r.x,r.y+r.height},{uv.x,uv.y+uv.height},packed}},clip,texture,data);
}
bool GameUIRenderer::Line(UiPoint first,UiPoint last,float width,UiRect clip,UiColor color,std::uint64_t texture)
{
    const float dx=last.x-first.x,dy=last.y-first.y,length=std::hypot(dx,dy);if(length<=0||color.a<=0)return true;
    const float x=-dy/length*width/2,y=dx/length*width/2;const auto packed=Color(color);
    return Append({GuiVertex{{first.x+x,first.y+y},{},packed},GuiVertex{{last.x+x,last.y+y},{},packed},
        GuiVertex{{last.x-x,last.y-y},{},packed},GuiVertex{{first.x-x,first.y-y},{},packed}},clip,texture,true);
}
bool GameUIRenderer::Append(std::array<GuiVertex,4> vertices,UiRect clip,std::uint64_t texture,bool data)
{
    float x=vertices[0].position.x,y=vertices[0].position.y,right=x,bottom=y;for(const auto& v:vertices){x=std::min(x,v.position.x);y=std::min(y,v.position.y);right=std::max(right,v.position.x);bottom=std::max(bottom,v.position.y);}
    const UiRect r{x,y,right-x,bottom-y};
    clip=Intersect(clip,{0,0,geometry_.displaySize.x,geometry_.displaySize.y});
    if(r.width<=0||r.height<=0||Intersect(r,clip).width<=0||Intersect(r,clip).height<=0)return true;
    if(geometry_.vertices.size()/4>=65536)return false;
    const auto vertex=static_cast<std::uint32_t>(geometry_.vertices.size()),index=static_cast<std::uint32_t>(geometry_.indices.size());
    const Vec4 scissor{clip.x,clip.y,clip.x+clip.width,clip.y+clip.height};
    geometry_.vertices.insert(geometry_.vertices.end(),vertices.begin(),vertices.end());
    geometry_.indices.insert(geometry_.indices.end(),{vertex,vertex+1,vertex+2,vertex,vertex+2,vertex+3});
    if(!geometry_.commands.empty()&&geometry_.commands.back().texture==texture&&geometry_.commands.back().textureIsData==data&&Same(geometry_.commands.back().clip,scissor))geometry_.commands.back().indexCount+=6;
    else geometry_.commands.push_back({scissor,texture,index,6,0,data});
    return true;
}
bool GameUIRenderer::Prepare(std::span<const UiVisual> visuals,unsigned width,unsigned height,Vec2 scale,AssetStore* assets,std::vector<AssetDiagnostic>& diagnostics)
{
    geometry_={};bindings_.clear();tokens_.clear();statistics_.quads=statistics_.batches=0;++frame_;
    const auto fail=[&](const char* key){diagnostics.push_back({key,{}});geometry_={};bindings_.clear();return false;};
    if(!width||!height||width>8192||height>8192||!IsFinite(scale.x)||!IsFinite(scale.y)||scale.x<=0||scale.y<=0)return fail("UI.RenderInvalid");
    geometry_.displaySize={width/scale.x,height/scale.y};geometry_.framebufferScale=scale;
    if(!IsFinite(geometry_.displaySize.x)||!IsFinite(geometry_.displaySize.y))return fail("UI.RenderInvalid");
    std::unordered_set<std::string> usedImages;
    std::uint64_t imageBytes=0;
    const auto white=Bind("white",{TextureKind::White});
    struct PendingScroll{UiScrollGeometry geometry;UiRect clip;UiColor color;float scale;std::uint32_t depth;bool active;};
    std::vector<PendingScroll> scrollbars;
    const auto scrollbar=[&](const PendingScroll& p){auto thumb=p.geometry.thumb;thumb.x+=2*p.scale;thumb.width=std::max(0.0f,thumb.width-4*p.scale);
        return Quad(p.geometry.track,p.clip,{0,0,1,1},{.015f,.025f,.04f,p.color.a},white,true)
            &&Quad(thumb,p.clip,{0,0,1,1},{p.active?.25f:.12f,p.active?.55f:.3f,p.active?.85f:.5f,p.color.a},white,true);};
    for(const auto& visual:visuals)
    {
        while(!scrollbars.empty()&&visual.depth<=scrollbars.back().depth){if(!scrollbar(scrollbars.back()))return fail("UI.RenderCapacity");scrollbars.pop_back();}
        if(!Finite(visual.rectangle)||!Finite(visual.clip)||!IsFinite(visual.opacity)||!IsFinite(visual.color.r)||!IsFinite(visual.color.g)||!IsFinite(visual.color.b)||!IsFinite(visual.color.a))return fail("UI.RenderInvalid");
        if(visual.depth>128||!std::isfinite(visual.value)||!std::isfinite(visual.minimum)||!std::isfinite(visual.maximum)||visual.maximum<visual.minimum||!std::isfinite(visual.maximum-visual.minimum)
            ||!std::isfinite(visual.padding)||visual.padding<0||!std::isfinite(visual.fontSize)||visual.fontSize<=0)return fail("UI.RenderInvalid");
        auto color=visual.color;color.a*=std::clamp(visual.opacity,0.0f,1.0f);
        if(UiEventSystem::Interactive(visual.kind)&&!visual.enabled)color.a*=.45f;
        if(color.a<=0||Intersect(visual.rectangle,visual.clip).width<=0||Intersect(visual.rectangle,visual.clip).height<=0)continue;
        if(visual.kind==UiKind::Image)
        {
            std::uint64_t token;
            auto source=assets&&!visual.imageAsset.empty()?assets->LoadTextureImage(visual.imageAsset,diagnostics):nullptr;
            if(source)
            {
                if(usedImages.insert(visual.imageAsset).second)
                {imageBytes+=source->rgba8.size();if(usedImages.size()>256||imageBytes>128ull*1024*1024)return fail("UI.RenderCapacity");}
                auto& cached=images_[visual.imageAsset];if(cached.source!=source){cached.texture.reset();cached.source=std::move(source);}
                token=Bind("image:"+visual.imageAsset,{TextureKind::Image,0,visual.imageAsset});
            }
            else token=Bind("missing",{TextureKind::Missing});
            if(!Quad(visual.rectangle,visual.clip,{0,0,1,1},color,token,false))return fail("UI.RenderCapacity");
        }
        else if(visual.kind!=UiKind::Label)
        {
            const auto r=visual.rectangle;const float s=visual.fontSize/18,state=visual.pressed?.75f:visual.hovered?1.3f:1;
            const UiColor background{color.r*.08f*state,color.g*.13f*state,color.b*.2f*state,color.a},accent{color.r*.12f*state,color.g*.5f*state,color.b*.85f*state,color.a};
            const auto clip=visual.kind==UiKind::Panel?visual.clip:Intersect(visual.clip,r);const auto rectangle=[&](UiRect area,UiColor tint){return Quad(area,clip,{0,0,1,1},tint,white,true);};bool valid=true;
            if(visual.kind==UiKind::Panel||visual.kind==UiKind::ScrollView)valid=rectangle(r,color);
            else if(visual.kind==UiKind::Checkbox)
            {
                const float size=std::min({22*s,r.width,r.height});const UiRect box{r.x,r.y+(r.height-size)/2,size,size};valid=rectangle(box,background);
                if(visual.value!=0)valid=valid&&rectangle({box.x+2*s,box.y+2*s,std::max(0.0f,size-4*s),std::max(0.0f,size-4*s)},accent)
                    &&Line({box.x+size*.2f,box.y+size*.5f},{box.x+size*.43f,box.y+size*.73f},2*s,clip,color,white)
                    &&Line({box.x+size*.43f,box.y+size*.73f},{box.x+size*.82f,box.y+size*.25f},2*s,clip,color,white);
            }
            else if(visual.kind==UiKind::Slider)
            {
                const auto g=MakeUiSliderGeometry(r,s,visual.value,visual.minimum,visual.maximum);auto filled=g.track;filled.width*=UiValueRatio(visual.value,visual.minimum,visual.maximum);
                valid=rectangle(g.track,background)&&rectangle(filled,accent)&&rectangle(g.thumb,{color.r*.65f,color.g*.82f,color.b,color.a});
            }
            else if(visual.kind==UiKind::ProgressBar)
            {auto filled=r;filled.width*=UiValueRatio(visual.value,visual.minimum,visual.maximum);valid=rectangle(r,background)&&rectangle(filled,accent);}
            else if(visual.kind==UiKind::TextInput)valid=rectangle(r,{color.r*.12f,color.g*.12f,color.b*.12f,color.a});
            else valid=rectangle(r,background);
            if(visual.kind==UiKind::ScrollView)
            {const auto g=MakeUiScrollGeometry(r,visual.padding,visual.contentHeight,visual.scroll,s);if(g.scrollable)scrollbars.push_back({g,clip,color,s,visual.depth,visual.hovered||visual.pressed});}
            if(visual.focused&&visual.enabled&&UiEventSystem::Interactive(visual.kind))
                valid=valid&&rectangle({r.x,r.y,r.width,s},accent)&&rectangle({r.x,r.y+r.height-s,r.width,s},accent)
                    &&rectangle({r.x,r.y,s,r.height},accent)&&rectangle({r.x+r.width-s,r.y,s,r.height},accent);
            if(!valid)return fail("UI.RenderCapacity");
        }
        if(visual.kind==UiKind::ScrollView||(visual.text.empty()&&visual.kind!=UiKind::TextInput)||visual.rectangle.width<=0)continue;
        auto label=visual;
        if(label.kind==UiKind::Checkbox){const float shift=std::min(22*visual.fontSize/18,visual.rectangle.height)+8*visual.fontSize/18;label.rectangle.x+=shift;label.rectangle.width=std::max(0.0f,label.rectangle.width-shift);}
        if(label.kind==UiKind::Slider){label.fontSize*=.8f;label.rectangle.height=std::max(0.0f,label.rectangle.height-12*visual.fontSize/18);}
        if(label.rectangle.width<=0||label.rectangle.height<=0)continue;
        UiTextPresentation text;std::string textError;if(!PrepareUiText(label,text,textError))return fail(textError.c_str());
        if(!fonts_)fonts_=std::make_unique<FontCache>();
        const float fontSize=text.fontSize*scale.y,maxWidth=text.content.width*scale.x;
        if(maxWidth<1.0f||fontSize<1.0f)continue;
        std::string key=text.displayText;key.push_back('\0');key.append(reinterpret_cast<const char*>(&fontSize),sizeof(fontSize));key.append(reinterpret_cast<const char*>(&maxWidth),sizeof(maxWidth));
        auto found=layouts_.find(key);
        if(found==layouts_.end())
        {
            TextLayout layout;std::string error;
            if(!fonts_->Layout(text.displayText,fontSize,maxWidth,layout,error))return fail(error.c_str());
            if(layouts_.size()>=256)
            {auto oldest=std::min_element(layouts_.begin(),layouts_.end(),[](const auto& a,const auto& b){return a.second.used<b.second.used;});layouts_.erase(oldest);}
            found=layouts_.emplace(std::move(key),LayoutEntry{std::move(layout),frame_}).first;++statistics_.layoutMisses;
        }
        found->second.used=frame_;const auto& layout=found->second.layout;auto view=PlaceUiText(text,layout,scale);
        if(visual.kind==UiKind::Button||visual.kind==UiKind::ProgressBar)view.origin.x+=std::max(0.0f,(text.content.width-layout.width/scale.x)/2);
        if(visual.kind==UiKind::Button||visual.kind==UiKind::ProgressBar||visual.kind==UiKind::Checkbox)view.origin.y+=std::max(0.0f,(text.content.height-layout.height/scale.y)/2);
        if(text.editing&&text.focused)for(const auto& r:view.Selection(layout,text.selectionFirst,text.selectionLast))
            if(!Quad(r,view.clip,{0,0,1,1},{.03f,.18f,.4f,.8f*color.a},white,true))return fail("UI.RenderCapacity");
        for(const auto& quad:layout.quads)
        {
            const auto& page=fonts_->Pages().at(quad.page);const auto& d=quad.destination;const auto& uv=quad.pixels;
            const auto token=Bind("font:"+std::to_string(quad.page),{TextureKind::Font,quad.page});
            if(!Quad({view.origin.x+d.x/scale.x,view.origin.y+d.y/scale.y,d.width/scale.x,d.height/scale.y},view.clip,
                {uv.x/page.width,uv.y/page.height,uv.width/page.width,uv.height/page.height},color,token,true))return fail("UI.RenderCapacity");
        }
        if(text.composing)for(const auto& r:view.Selection(layout,text.compositionFirst,text.compositionLast))
            if(!Quad({r.x,r.y+r.height-std::max(1.0f/scale.y,text.fontSize/18),r.width,std::max(1.0f/scale.y,text.fontSize/18)},view.clip,{0,0,1,1},color,white,true))return fail("UI.RenderCapacity");
        if(text.editing&&text.focused&&visual.enabled)
            if(!Quad(view.caret,view.clip,{0,0,1,1},color,white,true))return fail("UI.RenderCapacity");
    }
    while(!scrollbars.empty()){if(!scrollbar(scrollbars.back()))return fail("UI.RenderCapacity");scrollbars.pop_back();}
    std::erase_if(images_,[&](const auto& item){return !usedImages.contains(item.first);});
    statistics_.quads=geometry_.vertices.size()/4;statistics_.batches=geometry_.commands.size();statistics_.fontPages=fonts_?fonts_->Pages().size():0;statistics_.layoutEntries=layouts_.size();return true;
}
rhi::Result GameUIRenderer::Configure(rhi::Device& device,rhi::CommandContext& commands,const FrameData& f,unsigned width,unsigned height,const std::filesystem::path& shaders,std::vector<AssetDiagnostic>& diagnostics,std::span<const UiVisual> overlays)
{
    if(f.ui.empty()&&overlays.empty())
    {geometry_={};bindings_.clear();tokens_.clear();images_.clear();statistics_.quads=statistics_.batches=0;return {};}
    Vec2 scale{1,1};
    if(f.editorGuiEnabled)
    {
        scale=f.editorGui.framebufferScale;
        if(!IsFinite(f.sceneViewport.width)||!IsFinite(f.sceneViewport.height)||!IsFinite(scale.x)||!IsFinite(scale.y)||scale.x<=0||scale.y<=0
            ||f.sceneViewport.width<1||f.sceneViewport.height<1||f.sceneViewport.width*scale.x>8192||f.sceneViewport.height*scale.y>8192)return {rhi::Error::InvalidArgument};
        width=static_cast<unsigned>(std::lround(f.sceneViewport.width*scale.x));height=static_cast<unsigned>(std::lround(f.sceneViewport.height*scale.y));
    }
    // 只在确有覆盖层时复制；普通场景 UI 路径保持零额外分配。
    std::vector<UiVisual> composed;
    std::span<const UiVisual> visuals=f.ui;
    if(!overlays.empty())
    {
        composed.assign(f.ui.begin(),f.ui.end());
        composed.insert(composed.end(),overlays.begin(),overlays.end());
        visuals=composed;
    }
    if(!Prepare(visuals,width,height,scale,f.assets,diagnostics))return {rhi::Error::InvalidArgument};
    if(geometry_.commands.empty())return {};
    if(!pipeline_)
    {
        rhi::PipelineDescription description{rhi::PipelineKind::Gui,shaders/"Gui.hlsl",rhi::GuiLayoutVersion};description.colorFormat=rhi::ColorFormat::Rgba8Srgb;description.blend=rhi::BlendMode::Alpha;
        if(auto result=device.CreatePipeline(description,pipeline_);!result)return result;
    }
    if(!white_)
    {constexpr unsigned char pixels[]{255,255,255,255};if(auto result=device.CreateTexture({1,1,1,1,rhi::TextureFormat::Rgba8,"GameUiWhite"},pixels,white_);!result)return result;}
    if(fonts_)
    {
        pages_.resize(fonts_->Pages().size());
        for(size_t i=0;i<pages_.size();++i)
        {
            const auto& source=fonts_->Pages()[i];auto& destination=pages_[i];if(destination.texture&&destination.revision==source.revision)continue;
            std::vector<unsigned char> rgba(source.alpha.size()*4,255);for(size_t pixel=0;pixel<source.alpha.size();++pixel)rgba[pixel*4+3]=source.alpha[pixel];
            rhi::Texture texture;if(auto result=device.CreateTexture({source.width,source.height,1,1,rhi::TextureFormat::Rgba8,"GameUiFont"},rgba,texture);!result)return result;
            destination.texture=std::move(texture);destination.revision=source.revision;++statistics_.atlasUploads;
        }
    }
    std::vector<rhi::TextureHandle> textures;textures.reserve(geometry_.commands.size());
    for(const auto& command:geometry_.commands)
    {
        const auto& binding=bindings_.at(static_cast<size_t>(command.texture));
        if(binding.kind==TextureKind::White)textures.push_back(*white_);
        else if(binding.kind==TextureKind::Font)textures.push_back(*pages_.at(binding.page).texture);
        else if(binding.kind==TextureKind::Missing)
        {
            if(!missing_){constexpr unsigned char pixels[]{255,0,255,255,32,32,32,255,32,32,32,255,255,0,255,255};if(auto result=device.CreateTexture({2,2,1,1,rhi::TextureFormat::Rgba8,"GameUiMissing"},pixels,missing_);!result)return result;}
            textures.push_back(*missing_);
        }
        else
        {
            auto& image=images_.at(binding.image);
            if(!image.texture)
            {if(auto result=device.CreateTexture({image.source->width,image.source->height,1,1,rhi::TextureFormat::Rgba8,"GameUiImage"},image.source->rgba8,image.texture);!result)return result;++statistics_.imageUploads;}
            textures.push_back(*image.texture);
        }
    }
    return commands.ConfigureGameUi(*pipeline_,geometry_,textures);
}
void GameUIRenderer::Clear()noexcept
{
    geometry_={};bindings_.clear();tokens_.clear();layouts_.clear();fonts_.reset();pages_.clear();images_.clear();white_.reset();missing_.reset();pipeline_.reset();statistics_={};frame_=0;
}
}
