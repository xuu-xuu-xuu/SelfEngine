#pragma once
#include "UiTree.h"
#include "CoreLayer/Localization.h"
#include <span>

namespace se
{
struct ControlsGallery
{
    UiId root=0,title=0,controls=0,label=0,image=0,button=0,checkbox=0,slider=0,progress=0,textInput=0;
    UiId scroll=0,animated=0,animatedLabel=0,status=0;
    std::vector<UiId> scrollButtons;
    unsigned clicks=0;
    bool animationTarget=false;
};
// 新建完整示例树；失败保持 output/gallery。固定创建顺序可重建相同 ID。
bool BuildControlsGallery(UiTree& output,const Localization& localization,ControlsGallery& gallery,
    std::string imageAsset={});
// 调用方统一 TakeEvents 后可把同一批事件交给其他观察者；本函数不清事件、不推进时间。
void UpdateControlsGallery(UiTree& ui,ControlsGallery& gallery,std::span<const UiEvent> events,
    const Localization& localization);
}
