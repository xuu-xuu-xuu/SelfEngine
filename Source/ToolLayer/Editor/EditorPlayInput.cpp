#include "EditorPlayInput.h"
#include <imgui/imgui_internal.h>
#include <windowsx.h>
#include <algorithm>
#include <cmath>

namespace se
{
namespace
{
struct ContextScope
{
    ImGuiContext* previous=ImGui::GetCurrentContext();
    explicit ContextScope(ImGuiContext* context){ImGui::SetCurrentContext(context);}
    ~ContextScope(){ImGui::SetCurrentContext(previous);}
};
bool Keyboard(UINT message)
{
    return (message>=WM_KEYFIRST&&message<=WM_KEYLAST)||message==WM_UNICHAR
        ||message==WM_IME_SETCONTEXT||message==WM_IME_STARTCOMPOSITION||message==WM_IME_ENDCOMPOSITION
        ||message==WM_IME_COMPOSITION||message==WM_IME_CHAR||message==WM_IME_KEYDOWN||message==WM_IME_KEYUP;
}
int Button(UINT message)
{
    if(message==WM_LBUTTONDOWN||message==WM_LBUTTONUP||message==WM_LBUTTONDBLCLK)return 0;
    if(message==WM_RBUTTONDOWN||message==WM_RBUTTONUP||message==WM_RBUTTONDBLCLK)return 1;
    if(message==WM_MBUTTONDOWN||message==WM_MBUTTONUP||message==WM_MBUTTONDBLCLK)return 2;
    return -1;
}
bool Down(UINT message)
{return message==WM_LBUTTONDOWN||message==WM_LBUTTONDBLCLK||message==WM_RBUTTONDOWN||message==WM_RBUTTONDBLCLK||message==WM_MBUTTONDOWN||message==WM_MBUTTONDBLCLK;}
}
bool EditorPlayInput::Held() const {return std::any_of(held_.begin(),held_.end(),[](bool value){return value;});}
void EditorPlayInput::Attach(UiTree* ui)
{
    if(ui==ui_)return;
    Deactivate(window_);input_.reset();ui_=ui;
    if(ui_){input_=std::make_unique<WindowsUiInput>(*ui_);input_->SetViewport(viewport_);}
}
void EditorPlayInput::Deactivate(HWND window)
{
    if(input_&&ui_)
    {
        if(window&&IsWindow(window))input_->Process(window,WM_KILLFOCUS,0,0);
        else ui_->CancelInput();
    }
    active_=over_=false;held_.fill(false);
}
bool EditorPlayInput::OwnsPoint(UiPoint point) const
{
    if(!context_||!ui_||!viewport_.Contains(point))return false;
    ContextScope context(context_);
    auto* viewport=ImGui::FindWindowByName("###EditorUi.Viewport");
    if(!viewport||viewport->Collapsed||!viewport->Active||context_->OpenPopupStack.Size)return false;
    if(context_->MovingWindow)return false;
    if(context_->ActiveId&&context_->ActiveIdWindow&&context_->ActiveIdWindow->RootWindow!=viewport->RootWindow
        &&context_->IO.MouseDown[0])return false;
    ImGuiWindow* hovered=nullptr;
    ImGui::FindHoveredWindowEx({point.x,point.y},true,&hovered,nullptr);
    return hovered&&hovered->RootWindow==viewport->RootWindow;
}
void EditorPlayInput::SetViewport(HWND window,UiRect viewport)
{
    window_=window;viewport_=viewport;
    if(input_){input_->SetViewport(viewport);input_->SynchronizeCapture(window);}
    over_=OwnsPoint(pointer_);
    if(active_)
    {
        ContextScope context(context_);
        auto* owner=ImGui::FindWindowByName("###EditorUi.Viewport");
        if(!owner||owner->Collapsed||!owner->Active||context_->OpenPopupStack.Size
            ||(context_->NavWindow&&context_->NavWindow->RootWindow!=owner->RootWindow))Deactivate(window);
        else if(input_)input_->PositionIme(window);
    }
}
EditorInputRoute EditorPlayInput::Process(HWND window,UINT message,WPARAM wParam,LPARAM lParam)
{
    EditorInputRoute result;
    if(!ui_||!input_)return result;
    window_=window;
    if(message==WM_KILLFOCUS){Deactivate(window);return result;}
    if(message==WM_CAPTURECHANGED)
    {held_.fill(false);input_->Process(window,message,wParam,lParam);return result;}
    if(Keyboard(message)&&active_)
    {
        result.forwardEditor=false;result.handled=input_->Process(window,message,wParam,lParam);return result;
    }
    const int button=Button(message);
    const bool pointerMessage=button>=0||message==WM_MOUSEMOVE||message==WM_MOUSEWHEEL||message==WM_MOUSEHWHEEL;
    if(!pointerMessage)return result;
    POINT point{GET_X_LPARAM(lParam),GET_Y_LPARAM(lParam)};
    if(message==WM_MOUSEWHEEL||message==WM_MOUSEHWHEEL)ScreenToClient(window,&point);
    pointer_={float(point.x),float(point.y)};over_=OwnsPoint(pointer_);
    if(button>=0&&Down(message))
    {
        if(!over_){Deactivate(window);return result;}
        if(!active_)
        {
            active_=true;
            ContextScope context(context_);
            ImGui::ClearActiveID();ImGui::GetIO().ClearInputKeys();ImGui::GetIO().InputQueueSurrogate=0;
            // 同一消息批次在焦点切换前已排队的编辑器文字/键不能留到下一帧；鼠标/焦点仍需由 ImGui 处理。
            for(int index=context_->InputEventsQueue.Size-1;index>=0;--index)
                if(context_->InputEventsQueue[index].Type==ImGuiInputEventType_Key||context_->InputEventsQueue[index].Type==ImGuiInputEventType_Text)
                    context_->InputEventsQueue.erase(context_->InputEventsQueue.Data+index);
            ImGui::FocusWindow(ImGui::FindWindowByName("###EditorUi.Viewport"));
        }
        held_[static_cast<std::size_t>(button)]=true;
    }
    if(active_&&(over_||Held()))
    {
        // ImGui 仍观察鼠标以维护窗口焦点；内容区在 Play 时不会触发编辑器拾取/手柄。
        const UINT routedMessage=message==WM_LBUTTONDBLCLK?WM_LBUTTONDOWN:message;
        result.handled=input_->Process(window,routedMessage,wParam,lParam);
    }
    else if(active_&&message==WM_MOUSEMOVE)ui_->PointerMove({-100000,-100000});
    if(button>=0&&!Down(message))held_[static_cast<std::size_t>(button)]=false;
    return result;
}
bool EditorPlayInput::KeyboardConsumed() const {return !active_||!input_||input_->KeyboardConsumed();}
bool EditorPlayInput::PointerConsumed() const {return !active_||!input_||(!over_&&!Held())||input_->PointerConsumed();}
InputState EditorPlayInput::GameplayInput(const InputState& source) const
{
    InputState result=source;
    const int x=static_cast<int>(std::lround(viewport_.x)),y=static_cast<int>(std::lround(viewport_.y));
    result.mouseX-=x;result.mouseY-=y;for(auto& event:result.pointerEvents){event.x-=x;event.y-=y;}
    if(KeyboardConsumed()){result.keysDown.fill(false);result.keyEvents.clear();result.keyEvents.push_back({KeyEventType::Reset,0});result.textEvents.clear();}
    if(PointerConsumed()){result.isLeftDown=false;result.mouseButtons.fill(false);result.pointerEvents.clear();}
    return result;
}
void EditorPlayInput::EndFrame(){if(input_)input_->EndFrame();}
}
