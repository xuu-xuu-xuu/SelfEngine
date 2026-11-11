#pragma once
#include "ImGuiEditor.h"
#include <Windows.h>
#include <optional>

namespace se
{
// 一个窗口/context 配一份；须在 ImGuiEditor/context 之前销毁。
// 仅包装官方 Win32 platform backend，不包含任何 GPU renderer backend。
class WindowsEditorInput
{
public:
    static void EnableDpiAwareness();
    WindowsEditorInput(HWND window, ImGuiEditor& editor);
    ~WindowsEditorInput();
    WindowsEditorInput(const WindowsEditorInput&) = delete;
    WindowsEditorInput& operator=(const WindowsEditorInput&) = delete;
    bool Ready() const noexcept { return ready_; }
    const std::string& LastError() const noexcept { return error_; }
    // 窗口消息应先交给此方法；返回值表示 UI 消费，仍需让底层维护原始按键/焦点状态。
    // WM_IME_* 继续交给 DefWindowProc，提交文字通过 WM_CHAR 只进入 ImGui 一次。
    std::optional<LRESULT> Process(HWND window, UINT message, WPARAM wParam, LPARAM lParam);
    // 每帧在 ImGuiEditor::Draw 前调用一次；随后使用 usePlatformInput=true。
    bool PrepareFrame();
    bool WantsMouse() const;
    bool WantsKeyboard() const;
private:
    HWND window_ = nullptr;
    ImGuiEditor* editor_=nullptr;
    ImGuiContext* context_ = nullptr;
    bool ready_ = false;
    std::string error_;
};
}
