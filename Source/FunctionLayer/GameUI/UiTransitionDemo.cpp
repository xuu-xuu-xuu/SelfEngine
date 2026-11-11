#include "UiTransitionDemo.h"
#include <cmath>

namespace se
{
bool BuildUiTransitionDemo(UiTree& output,const Localization& text,UiTransitionDemo& demo)
{
    UiTree ui;UiTransitionDemo ids;bool valid=ui.SetCanvasReference(960,640);
    const auto create=[&](UiKind kind,UiId parent,UiPoint position,UiPoint size,UiColor color,const char* caption=nullptr)
    {
        const auto id=ui.Create(kind,parent);UiStyle style;style.offset=position;style.size=size;
        valid=id&&ui.SetStyle(id,style)&&ui.SetColor(id,color)&&valid;
        if(caption)valid=ui.SetText(id,text.GetText(caption))&&valid;return id;
    };
    ids.root=create(UiKind::Panel,0,{0,0},{960,640},{.015f,.024f,.04f,1});
    ids.title=create(UiKind::Label,ids.root,{32,28},{896,48},{.8f,.9f,1,1},"UI.Transitions.Title");
    create(UiKind::Label,ids.root,{32,90},{896,70},{.6f,.72f,.86f,1},"UI.Transitions.Help");
    ids.toggle=create(UiKind::Button,ids.root,{32,190},{300,56},{1,1,1,1},"UI.Transitions.Toggle");
    ids.menu=create(UiKind::Panel,ids.root,{400,180},{480,310},{.04f,.1f,.18f,1});
    valid=ui.SetBlocksPointer(ids.menu,true)&&valid;
    create(UiKind::Label,ids.menu,{24,24},{432,48},{.8f,.9f,1,1},"UI.Transitions.Menu");
    create(UiKind::Label,ids.menu,{24,90},{432,108},{.7f,.82f,.96f,1},"UI.Transitions.Description");
    ids.close=create(UiKind::Button,ids.menu,{24,226},{432,56},{1,1,1,1},"UI.Transitions.Close");
    create(UiKind::Label,ids.root,{32,550},{896,70},{.6f,.72f,.86f,1},"UI.Transitions.Footer");
    if(!valid)return false;ui.Layout(960,640);output=std::move(ui);demo=ids;return true;
}
bool SetUiTransitionMenuOpen(UiTree& ui,UiTransitionDemo& demo,bool open,double seconds)
{
    if(!ui.Get(demo.menu)||!std::isfinite(seconds)||seconds<0)return false;
    demo.open=open;ui.SetVisible(demo.menu,true);ui.SetEnabled(demo.menu,open);
    ui.MoveTo(demo.menu,open?UiPoint{400,180}:UiPoint{480,180},seconds);
    ui.FadeTo(demo.menu,open?1.0f:0.0f,seconds);
    if(!open&&seconds==0)ui.SetVisible(demo.menu,false);
    return true;
}
void UpdateUiTransitionDemo(UiTree& ui,UiTransitionDemo& demo,std::span<const UiEvent> events)
{
    for(const auto& event:events)
    {
        if(event.target==demo.toggle||event.target==demo.close)
        {
            if(event.kind==UiEventKind::PointerEnter)ui.ColorTo(event.target,{.45f,.8f,1,1},.15);
            if(event.kind==UiEventKind::PointerLeave)ui.ColorTo(event.target,{1,1,1,1},.15);
        }
        if(event.kind==UiEventKind::Click&&event.target==demo.toggle)SetUiTransitionMenuOpen(ui,demo,!demo.open);
        if(event.kind==UiEventKind::Click&&event.target==demo.close)SetUiTransitionMenuOpen(ui,demo,false);
    }
    const auto menu=ui.Get(demo.menu);
    if(menu&&!demo.open&&menu->opacity==0)ui.SetVisible(demo.menu,false);
}
}
