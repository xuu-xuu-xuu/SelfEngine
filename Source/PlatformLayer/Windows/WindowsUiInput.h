#pragma once
#include <Windows.h>
#include <optional>
#include <functional>
#include <utility>
#include "FunctionLayer/GameUI/UiTree.h"
#include "ResourceLayer/Fonts/FontCache.h"

namespace se
{
struct UiClipboard
{
    std::function<bool(HWND,std::string_view)> write;
    std::function<std::optional<std::string>(HWND)> read;
};
struct UiImeUpdate
{
    std::optional<std::string> result,composition;
    std::size_t caret=std::string::npos;
    bool cancel=false;
};
// 每个窗口各有一份，UI 先消费事件，应用仍可维护原始按键状态。
class WindowsUiInput
{
public:
    explicit WindowsUiInput(UiTree& tree,UiClipboard clipboard={}) : tree_(tree),clipboard_(std::move(clipboard)) {}
    ~WindowsUiInput();
    std::optional<LRESULT> Process(HWND window,UINT message,WPARAM wParam,LPARAM lParam);
    void PositionIme(HWND window);
    void SetViewport(UiRect viewport); // 编辑器内嵌 Play 区域；输入/IME 使用窗口客户区原点偏移。
    void SynchronizeCapture(HWND window);
    std::optional<UiRect> ImeCaret()const{return imeCaret_;} // 客户区坐标；供宿主/自动验收查询。
    bool UpdateIme(const UiImeUpdate& update); // IMM 适配器与确定性消息用例共用同一提交入口。
    bool KeyboardConsumed() const { return keyboardConsumed_ || tree_.ConsumesKeyboard(); }
    bool PointerConsumed() const { return pointerConsumed_ || gestureConsumed_ || tree_.HasPointerInput(); }
    void EndFrame() { SynchronizeCapture(window_);keyboardConsumed_ = pointerConsumed_ = false; }
private:
    void ReleaseOwnedCapture();
    void SynchronizeTextInput(HWND window);
    bool InsertUnit(char16_t unit);
    bool TextFocused() const;
    void PlaceCaret(UiPoint point,bool extend,bool newlyFocused=false);
    UiTree& tree_;
    FontCache fonts_;
    UiClipboard clipboard_;
    char16_t highSurrogate_ = 0;
    bool selecting_ = false;
    bool keyboardConsumed_ = false, pointerConsumed_ = false;
    bool compositionActive_ = false;
    HWND window_=nullptr;
    bool captureOwned_=false,gestureConsumed_=false,trackingMouse_=false;
    std::optional<UiRect> viewport_;
    std::size_t selectionAnchor_ = 0;
    std::uint64_t observedInputEpoch_=0;
    std::optional<UiRect> imeCaret_;
};
}
