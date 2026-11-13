#include "AppBranding.h"
#include <shobjidl.h>

namespace se
{
HICON AppIcon(int pixels) noexcept
{
    HMODULE module=nullptr;
    if(!GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS|GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
        reinterpret_cast<LPCWSTR>(&AppIcon),&module))return nullptr;
    return static_cast<HICON>(LoadImageW(module,MAKEINTRESOURCEW(101),IMAGE_ICON,pixels,pixels,LR_SHARED));
}
void ApplyAppIcon(HWND window) noexcept
{
    [[maybe_unused]] static const bool grouped=SUCCEEDED(SetCurrentProcessExplicitAppUserModelID(L"Flare.Engine"));
    const UINT dpi=GetDpiForWindow(window);const int smallIconSize=MulDiv(16,dpi?dpi:96,96),largeIconSize=MulDiv(32,dpi?dpi:96,96);
    if(const auto icon=AppIcon(smallIconSize))SendMessageW(window,WM_SETICON,ICON_SMALL,reinterpret_cast<LPARAM>(icon));
    if(const auto icon=AppIcon(largeIconSize))SendMessageW(window,WM_SETICON,ICON_BIG,reinterpret_cast<LPARAM>(icon));
}
}
