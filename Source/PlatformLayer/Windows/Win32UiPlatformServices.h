#pragma once

#include "FunctionLayer/UI/Interfaces/UiPlatformServices.h"

#include <Windows.h>
#include <string>
#include <string_view>

namespace se
{

// Win32 适配器：剪贴板、DPI、IME 候选位置；核心只依赖 UiPlatformServices 接口。
class Win32UiPlatformServices final : public ui::UiPlatformServices
{
public:
    explicit Win32UiPlatformServices(HWND window);

    bool ReadClipboard(std::string& utf8) override;
    bool WriteClipboard(std::string_view utf8) override;
    [[nodiscard]] float DpiScale() const override;
    void SetImeComposition(std::string_view utf8) override;
    void SetImeCandidateAnchor(const ui::UiImeCandidateAnchor& anchor) override;
    void CancelIme() override;

private:
    HWND window_ = nullptr;
};

} // namespace se
