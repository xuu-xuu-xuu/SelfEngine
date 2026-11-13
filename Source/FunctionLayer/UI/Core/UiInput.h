#pragma once

#include "FunctionLayer/UI/Core/UiDocument.h"
#include "FunctionLayer/UI/Core/UiLayout.h"
#include "FunctionLayer/UI/Core/UiStyle.h"

#include <cstdint>
#include <functional>
#include <memory>
#include <string_view>
#include <vector>

namespace se::ui
{

using UiKeyCode = std::uint32_t;

inline constexpr UiKeyCode UiKeyTab = 9;
inline constexpr UiKeyCode UiKeyEnter = 13;
inline constexpr UiKeyCode UiKeyEscape = 27;
inline constexpr UiKeyCode UiKeyBackspace = 8;
inline constexpr UiKeyCode UiKeyDelete = 46;
inline constexpr UiKeyCode UiKeyLeft = 37;
inline constexpr UiKeyCode UiKeyUp = 38;
inline constexpr UiKeyCode UiKeyRight = 39;
inline constexpr UiKeyCode UiKeyDown = 40;
inline constexpr UiKeyCode UiKeyHome = 36;
inline constexpr UiKeyCode UiKeyEnd = 35;
inline constexpr UiKeyCode UiKeySpace = 32;

enum class UiEventType : std::uint8_t
{
    PointerMove = 0,
    PointerDown,
    PointerUp,
    PointerWheel,
    KeyDown,
    KeyUp,
    TextInput,
    ImeComposition,
    ImeCommit,
    ImeCancel,
    FocusGained,
    FocusLost,
    PointerCaptureLost,
};

enum class UiEventPhase : std::uint8_t
{
    Capture = 0,
    Target,
    Bubble,
};

struct UiInputEvent
{
    UiEventType type = UiEventType::PointerMove;
    UiEventPhase phase = UiEventPhase::Target;
    UiElementHandle target;
    UiElementHandle currentTarget;
    float x = 0.0f;
    float y = 0.0f;
    float wheel = 0.0f;
    std::uint8_t button = 0; // 0 左键、1 右键、2 中键；移动事件由捕获者解释。
    UiKeyCode key = 0;
    bool shift = false;
    bool control = false;
    bool alt = false;
    std::uint32_t codepoint = 0;
    std::string_view text;
    bool propagationStopped = false;
    bool handled = false;

    void StopPropagation() noexcept
    {
        propagationStopped = true;
    }
};

using UiEventHandler = std::function<void(UiInputEvent&)>;

struct UiHitResult
{
    UiElementHandle target;
    std::vector<UiElementHandle> path;
};

class UiHitTest
{
public:
    UiHitTest(UiDocument& document, UiLayoutEngine& layout, UiStyleEngine* styleEngine = nullptr);

    [[nodiscard]] UiHitResult Hit(float x, float y) const;
    [[nodiscard]] UiHitResult Hit(UiElementHandle root,float x,float y) const;

private:
    [[nodiscard]] bool HitNode(UiElementHandle element, float x, float y, const UiRect& clip,
                               UiElementHandle& target) const;

    UiDocument* document_;
    UiLayoutEngine* layout_;
    UiStyleEngine* styleEngine_;
};

// 捕获/目标/冒泡分发、焦点、指针捕获。核心不依赖 Win32。
class UiDispatcher
{
public:
    UiDispatcher(UiDocument& document, UiLayoutEngine& layout, UiStyleEngine* styleEngine = nullptr);
    ~UiDispatcher();

    UiDispatcher(UiDispatcher&&) noexcept;
    UiDispatcher& operator=(UiDispatcher&&) noexcept;
    UiDispatcher(const UiDispatcher&) = delete;
    UiDispatcher& operator=(const UiDispatcher&) = delete;

    void AddHandler(UiElementHandle element, UiEventType type, UiEventHandler handler,
                    UiEventPhase phase = UiEventPhase::Target);
    void RemoveHandlers(UiElementHandle element);

    // 返回是否找到目标并完成分发；无目标返回 false。
    bool Dispatch(UiInputEvent& event);
    // 多原生窗口共用文档时，仅在当前表面命中及遍历 Tab 焦点；捕获仍跨表面有效。
    void SetInputRoot(UiElementHandle root);

    void SetFocus(UiElementHandle element);
    [[nodiscard]] UiElementHandle Focus() const noexcept;
    void ClearFocus();
    void FocusNext(bool backward = false);
    void ValidateFocus();

    void SetPointerCapture(UiElementHandle element);
    void ReleasePointerCapture();
    [[nodiscard]] UiElementHandle PointerCapture() const noexcept;

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

} // namespace se::ui
