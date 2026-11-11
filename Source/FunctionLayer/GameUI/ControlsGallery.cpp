#include "ControlsGallery.h"
#include <algorithm>

namespace se
{
bool BuildControlsGallery(UiTree& output,const Localization& text,ControlsGallery& gallery,std::string imageAsset)
{
    UiTree ui;ControlsGallery ids;
    bool valid=ui.SetCanvasReference(1280,720);
    const auto create=[&](UiKind kind,UiId parent,const char* caption,UiPoint position,UiPoint size){
        const auto id=ui.Create(kind,parent);UiStyle style;style.offset=position;style.size=size;
        valid=id&&ui.SetStyle(id,style)&&valid;
        if(caption)valid=ui.SetText(id,text.GetText(caption))&&valid;
        return id;
    };
    ids.root=create(UiKind::Panel,0,nullptr,{0,0},{1280,720});
    valid=ui.SetColor(ids.root,{0.08f,0.10f,0.14f,1})&&ui.SetBlocksPointer(ids.root,true)&&valid;
    ids.title=create(UiKind::Label,ids.root,"UI.Gallery.Title",{24,20},{1200,40});
    ids.controls=create(UiKind::Panel,ids.root,nullptr,{24,80},{400,540});
    auto controlsStyle=ui.Get(ids.controls)->style;controlsStyle.clip=true;
    valid=ui.SetStyle(ids.controls,controlsStyle)&&ui.SetColor(ids.controls,{0.13f,0.16f,0.22f,1})&&valid;
    ids.label=create(UiKind::Label,ids.controls,"UI.Gallery.Label",{20,20},{360,40});
    ids.image=create(UiKind::Image,ids.root,"UI.Gallery.Image",{920,90},{240,180});
    valid=ui.SetImage(ids.image,std::move(imageAsset))&&ui.SetColor(ids.image,{0.35f,0.65f,0.95f,1})&&valid;
    ids.button=create(UiKind::Button,ids.controls,"UI.Gallery.Button",{20,80},{340,40});
    ids.checkbox=create(UiKind::Checkbox,ids.controls,"UI.Gallery.Checkbox",{20,140},{340,40});
    valid=ui.SetValue(ids.checkbox,1,false)&&valid;
    ids.slider=create(UiKind::Slider,ids.controls,"UI.Gallery.Slider",{20,210},{340,36});
    valid=ui.SetRange(ids.slider,0,100)&&ui.SetStep(ids.slider,5)&&ui.SetValue(ids.slider,40,false)&&valid;
    ids.progress=create(UiKind::ProgressBar,ids.controls,"UI.Gallery.Progress",{20,270},{340,28});
    valid=ui.SetValue(ids.progress,0.4,false)&&valid;
    ids.textInput=create(UiKind::TextInput,ids.controls,"UI.Gallery.TextInput",{20,330},{340,100});
    ids.scroll=create(UiKind::ScrollView,ids.root,"UI.Gallery.Scroll",{460,90},{400,450});
    auto scrollStyle=ui.Get(ids.scroll)->style;scrollStyle.layout=UiLayout::Vertical;scrollStyle.padding=12;scrollStyle.spacing=8;scrollStyle.clip=true;
    valid=ui.SetStyle(ids.scroll,scrollStyle)&&ui.SetColor(ids.scroll,{.04f,.065f,.1f,1})&&valid;
    for(unsigned index=0;index<14;++index)
    {
        const auto item=create(UiKind::Button,ids.scroll,nullptr,{0,0},{356,38});
        valid=ui.SetText(item,text.GetText("UI.Gallery.ScrollItem")+" "+std::to_string(index+1))&&valid;
        ids.scrollButtons.push_back(item);
    }
    ids.animated=create(UiKind::Panel,ids.root,nullptr,{920,340},{160,120});
    valid=ui.SetColor(ids.animated,{0.2f,0.5f,0.95f,1})&&valid;
    ids.animatedLabel=create(UiKind::Label,ids.animated,"UI.Gallery.Animated",{8,16},{144,72});
    ids.status=create(UiKind::Label,ids.root,"UI.Gallery.Ready",{24,650},{1200,42});
    if(!valid)return false;
    ui.Layout(1280,720);output=std::move(ui);gallery=std::move(ids);return true;
}
void UpdateControlsGallery(UiTree& ui,ControlsGallery& gallery,std::span<const UiEvent> events,const Localization& text)
{
    for(const auto& event:events)
    {
        if(event.kind==UiEventKind::Click&&event.target==gallery.button)
        {
            ++gallery.clicks;gallery.animationTarget=!gallery.animationTarget;
            ui.SetText(gallery.status,text.GetText("UI.Gallery.ClickCount")+" "+std::to_string(gallery.clicks));
            ui.MoveTo(gallery.animated,gallery.animationTarget?UiPoint{1060,480}:UiPoint{920,340},0.6);
            ui.ColorTo(gallery.animated,gallery.animationTarget?UiColor{0.95f,0.45f,0.2f,1}:UiColor{0.2f,0.5f,0.95f,1},0.6);
            ui.FadeTo(gallery.animated,gallery.animationTarget?0.45f:1.0f,0.6);
        }
        if(event.kind==UiEventKind::ValueChanged&&event.target==gallery.checkbox)
            ui.SetVisible(gallery.image,event.value!=0);
        if(event.kind==UiEventKind::ValueChanged&&event.target==gallery.slider)
            ui.SetValue(gallery.progress,std::clamp(event.value/100.0,0.0,1.0),false);
        if(event.kind==UiEventKind::TextChanged&&event.target==gallery.textInput)
            ui.SetText(gallery.status,text.GetText("UI.Gallery.TextChanged")+" "+event.text);
        if(event.kind==UiEventKind::Click)
        {
            const auto found=std::find(gallery.scrollButtons.begin(),gallery.scrollButtons.end(),event.target);
            if(found!=gallery.scrollButtons.end())ui.SetText(gallery.status,text.GetText("UI.Gallery.ScrollClicked")+" "+std::to_string(found-gallery.scrollButtons.begin()+1));
        }
    }
}
}
