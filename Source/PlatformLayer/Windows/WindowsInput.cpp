#include "WindowsInput.h"
#include <windowsx.h>
#include <algorithm>

InputMessageResult ProcessWindowsInput(HWND hwnd,UINT message,WPARAM wParam,LPARAM lParam,InputState& input)
{
    try
    {
        switch(message)
        {
        case WM_KEYDOWN:case WM_SYSKEYDOWN:case WM_KEYUP:case WM_SYSKEYUP:
        {
            if(wParam>=input.keysDown.size())break;
            const bool down=message==WM_KEYDOWN||message==WM_SYSKEYDOWN;
            if(input.keysDown[wParam]==down)break;
            input.keysDown[wParam]=down;
            input.keyEvents.push_back({down?KeyEventType::Pressed:KeyEventType::Released,static_cast<unsigned>(wParam)});
            if(down&&wParam==VK_ESCAPE&&GetCapture()==hwnd)ReleaseCapture();
            break; // 保留 Alt+F4 等默认系统行为。
        }
        case WM_LBUTTONDOWN:case WM_RBUTTONDOWN:case WM_MBUTTONDOWN:
        case WM_LBUTTONUP:case WM_RBUTTONUP:case WM_MBUTTONUP:
        {
            const bool down=message==WM_LBUTTONDOWN||message==WM_RBUTTONDOWN||message==WM_MBUTTONDOWN;
            const unsigned button=message==WM_LBUTTONDOWN||message==WM_LBUTTONUP?0u:message==WM_RBUTTONDOWN||message==WM_RBUTTONUP?1u:2u;
            input.mouseX=GET_X_LPARAM(lParam);input.mouseY=GET_Y_LPARAM(lParam);
            input.mouseButtons[button]=down;input.isLeftDown=input.mouseButtons[0];
            input.pointerEvents.push_back({down?PointerEventType::Down:PointerEventType::Up,input.mouseX,input.mouseY,button,0});
            if(down&&GetCapture()!=hwnd)SetCapture(hwnd);
            else if(GetCapture()==hwnd&&std::none_of(input.mouseButtons.begin(),input.mouseButtons.end(),[](bool value){return value;}))ReleaseCapture();
            return InputMessageResult::Handled;
        }
        case WM_MOUSEMOVE:
            input.mouseX=GET_X_LPARAM(lParam);input.mouseY=GET_Y_LPARAM(lParam);
            input.pointerEvents.push_back({PointerEventType::Move,input.mouseX,input.mouseY,0,0});return InputMessageResult::Handled;
        case WM_MOUSEWHEEL:
        {
            POINT point{GET_X_LPARAM(lParam),GET_Y_LPARAM(lParam)};ScreenToClient(hwnd,&point);
            input.pointerEvents.push_back({PointerEventType::Wheel,point.x,point.y,0,float(GET_WHEEL_DELTA_WPARAM(wParam))/WHEEL_DELTA});return InputMessageResult::Handled;
        }
        case WM_CAPTURECHANGED:case WM_CANCELMODE:
            input.mouseButtons.fill(false);input.isLeftDown=false;
            input.pointerEvents.push_back({PointerEventType::Cancel,input.mouseX,input.mouseY,0,0});
            if(message==WM_CANCELMODE&&GetCapture()==hwnd)ReleaseCapture();
            return InputMessageResult::Handled;
        case WM_CHAR:
            input.textEvents.push_back({static_cast<char16_t>(wParam)});return InputMessageResult::Handled;
        case WM_KILLFOCUS:
            input.hasFocus=false;
            input.keysDown.fill(false);input.mouseButtons.fill(false);input.isLeftDown=false;
            input.keyEvents.push_back({KeyEventType::Reset,0});input.pointerEvents.push_back({PointerEventType::Cancel,input.mouseX,input.mouseY,0,0});
            if(GetCapture()==hwnd)ReleaseCapture();return InputMessageResult::Handled;
        case WM_SETFOCUS:input.hasFocus=true;return InputMessageResult::Handled;
        }
    }
    catch(...){OutputDebugStringW(L"Input event queue failed.\n");}
    return InputMessageResult::UseDefault;
}
