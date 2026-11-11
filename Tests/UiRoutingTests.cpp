#include "FunctionLayer/GameUI/UiEventSystem.h"
#include "PlatformLayer/Windows/WindowsUiInput.h"
#include "PlatformLayer/Windows/Window.h"
#include "FunctionLayer/Application/FreeCameraInput.h"
#include <nlohmann/json.hpp>
#include <algorithm>
#include <fstream>
#include <filesystem>
#include <iostream>
#include <limits>
#include <stdexcept>
using namespace se;
namespace
{
void Check(bool value,const char* message){if(!value)throw std::runtime_error(message);}
UiId Node(UiTree& tree,UiKind kind,UiRect r,UiId parent=0,bool clip=false)
{const auto id=tree.Create(kind,parent);UiStyle style;style.offset={r.x,r.y};style.size={r.width,r.height};style.clip=clip;Check(tree.SetStyle(id,style),"node style");return id;}
std::size_t Clicks(UiTree& tree,UiId id){const auto events=tree.TakeEvents();return std::count_if(events.begin(),events.end(),[&](const auto& e){return e.kind==UiEventKind::Click&&e.target==id;});}
void Routing()
{
    UiTree tree;const auto panel=Node(tree,UiKind::Panel,{10,10,200,180},0,true),bottom=Node(tree,UiKind::Button,{10,10,100,40},panel),top=Node(tree,UiKind::Button,{10,10,100,40},panel);
    const auto label=Node(tree,UiKind::Label,{0,0,100,40},top);(void)label;tree.Layout(640,480);
    const auto route=UiEventSystem::Route(tree,{30,30});Check(route.target==top&&route.path==std::vector<UiId>{top,panel},"topmost interactive target and leaf-to-root route");
    Check(tree.PointerDown({30,30})&&tree.PointerDown({180,120})&&tree.Captured()==top,"duplicate down preserves capture");Check(tree.PointerUp({30,30})&&Clicks(tree,top)==1,"one target click");
    tree.PointerDown({30,30});tree.PointerMove({400,400});Check(tree.PointerUp({400,400})&&Clicks(tree,top)==0,"drag outside release cancels");
    tree.PointerDown({30,30});tree.PointerMove({400,400});tree.PointerMove({30,30});tree.PointerUp({30,30});Check(Clicks(tree,top)==1,"drag outside and back clicks once");
    tree.PointerDown({30,30});const auto overlay=Node(tree,UiKind::Button,{10,10,100,40},panel);tree.Layout(640,480);tree.PointerUp({30,30});Check(Clicks(tree,top)==0,"new topmost overlay prevents old target click");tree.Remove(overlay);
    tree.PointerDown({30,30});tree.SetEnabled(top,false);Check(!tree.Captured()&&!tree.Focused()&&UiEventSystem::HitTarget(tree,{30,30})==bottom,"disabled target clears capture/focus");tree.PointerUp({30,30});Check(Clicks(tree,bottom)==0,"release never clicks lower control");tree.SetEnabled(top,true);
    tree.PointerDown({30,30});tree.SetVisible(panel,false);Check(!tree.Captured()&&!tree.Focused()&&!tree.Hovered(),"hidden ancestor clears all input");tree.SetVisible(panel,true);tree.Layout(640,480);
    const float nan=std::numeric_limits<float>::quiet_NaN();tree.PointerDown({30,30});const auto focus=tree.Focused();
    Check(!tree.PointerMove({nan,0})&&!tree.PointerDown({0,nan})&&!tree.PointerUp({nan,0})&&!tree.Wheel({0,nan},1)&&tree.Captured()==top&&tree.Focused()==focus,"invalid coordinates preserve input state");tree.CancelInput();
    tree.PointerMove({30,30});tree.TakeEvents();Check(tree.PointerLeave()&&!tree.PointerLeave(),"pointer leaves once");const auto leaves=tree.TakeEvents();Check(leaves.size()==1&&leaves[0].kind==UiEventKind::PointerLeave,"one leave notification");
    const auto shield=Node(tree,UiKind::Panel,{0,0,300,230});tree.Layout(640,480);Check(UiEventSystem::HitTarget(tree,{30,30})==top,"visual panel passes through by default");
    Check(tree.SetBlocksPointer(shield,true)&&tree.PointerDown({30,30})&&tree.Captured()==shield&&tree.ConsumesKeyboard(),"blocking panel targets and focuses");tree.PointerUp({30,30});Check(Clicks(tree,top)==0,"menu panel prevents underlying action");
    tree.SetBlocksPointer(shield,false);Check(!tree.Focused()&&UiEventSystem::HitTarget(tree,{30,30})==top,"disable blocking restores pass-through");tree.SetBlocksPointer(shield,true);
    const auto saved=tree.SaveJson();UiTree loaded;std::string error;Check(loaded.LoadJson(saved,error)&&loaded.Get(shield)->blocksPointer,"blocking JSON roundtrip");auto legacy=nlohmann::json::parse(saved);for(auto& node:legacy["nodes"])node.erase("blocksPointer");Check(loaded.LoadJson(legacy.dump(),error)&&!loaded.Get(shield)->blocksPointer,"old JSON defaults pass-through");
    auto invalid=nlohmann::json::parse(saved);invalid["nodes"][0]["blocksPointer"]=1;const auto before=loaded.SaveJson();Check(!loaded.LoadJson(invalid.dump(),error)&&loaded.SaveJson()==before,"invalid bool load atomic");
    tree.Remove(shield);tree.PointerDown({30,30});tree.Remove(top);Check(!tree.Captured()&&!tree.Focused()&&!tree.Hovered(),"delete target no dangling input");tree.PointerUp({30,30});Check(Clicks(tree,bottom)==0,"deleted gesture cannot click surviving sibling");
    const auto slider=Node(tree,UiKind::Slider,{300,0,100,30});tree.Layout(640,480);tree.SetRange(slider,0,10);tree.PointerDown({350,10});tree.PointerMove({1000,10});tree.PointerUp({1000,10});Check(tree.Get(slider)->value==10,"slider drags outside with clamp");
}
void Scroll()
{
    UiTree tree;const auto outer=Node(tree,UiKind::ScrollView,{0,0,200,150},0,true);UiStyle style=tree.Get(outer)->style;style.layout=UiLayout::Vertical;tree.SetStyle(outer,style);
    const auto inner=Node(tree,UiKind::ScrollView,{0,0,180,100},outer,true);style=tree.Get(inner)->style;style.layout=UiLayout::Vertical;tree.SetStyle(inner,style);
    for(unsigned i=0;i<5;++i)Node(tree,UiKind::Button,{0,0,150,30},inner);Node(tree,UiKind::Button,{0,0,180,400},outer);tree.Layout(640,480);
    const UiPoint point{20,20};Check(tree.Wheel(point,-100)&&tree.Get(inner)->scroll==50&&tree.Get(outer)->scroll==0,"closest scroll receives first");
    Check(tree.Wheel(point,-1)&&tree.Get(outer)->scroll==32,"inner edge bubbles to outer scroll");tree.Wheel(point,100);tree.Wheel(point,100);
    Check(tree.Wheel(point,1)&&tree.Get(inner)->scroll==0&&tree.Get(outer)->scroll==0,"UI boundary wheel consumed without scene leakage");Check(!tree.Wheel({500,400},1),"scene wheel passes through");
}
LPARAM Point(short x,short y){return MAKELPARAM(x,y);}
void Platform()
{
    UiTree tree;auto button=Node(tree,UiKind::Button,{10,10,100,40});const auto text=Node(tree,UiKind::TextInput,{10,80,200,40});tree.Layout(640,480);
    WindowState window;window.postQuitOnDestroy=false;Check(CreateEngineWindow(window,false)&&SetEngineWindowClientSize(window,640,480),"native hidden input surface");
    struct WindowScope{WindowState& window;~WindowScope(){window.messageHandler={};DestroyEngineWindow(window);}} scope{window};
    WindowsUiInput input(tree);window.messageHandler=[&](HWND hwnd,UINT message,WPARAM wp,LPARAM lp){return input.Process(hwnd,message,wp,lp);};
    SendMessageW(window.hwnd,WM_SETFOCUS,0,0);SendMessageW(window.hwnd,WM_MOUSEMOVE,0,Point(30,30));tree.TakeEvents();
    SendMessageW(window.hwnd,WM_MOUSELEAVE,0,0);auto events=tree.TakeEvents();Check(events.size()==1&&events[0].kind==UiEventKind::PointerLeave&&!tree.Hovered(),"real WM_MOUSELEAVE clears hover");
    SendMessageW(window.hwnd,WM_LBUTTONDOWN,MK_LBUTTON,Point(30,30));Check(GetCapture()==window.hwnd&&tree.Captured()==button&&window.input.isLeftDown&&input.PointerConsumed(),("raw button state preserved while UI captures; window="+std::to_string(GetCapture()==window.hwnd)+", tree="+std::to_string(tree.Captured())+", left="+std::to_string(window.input.isLeftDown)).c_str());
    tree.Remove(button);input.SynchronizeCapture(window.hwnd);Check(GetCapture()!=window.hwnd&&!tree.Captured()&&input.PointerConsumed(),"delete immediately releases OS capture and retains gesture consumption");input.EndFrame();
    Check(input.PointerConsumed(),"deleted gesture remains consumed across frames");SendMessageW(window.hwnd,WM_LBUTTONUP,0,Point(500,400));Check(input.PointerConsumed(),"deleted gesture up still consumed");input.EndFrame();Check(!input.PointerConsumed(),"next frame releases gesture latch");
    SendMessageW(window.hwnd,WM_LBUTTONDOWN,MK_LBUTTON,Point(500,400));Check(!input.PointerConsumed(),"fresh scene gesture passes through");SendMessageW(window.hwnd,WM_LBUTTONUP,0,Point(500,400));input.EndFrame();
    SendMessageW(window.hwnd,WM_LBUTTONDOWN,MK_LBUTTON,Point(30,90));SendMessageW(window.hwnd,WM_LBUTTONUP,0,Point(30,90));SendMessageW(window.hwnd,WM_KEYDOWN,'W',0);SendMessageW(window.hwnd,WM_RBUTTONDOWN,MK_RBUTTON,Point(30,90));
    FreeCameraInputRouter camera;const auto filtered=camera.Capture(window.input,true,input.KeyboardConsumed(),input.PointerConsumed());Check(Length(filtered.move)==0&&filtered.lookPixels.x==0&&filtered.lookPixels.y==0&&window.input.keysDown['W'],"real typed W and pointer cannot move gameplay camera");
    tree.Remove(text);Check(input.KeyboardConsumed(),"typed key remains consumed in deletion frame");input.EndFrame();Check(!input.KeyboardConsumed(),"deleted focus not retained next frame");
    button=Node(tree,UiKind::Button,{10,10,100,40});tree.Layout(640,480);SendMessageW(window.hwnd,WM_LBUTTONDOWN,MK_LBUTTON,Point(30,30));SendMessageW(window.hwnd,WM_CANCELMODE,0,0);Check(!tree.Captured()&&GetCapture()!=window.hwnd,"cancel mode releases capture");
    SendMessageW(window.hwnd,WM_LBUTTONUP,0,Point(30,30));Check(Clicks(tree,button)==0,"cancel mode suppresses click");input.EndFrame();SendMessageW(window.hwnd,WM_KILLFOCUS,0,0);Check(!tree.Focused()&&!tree.Captured()&&!window.input.hasFocus,"focus loss clears raw and UI input");window.messageHandler={};
}
}
int main(int argc,char** argv)
{
    try{Routing();Scroll();Platform();if(argc>1){const auto path=std::filesystem::path(argv[1]);std::filesystem::create_directories(path.parent_path());std::ofstream file(path);file<<"{\"success\":true,\"topmost\":true,\"capture\":true,\"bubble\":true,\"blocking\":true,\"jsonLegacy\":true,\"deleteGesture\":true,\"windowCapture\":true,\"gameplayFiltered\":true}\n";}std::cout<<"UiRoutingTests passed (target/bubble, capture, cancel, blocking, JSON, native Windows gesture and camera filtering).\n";return 0;}catch(const std::exception& error){std::cerr<<error.what()<<'\n';return 1;}
}
