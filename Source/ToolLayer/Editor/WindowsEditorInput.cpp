#include "WindowsEditorInput.h"
#include <imgui/backends/imgui_impl_win32.h>
#include <shellapi.h>

extern IMGUI_IMPL_API LRESULT ImGui_ImplWin32_WndProcHandler(HWND,UINT,WPARAM,LPARAM);

namespace se
{
namespace
{
struct ContextScope
{
    ImGuiContext* previous=ImGui::GetCurrentContext();
    explicit ContextScope(ImGuiContext* next){ImGui::SetCurrentContext(next);}
    ~ContextScope(){ImGui::SetCurrentContext(previous);}
};
bool MouseMessage(UINT message)
{
    return message==WM_MOUSEMOVE||message==WM_NCMOUSEMOVE||message==WM_MOUSEWHEEL||message==WM_MOUSEHWHEEL
        || (message>=WM_LBUTTONDOWN&&message<=WM_MBUTTONDBLCLK)
        || (message>=WM_XBUTTONDOWN&&message<=WM_XBUTTONDBLCLK);
}
bool KeyboardMessage(UINT message)
{
    return message==WM_KEYDOWN||message==WM_KEYUP||message==WM_SYSKEYDOWN||message==WM_SYSKEYUP
        || message==WM_CHAR||message==WM_UNICHAR||message==WM_SYSCHAR;
}
}

void WindowsEditorInput::EnableDpiAwareness(){ImGui_ImplWin32_EnableDpiAwareness();}
WindowsEditorInput::WindowsEditorInput(HWND window,ImGuiEditor& editor):window_(window),editor_(&editor),context_(editor.Context())
{
    if(!IsWindow(window_)||!IsWindowUnicode(window_)){error_="EditorUi.InvalidWindow";return;}
    ContextScope context(context_);
    if(ImGui::GetIO().BackendPlatformUserData||!ImGui_ImplWin32_Init(window_))
    {error_="EditorUi.PlatformInitFailed";return;}
    editor.SetDpiScale(ImGui_ImplWin32_GetDpiScaleForHwnd(window_));DragAcceptFiles(window_,TRUE);ready_=true;
}
WindowsEditorInput::~WindowsEditorInput()
{
    if(ready_){DragAcceptFiles(window_,FALSE);ContextScope context(context_);ImGui_ImplWin32_Shutdown();}
}
std::optional<LRESULT> WindowsEditorInput::Process(HWND window,UINT message,WPARAM wParam,LPARAM lParam)
{
    if(!ready_||window!=window_)return {};
    ContextScope context(context_);
    if(message==WM_DPICHANGED)
    {
        editor_->SetDpiScale(float(HIWORD(wParam))/96.0f);
        if(lParam)
        {const auto* bounds=reinterpret_cast<const RECT*>(lParam);SetWindowPos(window,nullptr,bounds->left,bounds->top,bounds->right-bounds->left,bounds->bottom-bounds->top,SWP_NOZORDER|SWP_NOACTIVATE);}
        return 0;
    }
    if(message==WM_DROPFILES)
    {
        const auto drop=reinterpret_cast<HDROP>(wParam);
        const auto count=DragQueryFileW(drop,0xffffffff,nullptr,0);
        std::vector<std::filesystem::path> paths;paths.reserve(count);
        for(UINT i=0;i<count;++i)
        {const auto size=DragQueryFileW(drop,i,nullptr,0);std::wstring path(size+1,L'\0');if(DragQueryFileW(drop,i,path.data(),size+1)){path.resize(size);paths.emplace_back(path);}}
        DragFinish(drop);editor_->QueueAssetImport(std::move(paths));return 0;
    }
    if(message==WM_UNICHAR)
    {
        if(wParam==UNICODE_NOCHAR)return TRUE;
        if(wParam<=0x10ffff&&!(wParam>=0xd800&&wParam<=0xdfff))ImGui::GetIO().AddInputCharacter(static_cast<unsigned>(wParam));
        if(ImGui::GetIO().WantCaptureKeyboard||ImGui::GetIO().WantTextInput)return 0;
        return {};
    }
    if(message==WM_KILLFOCUS)ImGui::GetIO().InputQueueSurrogate=0;
    const auto result=ImGui_ImplWin32_WndProcHandler(window,message,wParam,lParam);
    if(result)return result;
    const auto& io=ImGui::GetIO();
    if((MouseMessage(message)&&io.WantCaptureMouse)||(KeyboardMessage(message)&&(io.WantCaptureKeyboard||io.WantTextInput)))return 0;
    return {};
}
bool WindowsEditorInput::PrepareFrame()
{
    if(!ready_||!IsWindow(window_)){error_="EditorUi.InvalidWindow";return false;}
    ContextScope context(context_);ImGui_ImplWin32_NewFrame();return true;
}
bool WindowsEditorInput::WantsMouse() const
{if(!ready_)return false;ContextScope context(context_);return ImGui::GetIO().WantCaptureMouse;}
bool WindowsEditorInput::WantsKeyboard() const
{if(!ready_)return false;ContextScope context(context_);return ImGui::GetIO().WantCaptureKeyboard||ImGui::GetIO().WantTextInput;}
}
