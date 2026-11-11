#include "WindowsUiInput.h"
#include "CoreLayer/Utf.h"
#include "FunctionLayer/GameUI/UiTextPresentation.h"
#include <imm.h>
#include <windowsx.h>
#include <algorithm>
#include <cstring>
#include <cmath>

namespace se
{
namespace
{
std::string ReadComposition(HIMC context,DWORD index)
{
    const LONG size=ImmGetCompositionStringW(context,index,nullptr,0);
    if(size<0||size>131072||size%2)return {};
    std::u16string text(static_cast<std::size_t>(size)/2,u'\0');
    if(ImmGetCompositionStringW(context,index,text.data(),static_cast<DWORD>(size))!=size)return {};
    std::u32string decoded;std::string result;
    if(DecodeUtf16(text,decoded))EncodeUtf8(decoded,result);
    return result;
}
std::size_t CompositionCursor(HIMC context,std::string_view utf8)
{
    const LONG position=ImmGetCompositionStringW(context,GCS_CURSORPOS,nullptr,0);std::u32string text;
    if(position<0||!DecodeUtf8(utf8,text))return std::string::npos;
    std::size_t index=0;LONG units=0;for(const auto c:text){const LONG count=c>0xffff?2:1;if(units+count>position)break;units+=count;++index;}return index;
}
bool WriteClipboard(HWND window,std::string_view utf8)
{
    std::u32string decoded;if(!DecodeUtf8(utf8,decoded))return false;
    const auto text=EncodeUtf16(decoded);const SIZE_T bytes=(text.size()+1)*sizeof(char16_t);
    HGLOBAL block=GlobalAlloc(GMEM_MOVEABLE,bytes);if(!block)return false;
    auto* target=static_cast<char16_t*>(GlobalLock(block));if(!target){GlobalFree(block);return false;}
    std::memcpy(target,text.data(),text.size()*2);target[text.size()]=0;GlobalUnlock(block);
    if(!OpenClipboard(window)){GlobalFree(block);return false;}
    const bool success=EmptyClipboard()&&SetClipboardData(CF_UNICODETEXT,block)!=nullptr;
    CloseClipboard();if(!success)GlobalFree(block);return success;
}
std::optional<std::string> ReadClipboard(HWND window)
{
    std::optional<std::string> result;if(!OpenClipboard(window))return result;
    const auto block=GetClipboardData(CF_UNICODETEXT);
    if(block)
    {
        const SIZE_T count=std::min<SIZE_T>(GlobalSize(block)/2,65537);
        if(const auto* text=static_cast<const char16_t*>(GlobalLock(block)))
        {
            const auto end=std::find(text,text+count,u'\0');std::u32string decoded;
            if(end!=text+count&&DecodeUtf16({text,static_cast<std::size_t>(end-text)},decoded))
            {std::string encoded;if(EncodeUtf8(decoded,encoded))result=std::move(encoded);}
            GlobalUnlock(block);
        }
    }
    CloseClipboard();return result;
}
}
void WindowsUiInput::ReleaseOwnedCapture()
{
    const bool owned=captureOwned_;captureOwned_=false;
    if(owned&&window_&&GetCapture()==window_)ReleaseCapture();
}
WindowsUiInput::~WindowsUiInput(){ReleaseOwnedCapture();tree_.CancelInput();}
void WindowsUiInput::SynchronizeCapture(HWND window)
{
    if(window_!=window){ReleaseOwnedCapture();window_=window;trackingMouse_=false;}
    if(!captureOwned_)return;
    if(!tree_.Captured()){selecting_=false;ReleaseOwnedCapture();}
    else if(GetCapture()!=window_){captureOwned_=false;selecting_=false;tree_.CancelPointerInput();}
}
void WindowsUiInput::SynchronizeTextInput(HWND window)
{
    const auto epoch=tree_.InputEpoch();if(epoch==observedInputEpoch_)return;
    observedInputEpoch_=epoch;highSurrogate_=0;selecting_=false;selectionAnchor_=0;imeCaret_.reset();
    const bool cancel=compositionActive_;compositionActive_=false;
    if(cancel&&window)if(HIMC context=ImmGetContext(window)){ImmNotifyIME(context,NI_COMPOSITIONSTR,CPS_CANCEL,0);ImmReleaseContext(window,context);}
}
bool WindowsUiInput::TextFocused() const
{ const auto* n=tree_.Get(tree_.Focused());return n&&n->kind==UiKind::TextInput; }
bool WindowsUiInput::InsertUnit(char16_t unit)
{
    if(unit>=0xD800&&unit<=0xDBFF){highSurrogate_=unit;return true;}
    std::u16string text;if(highSurrogate_){text.push_back(highSurrogate_);highSurrogate_=0;}text.push_back(unit);
    std::u32string decoded;std::string encoded;
    return DecodeUtf16(text,decoded)&&EncodeUtf8(decoded,encoded)&&tree_.InsertText(encoded);
}
void WindowsUiInput::PlaceCaret(UiPoint point,bool extend,bool newlyFocused)
{
    auto visuals=tree_.Visuals();auto found=std::find_if(visuals.begin(),visuals.end(),[&](const auto& n){return n.id==tree_.Focused()&&n.kind==UiKind::TextInput;});if(found==visuals.end())return;
    if(newlyFocused)found->focused=false;
    UiTextPresentation text;TextLayout layout;std::string error;
    if(!PrepareUiText(*found,text,error)||!fonts_.Layout(text.displayText,text.fontSize,text.content.width,layout,error))return;
    const auto index=PlaceUiText(text,layout).HitTest(layout,point);
    if(!extend)selectionAnchor_=index;
    tree_.SelectText(index,selectionAnchor_);
}
void WindowsUiInput::PositionIme(HWND window)
{
    SynchronizeTextInput(window);
    if(!TextFocused())
    {
        if(compositionActive_)
        {
            compositionActive_=false;
            if(HIMC context=ImmGetContext(window)){ImmNotifyIME(context,NI_COMPOSITIONSTR,CPS_CANCEL,0);ImmReleaseContext(window,context);}
        }
        imeCaret_.reset();return;
    }
    auto visuals=tree_.Visuals();const auto found=std::find_if(visuals.begin(),visuals.end(),[&](const auto& n){return n.id==tree_.Focused();});
    if(found==visuals.end())return;
    UiTextPresentation text;TextLayout layout;std::string error;
    if(!PrepareUiText(*found,text,error)||!fonts_.Layout(text.displayText,text.fontSize,text.content.width,layout,error))return;
    const auto view=PlaceUiText(text,layout);if(view.clip.width<=0||view.clip.height<=0){imeCaret_.reset();return;}
    auto caret=view.caret;caret.width=std::min(caret.width,view.clip.width);caret.height=std::min(caret.height,view.clip.height);
    caret.x=std::clamp(caret.x,view.clip.x,view.clip.x+view.clip.width-caret.width)+(viewport_?viewport_->x:0);
    caret.y=std::clamp(caret.y,view.clip.y,view.clip.y+view.clip.height-caret.height)+(viewport_?viewport_->y:0);imeCaret_=caret;
    const POINT position{static_cast<LONG>(std::lround(caret.x)),static_cast<LONG>(std::lround(caret.y+caret.height))};
    if(HIMC context=ImmGetContext(window))
    {
        COMPOSITIONFORM composition{};composition.dwStyle=CFS_POINT;composition.ptCurrentPos=position;ImmSetCompositionWindow(context,&composition);
        CANDIDATEFORM candidate{};candidate.dwStyle=CFS_CANDIDATEPOS;candidate.ptCurrentPos=position;ImmSetCandidateWindow(context,&candidate);
        ImmReleaseContext(window,context);
    }
}
void WindowsUiInput::SetViewport(UiRect viewport)
{
    if(!std::isfinite(viewport.x)||!std::isfinite(viewport.y)||!std::isfinite(viewport.width)||!std::isfinite(viewport.height)
        ||viewport.width<0||viewport.height<0)return;
    viewport_=viewport;tree_.Layout(viewport.width,viewport.height);
}
bool WindowsUiInput::UpdateIme(const UiImeUpdate& update)
{
    SynchronizeTextInput(window_);if(!TextFocused()||!compositionActive_)return false;
    bool success=true;
    if(update.result){if(!update.result->empty())success=tree_.InsertText(*update.result);else tree_.CancelComposition();}
    if(update.composition)success=tree_.SetComposition(*update.composition,update.caret)&&success;
    if(update.cancel)tree_.CancelComposition();return success;
}
std::optional<LRESULT> WindowsUiInput::Process(HWND window,UINT message,WPARAM wParam,LPARAM lParam)
{
    SynchronizeCapture(window);
    SynchronizeTextInput(window);
    if(tree_.ConsumesKeyboard() && ((message>=WM_KEYFIRST&&message<=WM_KEYLAST)
        ||message==WM_IME_COMPOSITION||message==WM_KILLFOCUS))keyboardConsumed_=true;
    const UiPoint point{float(GET_X_LPARAM(lParam))-(viewport_?viewport_->x:0),float(GET_Y_LPARAM(lParam))-(viewport_?viewport_->y:0)};
    switch(message)
    {
    case WM_SIZE: if(viewport_)tree_.Layout(viewport_->width,viewport_->height);else tree_.Layout(float(LOWORD(lParam)),float(HIWORD(lParam)));break;
    case WM_MOUSEMOVE:
        if(!trackingMouse_){TRACKMOUSEEVENT tracking{sizeof(TRACKMOUSEEVENT),TME_LEAVE,window,0};trackingMouse_=TrackMouseEvent(&tracking)!=FALSE;}
        if(selecting_&&TextFocused()&&tree_.Captured()==tree_.Focused())PlaceCaret(point,true);
        if(tree_.PointerMove(point)||selecting_){pointerConsumed_=true;return 0;}
        break;
    case WM_LBUTTONDOWN:
    {const auto previousFocus=tree_.Focused();
        gestureConsumed_=tree_.PointerDown(point);
        if(gestureConsumed_){pointerConsumed_=true;if(IsWindowVisible(window))SetFocus(window);if(GetCapture()!=window)SetCapture(window);captureOwned_=GetCapture()==window;SynchronizeTextInput(window);
            if(compositionActive_){compositionActive_=false;tree_.CancelComposition();if(HIMC context=ImmGetContext(window)){ImmNotifyIME(context,NI_COMPOSITIONSTR,CPS_CANCEL,0);ImmReleaseContext(window,context);}}
            selecting_=TextFocused();PlaceCaret(point,false,previousFocus!=tree_.Focused());PositionIme(window);return 0;}break;}
    case WM_RBUTTONDOWN:case WM_RBUTTONUP:case WM_MBUTTONDOWN:case WM_MBUTTONUP:
        if(tree_.PointerMove(point)){pointerConsumed_=true;return 0;}break;
    case WM_LBUTTONUP:
    {const bool consumed=tree_.PointerUp(point)||gestureConsumed_;gestureConsumed_=false;selecting_=false;ReleaseOwnedCapture();if(consumed){pointerConsumed_=true;return 0;}break;}
    case WM_MOUSELEAVE:trackingMouse_=false;pointerConsumed_=tree_.PointerLeave()||pointerConsumed_;break;
    case WM_MOUSEWHEEL:
    {POINT client{GET_X_LPARAM(lParam),GET_Y_LPARAM(lParam)};ScreenToClient(window,&client);if(tree_.Wheel({float(client.x)-(viewport_?viewport_->x:0),float(client.y)-(viewport_?viewport_->y:0)},float(GET_WHEEL_DELTA_WPARAM(wParam))/WHEEL_DELTA)){pointerConsumed_=true;return 0;}break;}
    case WM_CAPTURECHANGED:
    {const bool owned=captureOwned_;captureOwned_=false;selecting_=false;if(owned||tree_.Captured())tree_.CancelPointerInput();break;}
    case WM_CANCELMODE:selecting_=false;tree_.CancelPointerInput();ReleaseOwnedCapture();break;
    case WM_KILLFOCUS:
        if(HIMC context=ImmGetContext(window)){ImmNotifyIME(context,NI_COMPOSITIONSTR,CPS_CANCEL,0);ImmReleaseContext(window,context);}
        highSurrogate_=0;selecting_=false;compositionActive_=false;trackingMouse_=false;
        pointerConsumed_=gestureConsumed_||tree_.HasPointerInput()||pointerConsumed_;gestureConsumed_=false;tree_.CancelInput();ReleaseOwnedCapture();break;
    case WM_KEYDOWN:
        if(compositionActive_)break; // 编辑候选/组合文字的按键交给 IME。
        if(tree_.ConsumesKeyboard())
        {
            const bool control=(GetKeyState(VK_CONTROL)&0x8000)!=0,shift=(GetKeyState(VK_SHIFT)&0x8000)!=0;
            if(control&&TextFocused())
            {
                if(wParam=='C'||wParam=='X'){const auto selected=tree_.SelectedText();if(!selected.empty()&&(clipboard_.write?clipboard_.write(window,selected):WriteClipboard(window,selected))&&wParam=='X')tree_.InsertText("");return 0;}
                if(wParam=='V'){if(auto text=clipboard_.read?clipboard_.read(window):ReadClipboard(window))tree_.InsertText(*text);return 0;}
            }
            if(tree_.EditKey(static_cast<unsigned>(wParam),shift,control)){PositionIme(window);return 0;}
        }
        break;
    case WM_CHAR:
        if(TextFocused()){if(!compositionActive_&&wParam>=32&&wParam!=127)InsertUnit(static_cast<char16_t>(wParam));PositionIme(window);return 0;}break;
    case WM_UNICHAR:
        if(wParam==UNICODE_NOCHAR)return TRUE;
        if(TextFocused()){std::string text;if(!compositionActive_&&EncodeUtf8(std::u32string(1,static_cast<char32_t>(wParam)),text))tree_.InsertText(text);return 0;}break;
    case WM_IME_SETCONTEXT:
        if(TextFocused())return DefWindowProcW(window,message,wParam,lParam&~ISC_SHOWUICOMPOSITIONWINDOW);
        break;
    case WM_IME_STARTCOMPOSITION: if(TextFocused()){highSurrogate_=0;selecting_=false;compositionActive_=true;tree_.CancelComposition();PositionIme(window);return 0;}break;
    case WM_IME_COMPOSITION:
        if(TextFocused())
        {
            if(!compositionActive_)return 0;
            if(HIMC context=ImmGetContext(window))
            {
                UiImeUpdate update;
                if(lParam&GCS_RESULTSTR)update.result=ReadComposition(context,GCS_RESULTSTR);
                if(lParam&(GCS_COMPSTR|GCS_CURSORPOS)){update.composition=ReadComposition(context,GCS_COMPSTR);update.caret=CompositionCursor(context,*update.composition);}
                update.cancel=lParam==0;UpdateIme(update);
                ImmReleaseContext(window,context);
            }
            // 已读取提交文本，不再调用 DefWindowProc 生成重复的 WM_IME_CHAR。
            return 0;
        }
        break;
    case WM_IME_ENDCOMPOSITION:
    {const bool active=compositionActive_;compositionActive_=false;if(TextFocused()){if(active)tree_.CancelComposition();return 0;}break;}
    case WM_IME_CHAR:if(TextFocused())return 0;break;
    }
    return std::nullopt;
}
}
