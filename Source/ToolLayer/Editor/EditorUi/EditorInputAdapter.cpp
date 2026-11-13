#include "ToolLayer/Editor/EditorUi/EditorInputAdapter.h"
#include "PlatformLayer/Windows/WindowsInput.h"

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
    if(window_)RemovePropW(window_,WindowsTextInputModeProperty);
    window_ = window;
    SetTextInputActive(textInputActive_);
}

EditorInputAdapter::~EditorInputAdapter(){SetWindow(nullptr);}
void EditorInputAdapter::SetTextInputActive(bool active)noexcept
{
    textInputActive_=active;
    if(window_)SetPropW(window_,WindowsTextInputModeProperty,reinterpret_cast<HANDLE>(static_cast<UINT_PTR>(active?2:1)));
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
    event.shift = (GetKeyState(VK_SHIFT) & 0x8000) != 0;
    event.control = (GetKeyState(VK_CONTROL) & 0x8000) != 0;
    event.alt = (GetKeyState(VK_MENU) & 0x8000) != 0;
    switch (message)
    {
    case WM_MOUSEMOVE:
    case WM_LBUTTONDOWN:
    case WM_LBUTTONUP:
    case WM_RBUTTONDOWN:
    case WM_RBUTTONUP:
    case WM_MBUTTONDOWN:
    case WM_MBUTTONUP:
        event.button = (message==WM_RBUTTONDOWN||message==WM_RBUTTONUP)?1:
            (message==WM_MBUTTONDOWN||message==WM_MBUTTONUP)?2:0;
        event.x = SignedLowWord(lparam) / dpiScale_;
        event.y = SignedHighWord(lparam) / dpiScale_;
        event.type = message == WM_MOUSEMOVE ? UiEventType::PointerMove
            : (message == WM_LBUTTONDOWN || message == WM_RBUTTONDOWN || message == WM_MBUTTONDOWN ? UiEventType::PointerDown
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
    case WM_SYSKEYDOWN:
    case WM_SYSKEYUP:
        event.type = (message == WM_KEYDOWN||message==WM_SYSKEYDOWN) ? UiEventType::KeyDown : UiEventType::KeyUp;
        event.key = static_cast<UiKeyCode>(wparam);
        if(wparam==VK_PROCESSKEY&&!textInputActive_)
        {
            // 到 WndProc 时 TranslateMessage 已执行，ImmGetVirtualKey 可能仍返回 229。
            // 非文本上下文按原始扫描码恢复字母；文本框保持 IME 原消息和组合流程。
            UINT scan=static_cast<UINT>((static_cast<DWORD_PTR>(lparam)>>16)&0xff);
            if((static_cast<DWORD_PTR>(lparam)&(1u<<24))!=0)scan|=0xe000;
            const UINT captured=window_?static_cast<UINT>(reinterpret_cast<UINT_PTR>(GetPropW(window_,WindowsImeVirtualKeyProperty))):0;
            const UINT original=captured?captured:MapVirtualKeyExW(scan,MAPVK_VSC_TO_VK_EX,GetKeyboardLayout(0));
            if(original>='A'&&original<='Z')event.key=original;
        }
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
    case WM_KILLFOCUS:
        event.type=UiEventType::FocusLost;
        return true;
    case WM_CAPTURECHANGED:
        event.type=UiEventType::PointerCaptureLost;
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
