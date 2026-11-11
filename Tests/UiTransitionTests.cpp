#include "FunctionLayer/GameUI/UiTransitionDemo.h"
#include "FunctionLayer/GameUI/UiEventSystem.h"
#include <nlohmann/json.hpp>
#include <cmath>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <limits>
#include <stdexcept>
using namespace se;
namespace
{
void Check(bool value,const char* message){if(!value)throw std::runtime_error(message);}
bool Near(float a,float b){return std::abs(a-b)<.0001f;}
void Start(UiTree& ui,UiId id,double duration)
{ui.MoveTo(id,{120,80},duration);ui.FadeTo(id,.25f,duration);ui.ColorTo(id,{.2f,.4f,.8f,.6f},duration);}
nlohmann::json Rates()
{
    nlohmann::json records=nlohmann::json::array();
    for(const auto fps:{30,60,120,144})
    {
        UiTree ui;const auto id=ui.Create(UiKind::Panel);unsigned frames=0;
        do{Start(ui,id,.8);ui.Update(1.0/fps);++frames;Check(frames<1000,"repeated same target must finish");}while(ui.ActiveTransitions());
        const double time=double(frames)/fps;
        Check(time>=.8-1e-6&&time<=.8+1.0/fps+1e-6,"duration independent of display rate");
        const auto n=ui.Get(id);Check(n->style.offset==UiPoint{120,80}&&n->opacity==.25f&&n->color==UiColor{.2f,.4f,.8f,.6f},"exact final values");
        records.push_back({{"fps",fps},{"frames",frames},{"seconds",time}});
    }
    UiTree ui;const auto id=ui.Create(UiKind::Panel);Start(ui,id,.8);for(const auto dt:{.07,.005,.21,.12,.395})ui.Update(dt);
    Check(!ui.ActiveTransitions()&&ui.Get(id)->style.offset.x==120,"irregular UI time reaches same target");return records;
}
void Rules()
{
    UiTree ui;const auto id=ui.Create(UiKind::Panel);Start(ui,id,1);ui.Update(.25);
    const auto before=*ui.Get(id);ui.MoveTo(id,{0,0},.5);ui.FadeTo(id,1,.5);ui.ColorTo(id,{1,1,1,1},.5);
    Check(ui.Get(id)->style.offset==before.style.offset&&ui.Get(id)->opacity==before.opacity&&ui.Get(id)->color==before.color,"reverse has no first-frame jump");ui.Update(.25);
    Check(Near(ui.Get(id)->style.offset.x,15)&&Near(ui.Get(id)->opacity,.90625f),"reverse starts at currently displayed value");ui.Update(.25);Check(!ui.ActiveTransitions(),"reverse clears all tracks");
    Start(ui,id,1);ui.Update(.2);const auto snapshot=ui.SaveJson();const auto count=ui.ActiveTransitions();const auto nan=std::numeric_limits<double>::quiet_NaN();
    ui.Update(-1);ui.Update(nan);ui.MoveTo(id,{std::numeric_limits<float>::infinity(),0},1);ui.ColorTo(id,{2,0,0,1},1);ui.FadeTo(id,0,-1);
    Check(ui.SaveJson()==snapshot&&ui.ActiveTransitions()==count,"invalid requests leave active state unchanged");
    auto style=ui.Get(id)->style;style.offset={7,9};Check(ui.SetStyle(id,style)&&ui.SetColor(id,{.1f,.2f,.3f,1})&&ui.ActiveTransitions()==1,"explicit setters cancel only their own tracks");
    ui.Update(1);Check(ui.Get(id)->style.offset==UiPoint{7,9}&&ui.Get(id)->color==UiColor{.1f,.2f,.3f,1},"old targets cannot override setters");
    Start(ui,id,1);ui.Update(.1);const auto held=ui.SaveJson();Check(ui.CancelTransitions(id)&&!ui.ActiveTransitions(),"explicit cancellation clears tracks");ui.Update(1);Check(ui.SaveJson()==held,"cancel holds current display");
    Start(ui,id,0);Check(!ui.ActiveTransitions()&&ui.Get(id)->style.offset==UiPoint{120,80}&&ui.Get(id)->opacity==.25f,"zero duration is immediate");
    ui.MoveTo(id,{0,0},std::numeric_limits<double>::max());ui.Update(std::numeric_limits<double>::max()/2);Check(ui.Get(id)->style.offset.x==60,"huge finite clock midpoint");ui.Update(std::numeric_limits<double>::max());Check(!ui.ActiveTransitions()&&ui.Get(id)->style.offset.x==0,"finite clock saturation avoids overflow");
    Start(ui,id,1);ui.SetVisible(id,false);ui.SetEnabled(id,false);ui.Update(1);Check(!ui.ActiveTransitions()&&ui.Get(id)->opacity==.25f,"hidden disabled tracks still complete");
    ui.SetVisible(id,true);ui.SetEnabled(id,true);ui.FadeTo(id,1,1);const auto saved=ui.SaveJson();std::string error;Check(!ui.LoadJson("{bad",error)&&ui.ActiveTransitions()==1,"failed load preserves tracks");Check(ui.LoadJson(saved,error)&&!ui.ActiveTransitions(),"successful load retains values without playback");
    const auto child=ui.Create(UiKind::Panel,id);Start(ui,id,1);Start(ui,child,1);Check(ui.Remove(id)&&!ui.ActiveTransitions()&&!ui.Get(child),"subtree deletion cancels immediately");ui.Update(1);const auto replacement=ui.Create(UiKind::Panel);Check(replacement>child&&ui.Get(replacement)->opacity==1&&!ui.CancelTransitions(id),"retired ID cannot animate replacement");
}
void Menu()
{
    Localization text;std::string error;Check(text.Load("Resources/Localization/zh-CN.txt",error),"menu language");UiTree ui;UiTransitionDemo ids;Check(BuildUiTransitionDemo(ui,text,ids),"menu fixture");
    const auto observe=[&]{const auto events=ui.TakeEvents();UpdateUiTransitionDemo(ui,ids,events);ui.Layout(960,640);};
    const auto button=ui.Get(ids.toggle)->rectangle;const UiPoint pointer{button.x+20,button.y+20};ui.PointerMove(pointer);observe();ui.Update(.075);Check(ui.Get(ids.toggle)->color.r<1&&ui.Get(ids.toggle)->color.r>.45f,"hover color transition");ui.PointerLeave();observe();ui.Update(.15);
    for(unsigned i=0;i<40;++i){Check(SetUiTransitionMenuOpen(ui,ids,false),"close request");Check(!ui.Get(ids.menu)->enabled,"closing disables menu immediately");ui.Update(.04);observe();Check(SetUiTransitionMenuOpen(ui,ids,true),"reverse request");ui.Update(.01);observe();}
    ui.Update(.3);observe();Check(ui.Get(ids.menu)->visible&&ui.Get(ids.menu)->enabled&&ui.Get(ids.menu)->opacity==1&&ui.Get(ids.menu)->style.offset==UiPoint{400,180}&&!ui.ActiveTransitions(),"rapid reversals settle without residual state");
    ui.PointerDown(pointer);ui.PointerUp(pointer);observe();Check(!ids.open,"real toggle event closes menu");ui.Update(.3);observe();Check(!ui.Get(ids.menu)->visible&&!ui.Get(ids.menu)->enabled&&!ui.ActiveTransitions(),"finished closing hides menu");
    Check(SetUiTransitionMenuOpen(ui,ids,true,0),"immediate reopen");observe();const auto close=ui.Get(ids.close)->rectangle;Check(UiEventSystem::HitTarget(ui,{close.x+10,close.y+10})==ids.close,"reopened menu hit matches display");
    Check(ui.Remove(ids.menu)&&!ui.ActiveTransitions()&&!SetUiTransitionMenuOpen(ui,ids,true),"destroyed menu cannot be reanimated");UpdateUiTransitionDemo(ui,ids,{});
}
}
int main(int argc,char** argv)
{
    try{const auto rates=Rates();Rules();Menu();const std::filesystem::path path=argc>1?argv[1]:"out/validation/ui-transition/verification.json";std::filesystem::create_directories(path.parent_path());std::ofstream report(path);report<<nlohmann::json{{"success",true},{"rates",rates},{"irregularTime",true},{"interrupt",true},{"repeat",true},{"immediate",true},{"cancel",true},{"destroy",true},{"load",true},{"finiteClock",true},{"rapidReversals",40},{"hover",true}}.dump(2);Check(bool(report),"transition evidence");std::cout<<"UiTransitionTests passed (frame rates, replacement, cancellation, lifetime and rapid menu reversals).\n";return 0;}
    catch(const std::exception& e){std::cerr<<e.what()<<'\n';return 1;}
}
