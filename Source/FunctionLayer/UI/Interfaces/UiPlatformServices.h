#pragma once

#include "FunctionLayer/UI/Interfaces/UiTextService.h"

#include <string>
#include <string_view>

namespace se::ui
{

// 剪贴板/IME/DPI/候选位置。平台适配器在 Platform 侧，核心只依赖此接口。
class UiPlatformServices
{
public:
    virtual ~UiPlatformServices() = default;

    [[nodiscard]] virtual bool ReadClipboard(std::string& utf8) = 0;
    [[nodiscard]] virtual bool WriteClipboard(std::string_view utf8) = 0;
    [[nodiscard]] virtual float DpiScale() const = 0;
    virtual void SetImeComposition(std::string_view utf8) = 0;
    virtual void SetImeCandidateAnchor(const UiImeCandidateAnchor& anchor) = 0;
    virtual void CancelIme() = 0;
};

} // namespace se::ui
