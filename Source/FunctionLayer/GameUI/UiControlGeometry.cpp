#include "UiControlGeometry.h"
#include <algorithm>
#include <cmath>
namespace se
{
float UiValueRatio(double value,double minimum,double maximum)noexcept
{const double range=maximum-minimum;return std::isfinite(value)&&std::isfinite(minimum)&&std::isfinite(maximum)&&std::isfinite(range)&&range>0?static_cast<float>((std::clamp(value,minimum,maximum)-minimum)/range):0;}
UiSliderGeometry MakeUiSliderGeometry(UiRect r,float scale,double value,double minimum,double maximum)noexcept
{
    UiSliderGeometry g;const float width=std::max(0.0f,r.width),height=std::max(0.0f,r.height),thumb=std::min(width,10*scale),th=std::min(height,12*scale),bar=std::min(height,4*scale);
    g.first=r.x+thumb/2;g.last=r.x+width-thumb/2;const float y=r.y+height-th/2;
    g.track={g.first,y-bar/2,std::max(0.0f,g.last-g.first),bar};g.thumb={g.first+(g.last-g.first)*UiValueRatio(value,minimum,maximum)-thumb/2,y-th/2,thumb,th};return g;
}
float UiSliderRatioAt(UiRect r,float scale,float x)noexcept
{const auto g=MakeUiSliderGeometry(r,scale,0,0,1);return g.last>g.first?std::clamp((x-g.first)/(g.last-g.first),0.0f,1.0f):0;}
UiScrollGeometry MakeUiScrollGeometry(UiRect r,float padding,float contentHeight,float scroll,float scale)noexcept
{
    UiScrollGeometry g;const float width=std::min(std::max(0.0f,r.width),12*scale);g.visible=std::max(0.0f,r.height-2*padding);g.maximum=std::max(0.0f,contentHeight-g.visible);g.scrollable=g.maximum>0&&r.height>0&&width>0;
    g.track={r.x+r.width-width,r.y,width,std::max(0.0f,r.height)};const float height=g.track.height*std::clamp(g.visible/std::max(1.0f,std::max(g.visible,contentHeight)),.05f,1.0f);
    g.thumb={g.track.x,g.track.y+(g.track.height-height)*(g.maximum>0?std::clamp(scroll/g.maximum,0.0f,1.0f):0),g.track.width,height};return g;
}
}
