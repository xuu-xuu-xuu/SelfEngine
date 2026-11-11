#include "FunctionLayer/GameUI/UiTree.h"
#include "ResourceLayer/Fonts/FontCache.h"
#include <iostream>
#include <stdexcept>
#include <algorithm>
using namespace se;
void Check(bool result,const char* message){if(!result)throw std::runtime_error(message);}
int main()
{
    try
    {
        UiTree ui;auto panel=ui.Create(UiKind::Panel);UiStyle p;p.size={300,200};p.layout=UiLayout::Vertical;p.spacing=4;ui.SetStyle(panel,p);
        auto button=ui.Create(UiKind::Button,panel);auto input=ui.Create(UiKind::TextInput,panel);auto slider=ui.Create(UiKind::Slider,panel);
        Check(button!=0,"button created");
        ui.Layout(800,600);Check(ui.Get(input)->rectangle.y==36,"vertical layout");
        Check(ui.PointerDown({10,10})&&ui.PointerUp({10,10}),"button capture");
        auto events=ui.TakeEvents();Check(std::count_if(events.begin(),events.end(),[](auto&e){return e.kind==UiEventKind::Click;})==1,"one click");
        ui.PointerDown({10,10});ui.PointerUp({400,400});events=ui.TakeEvents();Check(std::none_of(events.begin(),events.end(),[](auto&e){return e.kind==UiEventKind::Click;}),"outside release");
        ui.Focus(input);Check(ui.InsertText("中文A\xF0\x9F\x98\x80"),"unicode input");ui.EditKey(8,false,false);Check(ui.Get(input)->text=="中文A","surrogate-safe backspace");
        ui.SelectText(2,0);Check(ui.SelectedText()=="中文","selection codepoints");ui.InsertText("测试");Check(ui.Get(input)->text=="测试A","replace selection");
        ui.SetComposition("拼音");Check(ui.Visuals()[2].composition=="拼音","IME preview independent");ui.CancelComposition();Check(ui.Get(input)->text=="测试A","cancel unchanged");
        ui.SetRange(slider,0,100);ui.PointerDown({60,80});ui.PointerMove({1000,80});ui.PointerUp({1000,80});Check(ui.Get(slider)->value==100,"captured slider clamped");
        ui.Focus(input);ui.SetEnabled(panel,false);Check(!ui.Focused()&&!ui.PointerDown({10,10}),"ancestor disables input");ui.SetEnabled(panel,true);
        ui.FadeTo(panel,0.2f,1);ui.Update(0.5);Check(ui.Get(panel)->opacity>0.59f&&ui.Get(panel)->opacity<0.61f,"fade interpolation");
        ui.MoveTo(panel,{100,50},1);ui.ColorTo(panel,{0,0.5f,1,1},1);ui.Update(0.5);
        Check(ui.Get(panel)->style.offset.x==50&&ui.Get(panel)->color.r==0.5f,"position and color transition");
        ui.MoveTo(panel,{0,0},0.5);ui.Update(0.5);Check(ui.Get(panel)->style.offset.x==0,"new transition replaces old from current value");
        ui.SetStep(slider,10);ui.SetValue(slider,54);Check(ui.Get(slider)->value==50,"slider step");
        ui.SetCanvasReference(800,600);ui.Layout(1600,1200);Check(ui.Get(input)->rectangle.width==240&&ui.CanvasScale()==2,"reference scaling");
        const auto saved=ui.SaveJson();UiTree loaded;std::string error;Check(loaded.LoadJson(saved,error),"UI roundtrip");Check(loaded.SaveJson()==saved,"UI properties retained");
        Check(!loaded.LoadJson("{bad",error)&&loaded.SaveJson()==saved,"failed load preserves tree");Check(!loaded.Reparent(panel,input),"cycle rejected");
        UiTree deep;UiId parent=0;for(int i=0;i<128;++i){parent=deep.Create(UiKind::Panel,parent);Check(parent!=0,"allowed hierarchy depth");}Check(deep.Create(UiKind::Panel,parent)==0,"depth bounded");
        auto scroll=loaded.Create(UiKind::ScrollView);UiStyle ss;ss.offset={400,0};ss.size={100,70};ss.layout=UiLayout::Vertical;loaded.SetStyle(scroll,ss);
        for(int i=0;i<5;++i)loaded.Create(UiKind::Button,scroll);loaded.Layout(800,600);Check(loaded.Wheel({410,10},-1)&&loaded.Get(scroll)->scroll>0,"scrolling");
        loaded.Remove(panel);Check(!loaded.Get(input),"remove subtree and state");
        FontCache fonts;TextLayout text;Check(fonts.Layout("中文 text 😀",20,400,text,error),error.c_str());Check(!text.quads.empty()&&!fonts.Pages().empty(),"CPU raster and atlas");
        auto old=text.quads.front();auto revision=fonts.Pages()[0].revision;TextLayout repeat;Check(fonts.Layout("中文 text 😀",20,400,repeat,error),"cached shape");Check(fonts.Pages()[0].revision==revision,"same runs no new uploads");
        TextLayout another;Check(fonts.Layout("新字形",20,400,another,error),"atlas growth");Check(text.quads.front().pixels.x==old.pixels.x,"old coordinates stable");
        Check(text.Caret(2).x>text.Caret(0).x,"caret positions");auto caret=text.Caret(2);Check(text.HitTest(caret.x,caret.y+2)==2,"text hit test");
        Check(!fonts.Layout("\xC0\xAF",20,400,another,error),"bad UTF rejected");
        Check(fonts.Layout(std::string(250,'W'),20,4000,another,error)&&another.quads.size()>1,"wide glyph run tiled across atlas");
        std::cout<<"GameUITests passed (layout/input/IME model/serialization/font CPU atlas)\n";return 0;
    }
    catch(const std::exception& e){std::cerr<<"GameUITests failed: "<<e.what()<<'\n';return 1;}
}
