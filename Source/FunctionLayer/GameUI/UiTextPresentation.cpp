#include "UiTextPresentation.h"
#include "CoreLayer/Utf.h"
#include <algorithm>
#include <cmath>
namespace se
{
namespace
{
UiRect Intersect(UiRect a,UiRect b)
{const float x=std::max(a.x,b.x),y=std::max(a.y,b.y);return {x,y,std::max(0.0f,std::min(a.x+a.width,b.x+b.width)-x),std::max(0.0f,std::min(a.y+a.height,b.y+b.height)-y)};}
}
bool PrepareUiText(const UiVisual& visual,UiTextPresentation& output,std::string& error)
{
    error.clear();UiTextPresentation next;next.editing=visual.kind==UiKind::TextInput;next.focused=visual.focused;next.fontSize=visual.fontSize;next.content=visual.rectangle;
    for(const float value:{visual.rectangle.x,visual.rectangle.y,visual.rectangle.width,visual.rectangle.height,visual.clip.x,visual.clip.y,visual.clip.width,visual.clip.height})
        if(!std::isfinite(value)){error="Font.InvalidText";return false;}
    if(visual.rectangle.width<0||visual.rectangle.height<0||visual.clip.width<0||visual.clip.height<0){error="Font.InvalidText";return false;}
    if(!std::isfinite(next.fontSize)||next.fontSize<=0){error="Font.InvalidText";return false;}
    next.displayText=visual.text;
    if(next.editing)
    {
        const float padding=next.fontSize*4/18;
        const float px=std::min(padding,std::max(0.0f,(next.content.width-1)/2)),py=std::min(padding,std::max(0.0f,(next.content.height-1)/2));
        next.content={next.content.x+px,next.content.y+py,std::max(1.0f,next.content.width-2*px),std::max(1.0f,next.content.height-2*py)};
        std::u32string committed,composition;if(!DecodeUtf8(visual.text,committed)||!DecodeUtf8(visual.composition,composition)){error="Font.InvalidText";return false;}
        next.caret=std::min(visual.caret,committed.size());next.selectionFirst=std::min(next.caret,std::min(visual.selectionAnchor,committed.size()));next.selectionLast=std::max(next.caret,std::min(visual.selectionAnchor,committed.size()));
        next.composing=next.focused&&!composition.empty();
        if(next.composing)
        {
            committed.replace(next.selectionFirst,next.selectionLast-next.selectionFirst,composition);next.compositionFirst=next.selectionFirst;next.compositionLast=next.selectionFirst+composition.size();
            next.caret=next.compositionFirst+std::min(visual.compositionCaret,composition.size());next.selectionFirst=next.selectionLast=next.caret;
            if(!EncodeUtf8(committed,next.displayText)){error="Font.InvalidText";return false;}
        }
    }
    if(next.displayText.size()>65536){error="Font.InvalidText";return false;}
    next.clip=Intersect(visual.clip,next.content);output=std::move(next);return true;
}
UiTextView PlaceUiText(const UiTextPresentation& text,const TextLayout& layout,Vec2 scale)
{
    if(!std::isfinite(scale.x)||!std::isfinite(scale.y)||scale.x<=0||scale.y<=0)return {};
    UiTextView view;view.origin={text.content.x,text.content.y};view.clip=text.clip;view.scale=scale;
    const auto cursor=layout.Caret(text.caret);const float cw=std::max(1.0f/scale.x,text.fontSize/18);
    if(text.editing&&text.focused)
    {
        view.origin.x-=std::max(0.0f,cursor.x/scale.x+cw-text.content.width);
        view.origin.y-=std::max(0.0f,(cursor.y+cursor.height)/scale.y-text.content.height);
    }
    view.caret={view.origin.x+cursor.x/scale.x,view.origin.y+cursor.y/scale.y,cw,cursor.height/scale.y};return view;
}
std::size_t UiTextView::HitTest(const TextLayout& layout,UiPoint point)const
{return layout.HitTest((point.x-origin.x)*scale.x,(point.y-origin.y)*scale.y);}
std::vector<UiRect> UiTextView::Selection(const TextLayout& layout,std::size_t first,std::size_t last)const
{
    auto result=layout.Selection(first,last);for(auto& r:result)r={origin.x+r.x/scale.x,origin.y+r.y/scale.y,r.width/scale.x,r.height/scale.y};return result;
}
}
