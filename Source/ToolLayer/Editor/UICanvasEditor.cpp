#include "UICanvasEditor.h"
#include <algorithm>
#include <cmath>

namespace se
{
namespace
{
bool Finite(UiPoint p){return std::isfinite(p.x)&&std::isfinite(p.y);}
bool Same(UiRect a,UiRect b){return a.x==b.x&&a.y==b.y&&a.width==b.width&&a.height==b.height;}
}
bool UICanvasEditor::SetPreview(UiPoint size)
{
    if(!Finite(size)||size.x<64||size.y<64||size.x>8192||size.y>8192)return Fail("EditorUi.CanvasSizeInvalid");
    preview_=size;error_.clear();return true;
}
bool UICanvasEditor::Prepare(UiTree& ui,UiRect viewport)
{
    if(!Finite({viewport.x,viewport.y})||!Finite({viewport.width,viewport.height})||viewport.width<1||viewport.height<1||viewport.width>8192||viewport.height>8192)return Fail("EditorUi.CanvasSizeInvalid");
    viewport_=viewport;scale_=std::min(viewport.width/preview_.x,viewport.height/preview_.y);
    content_={viewport.x+(viewport.width-preview_.x*scale_)/2,viewport.y+(viewport.height-preview_.y*scale_)/2,preview_.x*scale_,preview_.y*scale_};
    ui.Layout(preview_.x,preview_.y);error_.clear();return true;
}
UiPoint UICanvasEditor::ToScreen(UiPoint p)const noexcept{return {content_.x+p.x*scale_,content_.y+p.y*scale_};}
UiPoint UICanvasEditor::ToPreview(UiPoint p)const noexcept{return {(p.x-content_.x)/scale_,(p.y-content_.y)/scale_};}
UiRect UICanvasEditor::ToScreen(UiRect r)const noexcept
{const auto p=ToScreen(UiPoint{r.x,r.y});return {p.x,p.y,r.width*scale_,r.height*scale_};}
UiId UICanvasEditor::Hit(const UiTree& ui,UiPoint screen)const
{
    if(!Finite(screen)||!content_.Contains(screen))return 0;
    const auto point=ToPreview(screen);const auto visuals=ui.Visuals();
    for(auto i=visuals.rbegin();i!=visuals.rend();++i)if(i->rectangle.Contains(point)&&i->clip.Contains(point))return i->id;
    return 0;
}
std::vector<UiVisual> UICanvasEditor::Visuals(const UiTree& ui)const
{
    auto result=ui.Visuals();
    for(auto& visual:result)
    {
        for(auto* rectangle:{&visual.rectangle,&visual.clip})
        {const auto screen=ToScreen(*rectangle);*rectangle={screen.x-viewport_.x,screen.y-viewport_.y,screen.width,screen.height};}
        visual.fontSize*=scale_;visual.padding*=scale_;visual.scroll*=scale_;visual.contentHeight*=scale_;
    }
    return result;
}
std::optional<UiCanvasAnchors> UICanvasEditor::Anchors(const UiTree& ui,UiId id)const
{
    const auto node=ui.Get(id);if(!node)return {};
    for(auto p=node;p;p=ui.Get(p->parent))if(!p->visible)return {};
    const float s=ui.CanvasScale();UiRect parent{0,0,preview_.x,preview_.y};
    if(const auto p=ui.Get(node->parent))
    {
        const float pad=p->style.padding*s;parent={p->rectangle.x+pad,p->rectangle.y+pad,std::max(0.0f,p->rectangle.width-2*pad),std::max(0.0f,p->rectangle.height-2*pad)};
        if(p->style.layout!=UiLayout::Absolute)for(const auto sibling:p->children)
        {
            if(sibling==id)break;const auto prior=ui.Get(sibling);if(!prior||!prior->visible)continue;
            if(p->style.layout==UiLayout::Horizontal)parent.x+=prior->rectangle.width+p->style.spacing*s;
            else parent.y+=prior->rectangle.height+p->style.spacing*s;
        }
        if(p->kind==UiKind::ScrollView)parent.y-=p->scroll*s;
    }
    const auto& style=node->style;
    return UiCanvasAnchors{ToScreen(node->rectangle),ToScreen(UiPoint{parent.x+parent.width*style.anchorMin.x,parent.y+parent.height*style.anchorMin.y}),
        ToScreen(UiPoint{parent.x+parent.width*style.anchorMax.x,parent.y+parent.height*style.anchorMax.y}),
        ToScreen(UiPoint{node->rectangle.x+style.pivot.x*style.size.x*s,node->rectangle.y+style.pivot.y*style.size.y*s})};
}
bool UICanvasEditor::Begin(UiTree& ui,UiId id,UiCanvasDrag kind,UiPoint screen)
{
    const auto node=ui.Get(id);if(Active()||!node||!Finite(screen)||!content_.Contains(screen))return Fail("EditorUi.CanvasGestureInvalid");
    if(kind!=UiCanvasDrag::Move&&kind!=UiCanvasDrag::Resize)return Fail("EditorUi.CanvasGestureInvalid");
    if(const auto parent=ui.Get(node->parent);kind==UiCanvasDrag::Move&&parent&&parent->style.layout!=UiLayout::Absolute)return Fail("EditorUi.CanvasFlowPosition");
    dragId_=id;dragKind_=kind;dragStart_=ToPreview(screen);initialStyle_=node->style;dragCanvasScale_=ui.CanvasScale();
    initialSize_={node->rectangle.width/dragCanvasScale_,node->rectangle.height/dragCanvasScale_};dragViewport_=viewport_;dragPreview_=preview_;error_.clear();return true;
}
bool UICanvasEditor::Update(UiTree& ui,UiPoint screen)
{
    if(!Active()||!ui.Get(dragId_)||!Finite(screen))return Fail("EditorUi.CanvasGestureInvalid");
    if(!Same(dragViewport_,viewport_)||dragPreview_!=preview_||dragCanvasScale_!=ui.CanvasScale())return Fail("EditorUi.CanvasProjectionChanged");
    const auto point=ToPreview(screen);UiPoint delta{(point.x-dragStart_.x)/dragCanvasScale_,(point.y-dragStart_.y)/dragCanvasScale_};auto style=initialStyle_;
    if(dragKind_==UiCanvasDrag::Move)style.offset={style.offset.x+delta.x,style.offset.y+delta.y};
    else
    {
        delta.x=std::max(delta.x,1-initialSize_.x);delta.y=std::max(delta.y,1-initialSize_.y);
        style.size={style.size.x+delta.x,style.size.y+delta.y};style.offset={style.offset.x+style.pivot.x*delta.x,style.offset.y+style.pivot.y*delta.y};
    }
    if(!ui.SetStyle(dragId_,style))return Fail("EditorUi.CanvasGestureInvalid");ui.Layout(preview_.x,preview_.y);error_.clear();return true;
}
bool UICanvasEditor::End(UiTree& ui,bool commit)
{
    if(!Active())return false;const auto id=dragId_;dragId_=0;const bool ok=ui.Get(id)&&(commit||ui.SetStyle(id,initialStyle_));
    ui.Layout(preview_.x,preview_.y);return ok;
}
}
