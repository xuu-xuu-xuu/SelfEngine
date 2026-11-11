#include "ToolLayer/Editor/WindowsEditorInput.h"
#include <imgui/imgui_internal.h>
#include <iostream>

#define CHECK(condition) do { if (!(condition)) { std::cerr << "FAIL line " << __LINE__ << ": " << #condition << '\n'; return 1; } } while (false)
int main()
{
    static_assert(sizeof(ImWchar)==4,"All ImGui consumers must use IMGUI_USE_WCHAR32");
    se::ImGuiEditor editor;
    se::WindowsEditorInput invalid(nullptr,editor);CHECK(!invalid.Ready());
    // 隐藏测试窗口，不抢焦点、不修改剪贴板，也不创建 GPU 设备。
    HWND window=CreateWindowExW(0,L"STATIC",L"SelfEngine input test",WS_POPUP,0,0,640,480,nullptr,nullptr,GetModuleHandleW(nullptr),nullptr);
    CHECK(window);
    {
        se::WindowsEditorInput input(window,editor);CHECK(input.Ready());
        auto* previous=ImGui::GetCurrentContext();ImGui::SetCurrentContext(editor.Context());
        CHECK(ImGui::GetPlatformIO().Platform_GetClipboardTextFn&&ImGui::GetPlatformIO().Platform_SetClipboardTextFn);
        CHECK(ImGui::GetPlatformIO().Platform_SetImeDataFn&&ImGui::GetMainViewport()->PlatformHandleRaw==window);
        auto& context=*editor.Context();context.InputEventsQueue.clear();
        input.Process(window,WM_CHAR,0xd83d,0);input.Process(window,WM_CHAR,0xde00,0);
        CHECK(context.InputEventsQueue.Size==1&&context.InputEventsQueue[0].Type==ImGuiInputEventType_Text);
        CHECK(context.InputEventsQueue[0].Text.Char==0x1f600);
        CHECK(input.Process(window,WM_UNICHAR,UNICODE_NOCHAR,0).value_or(0)==TRUE);
        context.InputEventsQueue.clear();
        input.Process(window,WM_UNICHAR,0x4e2d,0);
        CHECK(context.InputEventsQueue.Size==1&&context.InputEventsQueue[0].Text.Char==0x4e2d);
        context.InputEventsQueue.clear();
        input.Process(window,WM_KILLFOCUS,0,0);
        CHECK(context.InputEventsQueue.Size==1&&context.InputEventsQueue[0].Type==ImGuiInputEventType_Focus&&!context.InputEventsQueue[0].AppFocused.Focused);
        CHECK(input.PrepareFrame());CHECK(ImGui::GetIO().DisplaySize.x==640&&ImGui::GetIO().DisplaySize.y==480);
        ImGui::SetCurrentContext(previous);
    }
    DestroyWindow(window);
    std::cout<<"WindowsEditorInputTests passed (UTF16 pairs, Unicode, focus, IME/clipboard wiring, platform frame).\n";
    return 0;
}
