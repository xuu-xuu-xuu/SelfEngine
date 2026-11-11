#include "FunctionLayer/UI/Core/UiInput.h"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <string_view>
#include <unordered_map>
#include <utility>
#include <vector>

namespace se::ui
{
namespace
{
bool FinitePoint(float x, float y) noexcept
{
    return std::isfinite(x) && std::isfinite(y);
}

bool PointInRect(float x, float y, const UiRect& rect) noexcept
{
    return x >= rect.x && y >= rect.y && x < rect.x + rect.width && y < rect.y + rect.height;
}

bool FiniteRect(const UiRect& rect) noexcept
{
    return std::isfinite(rect.x) && std::isfinite(rect.y) && std::isfinite(rect.width) &&
        std::isfinite(rect.height) && rect.width >= 0.0f && rect.height >= 0.0f;
}

UiRect Intersect(const UiRect& lhs, const UiRect& rhs) noexcept
{
    const float x = std::max(lhs.x, rhs.x);
    const float y = std::max(lhs.y, rhs.y);
    return {x, y, std::max(0.0f, std::min(lhs.x + lhs.width, rhs.x + rhs.width) - x),
            std::max(0.0f, std::min(lhs.y + lhs.height, rhs.y + rhs.height) - y)};
}

bool Empty(const UiRect& rect) noexcept
{
    return rect.width <= 0.0f || rect.height <= 0.0f;
}

std::vector<UiElementHandle> PathToRoot(const UiDocument& document, UiElementHandle target)
{
    std::vector<UiElementHandle> path;
    UiElementHandle current = target;
    while (current.IsValid())
    {
        path.push_back(current);
        const UiElement* element = document.Element(current);
        if (!element)
        {
            break;
        }
        current = element->Parent();
    }
    std::reverse(path.begin(), path.end());
    return path;
}

bool Focusable(UiElementKind kind) noexcept
{
    return kind == UiElementKind::TextEdit || kind == UiElementKind::Button ||
        kind == UiElementKind::Checkbox || kind == UiElementKind::Combo;
}
} // namespace

UiHitTest::UiHitTest(UiDocument& document, UiLayoutEngine& layout, UiStyleEngine* styleEngine)
    : document_(&document), layout_(&layout), styleEngine_(styleEngine)
{
}

UiHitResult UiHitTest::Hit(float x, float y) const
{
    UiHitResult result;
    if (!document_ || !layout_ || !document_->IsValid() || !FinitePoint(x, y))
    {
        return result;
    }
    const UiElementHandle root = document_->Root();
    const UiRect viewport = layout_->Rect(root);
    if (!FiniteRect(viewport) || Empty(viewport))
    {
        return result;
    }
    const UiRect clip{0.0f, 0.0f, viewport.width, viewport.height};
    UiElementHandle target;
    if (!HitNode(root, x, y, clip, target) || !target.IsValid())
    {
        return result;
    }
    result.target = target;
    result.path = PathToRoot(*document_, target);
    return result;
}

bool UiHitTest::HitNode(UiElementHandle element, float x, float y, const UiRect& clip,
                        UiElementHandle& target) const
{
    const UiElement* node = document_->Element(element);
    if (!node || !document_->Visible(element) || !document_->Enabled(element))
    {
        return false;
    }
    const UiStyle& style = styleEngine_ ? styleEngine_->Compute(element) : document_->Style(element);
    if (style.display == UiDisplay::None || style.pointerEvents == UiPointerEvents::None)
    {
        return false;
    }
    const UiRect rect = layout_->Rect(element);
    if (!FiniteRect(rect) || !PointInRect(x, y, clip))
    {
        return false;
    }
    UiRect childClip = clip;
    if (style.overflow != UiOverflow::Visible)
    {
        childClip = Intersect(clip, rect);
        if (Empty(childClip))
        {
            return false;
        }
    }

    std::vector<UiElementHandle> children;
    for (UiElementHandle child = document_->FirstChild(element); child.IsValid();
         child = document_->NextSibling(child))
    {
        children.push_back(child);
    }
    for (auto child = children.rbegin(); child != children.rend(); ++child)
    {
        if (HitNode(*child, x, y, childClip, target))
        {
            return true;
        }
    }
    if (PointInRect(x, y, rect) && PointInRect(x, y, clip))
    {
        target = element;
        return true;
    }
    return false;
}

struct UiDispatcher::Impl
{
    struct Handler
    {
        UiElementHandle element;
        UiEventType type = UiEventType::PointerMove;
        UiEventPhase phase = UiEventPhase::Target;
        UiEventHandler function;
    };

