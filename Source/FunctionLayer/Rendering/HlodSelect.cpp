#include "HlodSelect.h"
#include <algorithm>
#include <cmath>

namespace se
{
float ProjectedHlodError(const HlodCell& cell,const HlodView& view)
{
    const auto distance=(std::max)(Length(view.position-cell.center),view.nearPlane);
    const auto tangent=(std::max)(std::tan(view.verticalFov*0.5f),1e-4f);
    return cell.worldError*(view.viewportHeight*0.5f)/(tangent*distance);
}
bool SelectHlodProxy(const HlodCell& cell,const HlodView& view,float thresholdPixels,bool currentlyProxy)
{
    if(!(cell.worldError>=0)||!(thresholdPixels>0))return false;
    const auto error=ProjectedHlodError(cell,view);
    if(currentlyProxy)return error<=thresholdPixels*1.25f;
    return error*1.25f<=thresholdPixels;
}
}
