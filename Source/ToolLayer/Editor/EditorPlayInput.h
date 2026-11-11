#pragma once
#include "PlatformLayer/Windows/WindowsUiInput.h"
#include "PlatformLayer/InputState.h"
#include <imgui/imgui.h>
#include <memory>

namespace se
{
struct EditorInputRoute
{
    bool forwardEditor=true;
    std::optional<LRESULT> handled;
};
// 使用最近完成帧的 ImGui 窗口层级命中，阻止覆盖面板/弹窗后的 Play UI 接收点击。
class EditorPlayInput
{
public:
    explicit EditorPlayInput(ImGuiContext* context):context_(context){}
    void Attach(UiTree* ui); // Stop 前先 Attach(nullptr)，此方法不访问 ImGui context。
    void SetViewport(HWND window,UiRect viewport);
    EditorInputRoute Process(HWND window,UINT message,WPARAM wParam,LPARAM lParam);
    bool Active() const noexcept {return active_;}
    bool KeyboardConsumed() const;
    bool PointerConsumed() const;
    InputState GameplayInput(const InputState& source) const;
    void EndFrame();
    void Deactivate(HWND window=nullptr);
private:
    bool OwnsPoint(UiPoint point) const;
    bool Held() const;
    ImGuiContext* context_=nullptr;
    UiTree* ui_=nullptr;
    std::unique_ptr<WindowsUiInput> input_;
    UiRect viewport_{};
    UiPoint pointer_{};
    std::array<bool,3> held_{};
    bool active_=false,over_=false;
    HWND window_=nullptr;
};
}