    UiDocument* document = nullptr;
    UiLayoutEngine* layout = nullptr;
    UiStyleEngine* style = nullptr;
    UiHitTest hitTest;
    std::vector<Handler> handlers;
    UiElementHandle focus;
    UiElementHandle capture;

    Impl(UiDocument& documentValue, UiLayoutEngine& layoutValue, UiStyleEngine* styleValue)
        : document(&documentValue), layout(&layoutValue), style(styleValue),
          hitTest(documentValue, layoutValue, styleValue)
    {
    }

    [[nodiscard]] bool FocusableElement(UiElementHandle element) const
    {
        const UiElement* node = document->Element(element);
        return node && Focusable(node->Kind()) && document->Visible(element) && document->Enabled(element);
    }

    void DispatchAt(UiElementHandle element, UiInputEvent& event, UiEventPhase phase)
    {
        event.currentTarget = element;
        event.phase = phase;
        for (const Handler& handler : handlers)
        {
            if (handler.element == element && handler.type == event.type && handler.phase == phase)
            {
                handler.function(event);
                if (event.propagationStopped)
                {
                    break;
                }
            }
        }
    }

    void DispatchPath(const std::vector<UiElementHandle>& path, UiInputEvent& event)
    {
        if (path.empty())
        {
            return;
        }
        for (std::size_t index = 0; index + 1 < path.size(); ++index)
        {
            DispatchAt(path[index], event, UiEventPhase::Capture);
            if (event.propagationStopped)
            {
                return;
            }
        }
        DispatchAt(path.back(), event, UiEventPhase::Target);
        if (event.propagationStopped)
        {
            return;
        }
        for (std::size_t index = path.size(); index-- > 0;)
        {
            DispatchAt(path[index], event, UiEventPhase::Bubble);
            if (event.propagationStopped)
            {
                return;
            }
        }
    }

    void SendFocusEvent(UiEventType type, UiElementHandle target)
    {
        if (!target.IsValid() || !document->Element(target))
        {
            return;
        }
        UiInputEvent event;
        event.type = type;
        event.target = target;
        DispatchPath(PathToRoot(*document, target), event);
    }

    void ClearFocusInternal()
    {
        const UiElementHandle old = focus;
        if (!old.IsValid())
        {
            return;
        }
        const UiElement* element = document->Element(old);
        if (element && element->Kind() == UiElementKind::TextEdit)
        {
            SendFocusEvent(UiEventType::ImeCancel, old);
        }
        focus = {};
        SendFocusEvent(UiEventType::FocusLost, old);
    }

