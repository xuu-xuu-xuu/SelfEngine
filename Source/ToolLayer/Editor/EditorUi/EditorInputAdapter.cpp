#include "ToolLayer/Editor/EditorUi/EditorInputAdapter.h"

#include <imm.h>

#include <algorithm>
#include <cmath>
#include <string>
#include <utility>
#include <vector>

namespace se::ui::editor
{
namespace
{
float SignedLowWord(LPARAM value) noexcept
{
    return static_cast<float>(static_cast<short>(LOWORD(static_cast<DWORD_PTR>(value))));
}

float SignedHighWord(LPARAM value) noexcept
{
    return static_cast<float>(static_cast<short>(HIWORD(static_cast<DWORD_PTR>(value))));
}

std::string Utf8FromWide(const std::wstring& text)
{
    if (text.empty())
    {
        return {};
    }
    const int bytes = WideCharToMultiByte(CP_UTF8, 0, text.data(), static_cast<int>(text.size()),
                                          nullptr, 0, nullptr, nullptr);
    if (bytes <= 0)
    {
        return {};
    }
    std::string output(static_cast<std::size_t>(bytes), '\0');
    WideCharToMultiByte(CP_UTF8, 0, text.data(), static_cast<int>(text.size()), output.data(), bytes,
                        nullptr, nullptr);
    return output;
}

std::wstring WideComposition(HIMC context, DWORD flags)
{
    if (context == nullptr)
    {
        return {};
    }
    const LONG bytes = ImmGetCompositionStringW(context, flags, nullptr, 0);
    if (bytes <= 0)
    {
        return {};
    }
    std::wstring text(static_cast<std::size_t>(bytes) / sizeof(wchar_t), L'\0');
    const LONG copied = ImmGetCompositionStringW(context, flags, text.data(), bytes);
    if (copied <= 0)
    {
        return {};
    }
    text.resize(static_cast<std::size_t>(copied) / sizeof(wchar_t));
    return text;
}
} // namespace

EditorInputAdapter::EditorInputAdapter(float dpiScale)
    : dpiScale_(dpiScale > 0.0f && std::isfinite(dpiScale) ? dpiScale : 1.0f)
{
}

void EditorInputAdapter::SetDpiScale(float scale)
{
    if (scale > 0.0f && std::isfinite(scale))
    {
        dpiScale_ = scale;
    }
}

float EditorInputAdapter::DpiScale() const noexcept
{
    return dpiScale_;
}

void EditorInputAdapter::SetWindow(HWND window) noexcept
{
    window_ = window;
}

void EditorInputAdapter::ApplyCandidateWindow()
{
    if (window_ == nullptr || candidateAnchor_.width <= 0.0f || candidateAnchor_.height <= 0.0f)
    {
        return;
    }
    HIMC context = ImmGetContext(window_);
    if (context == nullptr)
    {
        return;
    }
    CANDIDATEFORM form{};
    form.dwIndex = 0;
    form.dwStyle = CFS_CANDIDATEPOS;
    form.ptCurrentPos.x = static_cast<LONG>(std::lround(candidateAnchor_.x * dpiScale_));
    form.ptCurrentPos.y = static_cast<LONG>(std::lround((candidateAnchor_.y + candidateAnchor_.height) * dpiScale_));
    ImmSetCandidateWindow(context, &form);
    ImmReleaseContext(window_, context);
}

bool EditorInputAdapter::Translate(UINT message, WPARAM wparam, LPARAM lparam, UiInputEvent& event)
{
    event = {};
    switch (message)
    {
    case WM_MOUSEMOVE:
    case WM_LBUTTONDOWN:
    case WM_LBUTTONUP:
    case WM_RBUTTONDOWN:
    case WM_RBUTTONUP:
        event.x = SignedLowWord(lparam) / dpiScale_;
        event.y = SignedHighWord(lparam) / dpiScale_;
        event.type = message == WM_MOUSEMOVE ? UiEventType::PointerMove
            : (message == WM_LBUTTONDOWN || message == WM_RBUTTONDOWN ? UiEventType::PointerDown
                                                                      : UiEventType::PointerUp);
        return true;
    case WM_MOUSEWHEEL:
        event.x = SignedLowWord(lparam) / dpiScale_;
        event.y = SignedHighWord(lparam) / dpiScale_;
        event.wheel = static_cast<float>(static_cast<short>(HIWORD(wparam))) / 120.0f;
        event.type = UiEventType::PointerWheel;
        return true;
    case WM_KEYDOWN:
    case WM_KEYUP:
        event.type = message == WM_KEYDOWN ? UiEventType::KeyDown : UiEventType::KeyUp;
        event.key = static_cast<UiKeyCode>(wparam);
        event.shift = (GetKeyState(VK_SHIFT) & 0x8000) != 0;
        event.control = (GetKeyState(VK_CONTROL) & 0x8000) != 0;
        event.alt = (GetKeyState(VK_MENU) & 0x8000) != 0;
        return true;
    case WM_CHAR:
        // 组合期间由 WM_IME_COMPOSITION 提交正文，避免重复插入组合串。
        if (compositionActive_)
        {
            return false;
        }
        event.type = UiEventType::TextInput;
        event.codepoint = static_cast<std::uint32_t>(wparam);
        return true;
    case WM_IME_CHAR:
        // 有窗口上下文时以 GCS_RESULTSTR 为准；WM_IME_CHAR 只作为无 IME 上下文的回退。
        if (window_ != nullptr)
        {
            return false;
        }
        event.type = UiEventType::TextInput;
        event.codepoint = static_cast<std::uint32_t>(wparam);
        return true;
    case WM_IME_STARTCOMPOSITION:
        compositionActive_ = true;
        composition_.clear();
        event.type = UiEventType::ImeComposition;
        event.text = {};
        ApplyCandidateWindow();
        return true;
    case WM_IME_COMPOSITION:
    {
        if (window_ == nullptr)
        {
            event.type = UiEventType::ImeComposition;
            event.text = composition_;
            return true;
        }
        HIMC context = ImmGetContext(window_);
        if (context == nullptr)
        {
            event.type = UiEventType::ImeComposition;
            event.text = composition_;
            return true;
        }
        if ((lparam & GCS_RESULTSTR) != 0)
        {
            composition_ = Utf8FromWide(WideComposition(context, GCS_RESULTSTR));
            ImmReleaseContext(window_, context);
            compositionActive_ = false;
            event.type = UiEventType::ImeCommit;
            event.text = composition_;
            ApplyCandidateWindow();
            return true;
        }
        if ((lparam & GCS_COMPSTR) != 0)
        {
            composition_ = Utf8FromWide(WideComposition(context, GCS_COMPSTR));
            compositionActive_ = true;
            event.type = UiEventType::ImeComposition;
            event.text = composition_;
            ImmReleaseContext(window_, context);
            ApplyCandidateWindow();
            return true;
        }
        ImmReleaseContext(window_, context);
        event.type = UiEventType::ImeComposition;
        event.text = composition_;
        return true;
    }
    case WM_IME_ENDCOMPOSITION:
        compositionActive_ = false;
        composition_.clear();
        event.type = UiEventType::ImeCancel;
        return true;
    default:
        return false;
    }
}

void EditorInputAdapter::SetImeComposition(std::string composition)
{
    composition_ = std::move(composition);
}

const std::string& EditorInputAdapter::Composition() const noexcept
{
    return composition_;
}

void EditorInputAdapter::SetImeCandidateAnchor(const UiImeCandidateAnchor& anchor)
{
    candidateAnchor_ = anchor;
}

const UiImeCandidateAnchor& EditorInputAdapter::CandidateAnchor() const noexcept
{
    return candidateAnchor_;
}

} // namespace se::ui::editor
