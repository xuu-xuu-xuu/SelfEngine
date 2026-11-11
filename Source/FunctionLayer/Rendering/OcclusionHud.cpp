#include "OcclusionHud.h"
#include "CoreLayer/Localization.h"
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <string>

namespace se
{
namespace
{
constexpr UiId FirstVisualId=0x0cc1a00000000001ull;
constexpr std::uint32_t HudDepth=127;
constexpr float PanelWidth=304.0f,PanelHeight=92.0f,PanelMargin=8.0f;
UiVisual Visual(UiId id,UiKind kind,UiRect rectangle,UiRect clip,UiColor color)
{
    UiVisual visual{};
    visual.id=id;visual.kind=kind;visual.rectangle=rectangle;visual.clip=clip;
    visual.color=color;visual.opacity=1;visual.enabled=true;visual.fontSize=14;
    visual.minimum=0;visual.maximum=1;visual.depth=HudDepth;
    return visual;
}
std::string RateText(const Localization& text,const rhi::OcclusionCounters& counters)
{
    char percent[32]{};
    std::snprintf(percent,sizeof(percent),"%.1f%%",double(counters.occlusionRate)*100.0);
    return text.GetText("Rendering.OcclusionHudRate")+" "+percent;
}
}
std::vector<UiVisual> BuildOcclusionHud(const rhi::OcclusionCounters& counters,const Localization& text,
    std::uint32_t surfaceWidth,std::uint32_t surfaceHeight)
{
    std::vector<UiVisual> output;
    if(surfaceWidth<240||surfaceHeight<120)return output;
    const float width=(std::min)(PanelWidth,float(surfaceWidth)-2*PanelMargin);
    if(width<200)return output;
    const UiRect screen{0,0,float(surfaceWidth),float(surfaceHeight)};
    const UiRect panel{PanelMargin,PanelMargin,width,PanelHeight};
    const UiRect content{panel.x+10,panel.y,panel.width-20,panel.height};
    auto append=[&](UiKind kind,UiRect rectangle,UiRect clip,UiColor color,std::string value="")
    {
        auto visual=Visual(FirstVisualId+output.size(),kind,rectangle,clip,color);
        visual.text=std::move(value);
        output.push_back(std::move(visual));
    };
    append(UiKind::Panel,panel,screen,{.02f,.03f,.05f,.78f});
    const auto title=text.GetText("Rendering.OcclusionHudTitle")
        +" "+std::to_string(counters.frame);
    append(UiKind::Label,{content.x,panel.y+8,content.width,18},panel,{.72f,.84f,1,1},title);
    if(!counters.valid)
    {
        append(UiKind::Label,{content.x,panel.y+32,content.width,18},panel,{.78f,.82f,.9f,1},
            text.GetText("Rendering.OcclusionHudPending"));
        return output;
    }
    std::string counts=text.GetText("Rendering.OcclusionHudCandidates")+" "+std::to_string(counters.candidates)
        +"  "+text.GetText("Rendering.OcclusionHudVisible")+" "+std::to_string(counters.visible)
        +"  "+text.GetText("Rendering.OcclusionHudCulled")+" "+std::to_string(counters.culled);
    if(counters.overflow)counts+="  "+text.GetText("Rendering.OcclusionOverflow");
    append(UiKind::Label,{content.x,panel.y+30,content.width,18},panel,{.86f,.92f,1,1},std::move(counts));
    append(UiKind::Label,{content.x,panel.y+50,content.width,18},panel,{.62f,.76f,.9f,1},
        RateText(text,counters));
    const UiRect bar{content.x,panel.y+76,content.width,8};
    append(UiKind::Panel,bar,panel,{.55f,.12f,.12f,.95f});
    const float ratio=counters.candidates>0
        ?(std::clamp)(float(counters.visible)/float(counters.candidates),0.0f,1.0f):0.0f;
    if(ratio>0)append(UiKind::Panel,{bar.x,bar.y,bar.width*ratio,bar.height},panel,{.15f,.65f,.25f,.95f});
    return output;
}
}