    void ReleaseCaptureInternal()
    {
        const UiElementHandle old = capture;
        if (!old.IsValid())
        {
            return;
        }
        capture = {};
        SendFocusEvent(UiEventType::PointerCaptureLost, old);
    }
};

UiDispatcher::UiDispatcher(UiDocument& document, UiLayoutEngine& layout, UiStyleEngine* styleEngine)
    : impl_(std::make_unique<Impl>(document, layout, styleEngine))
{
}

UiDispatcher::~UiDispatcher() = default;

UiDispatcher::UiDispatcher(UiDispatcher&&) noexcept = default;

UiDispatcher& UiDispatcher::operator=(UiDispatcher&&) noexcept = default;

void UiDispatcher::AddHandler(UiElementHandle element, UiEventType type, UiEventHandler handler,
                              UiEventPhase phase)
{
    if (!impl_ || !handler)
    {
        return;
    }
    impl_->handlers.push_back({element, type, phase, std::move(handler)});
}

void UiDispatcher::RemoveHandlers(UiElementHandle element)
{
    if (!impl_)
    {
        return;
    }
    impl_->handlers.erase(std::remove_if(impl_->handlers.begin(), impl_->handlers.end(),
                                         [element](const Impl::Handler& handler) {
                                             return handler.element == element;
                                         }),
                          impl_->handlers.end());
}

bool UiDispatcher::Dispatch(UiInputEvent& event)
{
    if (!impl_ || !impl_->document || !impl_->layout)
    {
        return false;
    }
    ValidateFocus();

    if (event.type == UiEventType::KeyDown && event.key == UiKeyTab)
    {
        FocusNext(event.shift);
        event.handled = true;
        return true;
    }

    UiElementHandle target;
    const bool pointer = event.type == UiEventType::PointerMove ||
        event.type == UiEventType::PointerDown || event.type == UiEventType::PointerUp ||
        event.type == UiEventType::PointerWheel;
    if (pointer)
    {
        // 滚轮按指针位置命中，不使用拖动捕获；否则一次未释放的按下会把滚动永久锁在旧元素。
        if ((event.type == UiEventType::PointerMove || event.type == UiEventType::PointerUp) &&
            impl_->capture.IsValid() && impl_->document->Element(impl_->capture))
        {
            target = impl_->capture;
        }
        else
        {
            target = impl_->hitTest.Hit(event.x, event.y).target;
        }
    }
    else if (event.type == UiEventType::KeyDown || event.type == UiEventType::KeyUp ||
             event.type == UiEventType::TextInput || event.type == UiEventType::ImeComposition ||
             event.type == UiEventType::ImeCommit || event.type == UiEventType::ImeCancel)
    {
        target = impl_->focus;
        if (!target.IsValid() || !impl_->document->Element(target))
        {
            return false;
        }
        if ((event.type == UiEventType::TextInput || event.type == UiEventType::ImeComposition ||
             event.type == UiEventType::ImeCommit || event.type == UiEventType::ImeCancel))
        {
            const UiElement* focused = impl_->document->Element(target);
            if (!focused || focused->Kind() != UiElementKind::TextEdit)
            {
                return false;
            }
        }
    }
    else
    {
        target = event.target;
    }
    if (!target.IsValid() || !impl_->document->Element(target))
    {
        return false;
    }

    event.target = target;
    event.propagationStopped = false;
    if (event.type == UiEventType::PointerDown)
    {
        if (impl_->FocusableElement(target))
        {
            SetFocus(target);
        }
        else
        {
            ClearFocus();
        }
        SetPointerCapture(target);
    }

    const std::vector<UiElementHandle> path = PathToRoot(*impl_->document, target);
    impl_->DispatchPath(path, event);
    if (event.type == UiEventType::PointerUp)
    {
        ReleasePointerCapture();
    }
    event.currentTarget = {};
    return true;
}

void UiDispatcher::SetFocus(UiElementHandle element)
{
    if (!impl_)
    {
        return;
    }
    if (!element.IsValid() || !impl_->FocusableElement(element))
    {
        ClearFocus();
        return;
    }
    if (impl_->focus == element)
    {
        return;
    }
    impl_->ClearFocusInternal();
    impl_->focus = element;
    impl_->SendFocusEvent(UiEventType::FocusGained, element);
}

UiElementHandle UiDispatcher::Focus() const noexcept
{
    return impl_ ? impl_->focus : UiElementHandle{};
}

void UiDispatcher::ClearFocus()
{
    if (impl_)
    {
        impl_->ClearFocusInternal();
    }
}

void UiDispatcher::FocusNext(bool backward)
{
    if (!impl_)
    {
        return;
    }
    std::vector<UiElementHandle> focusable;
    const auto visit = [&focusable](auto&& self, UiDocument& document, UiElementHandle element) -> void {
        const UiElement* node = document.Element(element);
        if (!node)
        {
            return;
        }
        if (Focusable(node->Kind()) && document.Visible(element) && document.Enabled(element))
        {
            focusable.push_back(element);
        }
        for (UiElementHandle child = document.FirstChild(element); child.IsValid();
             child = document.NextSibling(child))
        {
            self(self, document, child);
        }
    };
    visit(visit, *impl_->document, impl_->document->Root());
    if (focusable.empty())
    {
        return;
    }
    std::size_t next = backward ? focusable.size() - 1 : 0;
    const auto current = std::find(focusable.begin(), focusable.end(), impl_->focus);
    if (current != focusable.end())
    {
        const std::size_t index = static_cast<std::size_t>(current - focusable.begin());
        next = backward ? (index + focusable.size() - 1) % focusable.size() : (index + 1) % focusable.size();
    }
    SetFocus(focusable[next]);
}

void UiDispatcher::ValidateFocus()
{
    if (!impl_ || !impl_->focus.IsValid())
    {
        return;
    }
    if (!impl_->FocusableElement(impl_->focus))
    {
        ClearFocus();
    }
}

void UiDispatcher::SetPointerCapture(UiElementHandle element)
{
    if (!impl_ || !element.IsValid() || !impl_->document->Element(element))
    {
        return;
    }
    if (impl_->capture == element)
    {
        return;
    }
    impl_->ReleaseCaptureInternal();
    impl_->capture = element;
}

void UiDispatcher::ReleasePointerCapture()
{
    if (impl_)
    {
        impl_->ReleaseCaptureInternal();
    }
}

UiElementHandle UiDispatcher::PointerCapture() const noexcept
{
    return impl_ ? impl_->capture : UiElementHandle{};
}

} // namespace se::ui
