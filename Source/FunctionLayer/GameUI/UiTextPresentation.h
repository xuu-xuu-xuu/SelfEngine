#pragma once
#include "ResourceLayer/Fonts/FontCache.h"
#include "CoreLayer/Math/Projection.h"
namespace se
{
struct UiTextPresentation
{
    std::string displayText;
    UiRect content,clip;
    float fontSize=18;
    std::size_t caret=0,selectionFirst=0,selectionLast=0,compositionFirst=0,compositionLast=0;
    bool editing=false,focused=false,composing=false;
};
struct UiTextView
{
    UiPoint origin;
    UiRect clip,caret;
    Vec2 scale{1,1};
    std::size_t HitTest(const TextLayout& layout,UiPoint point)const;
    std::vector<UiRect> Selection(const TextLayout& layout,std::size_t first,std::size_t last)const;
};
bool PrepareUiText(const UiVisual& visual,UiTextPresentation& output,std::string& error);
UiTextView PlaceUiText(const UiTextPresentation& text,const TextLayout& layout,Vec2 rasterScale={1,1});
}
