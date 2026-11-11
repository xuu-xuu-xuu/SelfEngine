#include "PlatformLayer/Windows/Win32UiPlatformServices.h"

#include "CoreLayer/Utf.h"

#include <cmath>
#include <cstring>
#include <cwchar>
#include <string>
#include <string_view>

namespace se
{
namespace
{
class ClipboardScope
{
public:
    explicit ClipboardScope(HWND window)
        : opened_(OpenClipboard(window) != FALSE)
    {
    }

    ~ClipboardScope()
    {
        if (opened_)
        {
            CloseClipboard();
        }
    }

    ClipboardScope(const ClipboardScope&) = delete;
    ClipboardScope& operator=(const ClipboardScope&) = delete;

    [[nodiscard]] bool Opened() const noexcept
    {
        return opened_;
    }

private:
    bool opened_ = false;
};
} // namespace

Win32UiPlatformServices::Win32UiPlatformServices(HWND window)
    : window_(window)
{
}

bool Win32UiPlatformServices::ReadClipboard(std::string& utf8)
{
    ClipboardScope clipboard(window_);
    if (!clipboard.Opened())
    {
        return false;
    }
    HANDLE handle = GetClipboardData(CF_UNICODETEXT);
    if (!handle)
    {
        return false;
    }
    const auto* text = static_cast<const wchar_t*>(GlobalLock(handle));
    if (!text)
    {
        return false;
    }
    std::u32string codepoints;
    const std::u16string utf16(reinterpret_cast<const char16_t*>(text));
    const bool decoded = DecodeUtf16(utf16, codepoints);
    GlobalUnlock(handle);
    if (!decoded)
    {
        return false;
    }
    return EncodeUtf8(codepoints, utf8);
}

bool Win32UiPlatformServices::WriteClipboard(std::string_view utf8)
{
    std::u32string codepoints;
    if (!DecodeUtf8(utf8, codepoints))
    {
        return false;
    }
    const std::u16string utf16 = EncodeUtf16(codepoints);
    ClipboardScope clipboard(window_);
    if (!clipboard.Opened() || !EmptyClipboard())
    {
        return false;
    }
    const std::size_t bytes = (utf16.size() + 1) * sizeof(wchar_t);
    HGLOBAL memory = GlobalAlloc(GMEM_MOVEABLE, bytes);
    if (!memory)
    {
        return false;
    }
    void* destination = GlobalLock(memory);
    if (!destination)
    {
        GlobalFree(memory);
        return false;
    }
    std::memcpy(destination, utf16.c_str(), bytes);
    GlobalUnlock(memory);
    if (!SetClipboardData(CF_UNICODETEXT, memory))
    {
        GlobalFree(memory);
        return false;
    }
    return true;
}

float Win32UiPlatformServices::DpiScale() const
{
    const UINT dpi = window_ ? GetDpiForWindow(window_) : 96;
    return dpi == 0 ? 1.0f : static_cast<float>(dpi) / 96.0f;
}

void Win32UiPlatformServices::SetImeComposition(std::string_view)
{
    // 组合串由 EditorInputAdapter/IME 消息在输入阶段接管；这里保持接口占位。
}

void Win32UiPlatformServices::SetImeCandidateAnchor(const ui::UiImeCandidateAnchor& anchor)
{
    if (!window_)
    {
        return;
    }
    HIMC context = ImmGetContext(window_);
    if (!context)
    {
        return;
    }
    CANDIDATEFORM form{};
    form.dwIndex = 0;
    form.dwStyle = 0;
    form.ptCurrentPos.x = static_cast<LONG>(std::lround(anchor.x));
    form.ptCurrentPos.y = static_cast<LONG>(std::lround(anchor.y + anchor.height));
    form.rcArea = RECT{form.ptCurrentPos.x, form.ptCurrentPos.y, form.ptCurrentPos.x, form.ptCurrentPos.y};
    ImmSetCandidateWindow(context, &form);
    ImmReleaseContext(window_, context);
}

void Win32UiPlatformServices::CancelIme()
{
    if (!window_)
    {
        return;
    }
    HIMC context = ImmGetContext(window_);
    if (!context)
    {
        return;
    }
    ImmNotifyIME(context, NI_COMPOSITIONSTR, CPS_CANCEL, 0);
    ImmReleaseContext(window_, context);
}

} // namespace se
