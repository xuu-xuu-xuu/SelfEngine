#pragma once
#include "UiTree.h"
#include "CoreLayer/Localization.h"
#include <span>

namespace se
{
struct UiTransitionDemo
{
    UiId root=0,title=0,toggle=0,menu=0,close=0;
    bool open=true;
};
bool BuildUiTransitionDemo(UiTree& output,const Localization& text,UiTransitionDemo& demo);
bool SetUiTransitionMenuOpen(UiTree& ui,UiTransitionDemo& demo,bool open,double seconds=.3);
// 时间只由 UiTree::Update 推进；此处处理事件和淡出后的隐藏。
void UpdateUiTransitionDemo(UiTree& ui,UiTransitionDemo& demo,std::span<const UiEvent> events);
}
