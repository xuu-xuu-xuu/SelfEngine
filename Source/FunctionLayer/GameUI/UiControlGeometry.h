#pragma once
#include "UiTree.h"
namespace se
{
float UiValueRatio(double value,double minimum,double maximum)noexcept;
struct UiSliderGeometry {UiRect track,thumb;float first=0,last=0;};
UiSliderGeometry MakeUiSliderGeometry(UiRect rectangle,float scale,double value,double minimum,double maximum)noexcept;
float UiSliderRatioAt(UiRect rectangle,float scale,float x)noexcept;
struct UiScrollGeometry {UiRect track,thumb;float visible=0,maximum=0;bool scrollable=false;};
UiScrollGeometry MakeUiScrollGeometry(UiRect rectangle,float padding,float contentHeight,float scroll,float scale)noexcept;
}
