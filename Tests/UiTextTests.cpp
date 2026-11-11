#include "FunctionLayer/GameUI/UiTextPresentation.h"
#include "PlatformLayer/Windows/WindowsUiInput.h"
#include "PlatformLayer/Windows/Window.h"
#include <imm.h>
#include <algorithm>
#include <array>
#include <cmath>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <stdexcept>
using namespace se;
namespace
{
void Check(bool value,const char* message){if(!value)throw std::runtime_error(message);}
UiId Field(UiTree& tree,float y,std::string text)
{const auto id=tree.Create(UiKind::TextInput);UiStyle s;s.offset={10,y};s.size={220,60};Check(tree.SetStyle(id,s)&&tree.SetText(id,text),"text fixture");return id;}
UiVisual Visual(const UiTree& tree,UiId id){for(auto v:tree.Visuals())if(v.id==id)return v;throw std::runtime_error("missing text visual");}
std::size_t Changes(UiTree& tree){const auto events=tree.TakeEvents();return std::count_if(events.begin(),events.end(),[](const auto& e){return e.kind==UiEventKind::TextChanged;});}
void Editing()
{
    UiTree tree;const auto id=Field(tree,10,"A中😀B");tree.Layout(640,480);tree.Focus(id);tree.TakeEvents();
    Check(tree.SelectText(3,1)&&tree.SelectedText()=="中😀","selection counts Unicode codepoints");tree.EditKey(37,false,false);Check(Visual(tree,id).caret==1&&tree.SelectedText().empty(),"left collapses selection");
    tree.SelectText(1,3);tree.EditKey(39,false,false);Check(Visual(tree,id).caret==3&&tree.SelectedText().empty(),"right collapses selection");
    tree.SelectText(3,1);tree.EditKey(8,false,false);Check(tree.Get(id)->text=="AB"&&Changes(tree)==1,"selection deletes full CJK/non-BMP characters once");tree.EditKey(8,false,false);Check(tree.Get(id)->text=="B","backspace codepoint");tree.EditKey(46,false,false);Check(tree.Get(id)->text.empty(),"delete codepoint");tree.TakeEvents();tree.EditKey(8,false,false);Check(Changes(tree)==0,"empty deletion emits no text change");
    tree.SetText(id,"A😀Z");tree.SelectText(2,1);tree.TakeEvents();Check(tree.SetComposition("中文",1),"set preview and inner composition caret");
    const auto before=tree.Get(id)->text;UiTextPresentation p;std::string error;Check(PrepareUiText(Visual(tree,id),p,error)&&p.displayText=="A中文Z"&&p.caret==2&&p.compositionFirst==1&&p.compositionLast==3,"preview replaces selection only for display");
    Check(tree.Get(id)->text==before&&Changes(tree)==0&&!tree.SetComposition("中",2)&&Visual(tree,id).composition=="中文","invalid composition atomic");tree.CancelComposition();Check(tree.Get(id)->text==before,"cancel preserves committed text");
    tree.SetComposition("中");tree.InsertText("中");Check(tree.Get(id)->text=="A中Z"&&Changes(tree)==1&&Visual(tree,id).composition.empty(),"result commits selected text once");
    tree.SetText(id,std::string(65536,'a'));Check(!tree.SetComposition("b")&&tree.Get(id)->text.size()==65536,"preview byte budget");tree.SelectText(65536,65535);Check(tree.SetComposition("b")&&!tree.SetComposition("😀"),"preview budget accounts for replacement byte width");
    const auto epoch=tree.InputEpoch();tree.SetText(id,"reset");Check(tree.InputEpoch()!=epoch&&Visual(tree,id).composition.empty(),"external text replacement invalidates pending input");
}
void Metrics()
{
    FontCache fonts;TextLayout layout;std::string error;Check(fonts.Layout("A中😀BC",20,600,layout,error),"mixed text layout");
    const auto range=layout.Selection(1,3);Check(!range.empty(),"DirectWrite selection metrics");const auto left=layout.Caret(1),right=layout.Caret(3);float x=1e6f,end=0;
    for(const auto r:range){x=std::min(x,r.x);end=std::max(end,r.x+r.width);}Check(std::abs(x-left.x)<.1f&&std::abs(end-right.x)<.1f,"selection maps UTF32 to complete UTF16 clusters");
    UiTree tree;const auto id=Field(tree,10,"A中😀BC");tree.Layout(640,480);tree.Focus(id);tree.SelectText(3,1);UiTextPresentation p;Check(PrepareUiText(Visual(tree,id),p,error),"shared presentation");
    Check(p.content.x==14&&p.content.y==14&&p.content.width==212,"one shared text padding");Check(fonts.Layout(p.displayText,p.fontSize,p.content.width,layout,error),"shared layout");const auto view=PlaceUiText(p,layout);
    const auto c=layout.Caret(2);Check(view.HitTest(layout,{view.origin.x+c.x,view.origin.y+c.y+c.height*.5f})==2,"shared mouse/caret origin");
    TextLayout dpi;Check(fonts.Layout(p.displayText,p.fontSize*2,p.content.width*2,dpi,error),"DPI text metrics");const auto scaled=PlaceUiText(p,dpi,{2,2});Check(std::abs(view.caret.x-scaled.caret.x)<1&&std::abs(view.caret.y-scaled.caret.y)<1,"2x raster shares logical caret");
    tree.SetText(id,std::string(300,'W'));p={};Check(PrepareUiText(Visual(tree,id),p,error)&&fonts.Layout(p.displayText,p.fontSize,p.content.width,layout,error),"long wrapped text");const auto scrolled=PlaceUiText(p,layout);Check(scrolled.caret.y>=p.content.y-.1f&&scrolled.caret.y+scrolled.caret.height<=p.content.y+p.content.height+.1f&&scrolled.origin.y<p.content.y,"focused long text scrolls caret into clip");
}
bool Platform()
{
    UiTree tree;const auto first=Field(tree,10,"A中😀B"),second=Field(tree,100,"");tree.Layout(640,480);tree.Focus(first);
    WindowState window;window.postQuitOnDestroy=false;Check(CreateEngineWindow(window,false)&&SetEngineWindowClientSize(window,640,480),"IME hidden window");
    struct WindowScope{WindowState& window;~WindowScope(){window.messageHandler={};DestroyEngineWindow(window);}} ws{window};
    std::string clipboard="keep";bool allowWrite=true;
    WindowsUiInput input(tree,{[&](HWND,std::string_view value){if(!allowWrite)return false;clipboard=value;return true;},[&](HWND){return std::optional<std::string>{clipboard};}});
    window.messageHandler=[&](HWND w,UINT m,WPARAM p,LPARAM l){return input.Process(w,m,p,l);};
    input.Process(window.hwnd,WM_CHAR,0xd83d,0);tree.Focus(second);input.Process(window.hwnd,WM_CHAR,0xde00,0);Check(tree.Get(second)->text.empty(),"surrogate half never leaks to next field");
    input.Process(window.hwnd,WM_CHAR,0xd83d,0);input.Process(window.hwnd,WM_CHAR,0xde00,0);Check(tree.Get(second)->text=="😀","UTF16 pair commits one codepoint");
    tree.SetText(second,"");input.Process(window.hwnd,WM_IME_STARTCOMPOSITION,0,0);tree.TakeEvents();Check(input.UpdateIme({std::nullopt,std::string("拼音😀"),2,false}),"deterministic IMM adapter preview");input.Process(window.hwnd,WM_CHAR,'a',0);
    Check(tree.Get(second)->text.empty()&&Visual(tree,second).composition=="拼音😀"&&Changes(tree)==0,"composition characters are preview only");input.PositionIme(window.hwnd);Check(input.ImeCaret().has_value(),"candidate uses shared visual caret");
    const auto anchor=*input.ImeCaret();Check(anchor.x>=14&&anchor.x<226&&anchor.y>=104&&anchor.y+anchor.height<=156.1f,"candidate anchor inside input content");bool candidateQueried=false;
    if(HIMC context=ImmGetContext(window.hwnd)){CANDIDATEFORM form{};if(ImmGetCandidateWindow(context,0,&form)){candidateQueried=true;Check(std::abs(form.ptCurrentPos.x-anchor.x)<=1&&std::abs(form.ptCurrentPos.y-anchor.y-anchor.height)<=1,"actual IMM candidate form follows shared caret");}ImmReleaseContext(window.hwnd,context);}
    Check(input.UpdateIme({std::string("中😀"),std::nullopt}),"IME result accepted");input.Process(window.hwnd,WM_IME_CHAR,0x4e2d,0);input.Process(window.hwnd,WM_IME_ENDCOMPOSITION,0,0);Check(tree.Get(second)->text=="中😀"&&Changes(tree)==1,"IME_CHAR does not duplicate result");
    input.Process(window.hwnd,WM_IME_STARTCOMPOSITION,0,0);input.UpdateIme({std::nullopt,std::string("cancel")});input.Process(window.hwnd,WM_IME_ENDCOMPOSITION,0,0);Check(tree.Get(second)->text=="中😀"&&Visual(tree,second).composition.empty(),"end/cancel preserves committed value");
    input.Process(window.hwnd,WM_IME_STARTCOMPOSITION,0,0);input.UpdateIme({std::nullopt,std::string("old")});tree.Remove(second);tree.Focus(first);input.PositionIme(window.hwnd);Check(!input.UpdateIme({std::string("stale"),std::nullopt})&&tree.Get(first)->text=="A中😀B","deleted composition cannot commit into replacement focus");
    std::array<BYTE,256> saved{},keys{};Check(GetKeyboardState(saved.data())!=FALSE,"save thread key modifiers");struct KeyScope{std::array<BYTE,256>& saved;~KeyScope(){SetKeyboardState(saved.data());}} ks{saved};keys=saved;keys[VK_CONTROL]=0x80;Check(SetKeyboardState(keys.data())!=FALSE,"set only test thread Ctrl state");
    tree.SelectText(3,1);input.Process(window.hwnd,WM_KEYDOWN,'C',0);Check(clipboard=="中😀"&&tree.Get(first)->text=="A中😀B","copy preserves Unicode");
    allowWrite=false;input.Process(window.hwnd,WM_KEYDOWN,'X',0);Check(tree.Get(first)->text=="A中😀B","failed copy cannot cut");allowWrite=true;input.Process(window.hwnd,WM_KEYDOWN,'X',0);Check(tree.Get(first)->text=="AB","cut removes selected codepoints");input.Process(window.hwnd,WM_KEYDOWN,'V',0);Check(tree.Get(first)->text=="A中😀B","paste restores Unicode");
    tree.SelectText(0,0);clipboard="keep";input.Process(window.hwnd,WM_KEYDOWN,'C',0);Check(clipboard=="keep","empty selection does not clear clipboard");window.messageHandler={};return candidateQueried;
}
}
int main(int argc,char** argv)
{
    try{Editing();Metrics();const bool imm=Platform();if(argc>1){const auto path=std::filesystem::path(argv[1]);std::filesystem::create_directories(path.parent_path());std::ofstream report(path);report<<"{\"success\":true,\"codepoints\":true,\"previewIsolated\":true,\"oneCommit\":true,\"focusEpoch\":true,\"sharedMetrics\":true,\"clipboardProvider\":true,\"systemClipboardModified\":false,\"immCandidateQueried\":"<<(imm?"true":"false")<<",\"manualImeVerified\":false}\n";}std::cout<<"UiTextTests passed (UTF32/UTF16, selection, preview/result/cancel, caret/DPI/scroll, focus and clipboard).\n";return 0;}catch(const std::exception& error){std::cerr<<error.what()<<'\n';return 1;}
}
