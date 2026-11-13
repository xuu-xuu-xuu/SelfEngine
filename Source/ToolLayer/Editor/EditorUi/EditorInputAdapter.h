#pragma once

#include "FunctionLayer/UI/Core/UiInput.h"
#include "FunctionLayer/UI/Interfaces/UiTextService.h"

#include <Windows.h>

#include <string>
#include <string_view>

namespace se::ui::editor
{

// Win32 消息 -> se::ui 事件；DPI 只缩放指针坐标，不把 ImGui 类型带进新路径。
class EditorInputAdapter
{
public:
    explicit EditorInputAdapter(float dpiScale = 1.0f);
    ~EditorInputAdapter();

    void SetDpiScale(float scale);
    [[nodiscard]] float DpiScale() const noexcept;
    void SetWindow(HWND window) noexcept;
    void SetTextInputActive(bool active)noexcept;

    bool Translate(UINT message, WPARAM wparam, LPARAM lparam, UiInputEvent& event);

    void SetImeComposition(std::string composition);
    [[nodiscard]] const std::string& Composition() const noexcept;
    void SetImeCandidateAnchor(const UiImeCandidateAnchor& anchor);
    [[nodiscard]] const UiImeCandidateAnchor& CandidateAnchor() const noexcept;

private:
    void ApplyCandidateWindow();
    float dpiScale_ = 1.0f;
    HWND window_ = nullptr;
    bool compositionActive_ = false;
    bool textInputActive_ = false;
    std::string composition_;
    UiImeCandidateAnchor candidateAnchor_{};
};

} // namespace se::ui::editor
