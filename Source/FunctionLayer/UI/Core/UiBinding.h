#pragma once

#include "FunctionLayer/UI/Core/UiDocument.h"

#include <cstddef>
#include <cstdint>
#include <functional>
#include <memory>
#include <string>

namespace se::ui
{

enum class UiBindingProperty : std::uint8_t
{
    Text = 0,
    Visible,
    Enabled,
};

// 显式更新的绑定表：在选中/数据变化时调用 Update，不参与每帧轮询。
// 目标元素被销毁后，绑定会被跳过；由调用方在适当时机 Unbind 或重建。
class UiBindingTable
{
public:
    explicit UiBindingTable(UiDocument& document);
    ~UiBindingTable();

    UiBindingTable(const UiBindingTable&) = delete;
    UiBindingTable& operator=(const UiBindingTable&) = delete;
    UiBindingTable(UiBindingTable&&) noexcept;
    UiBindingTable& operator=(UiBindingTable&&) noexcept;

    bool BindText(UiElementHandle element, std::function<std::string()> getter);
    bool BindVisible(UiElementHandle element, std::function<bool()> getter);
    bool BindEnabled(UiElementHandle element, std::function<bool()> getter);
    bool Unbind(UiElementHandle element, UiBindingProperty property);

    // 返回实际写入文档的属性数量；值未变化时不写、不标脏。
    std::size_t Update();
    [[nodiscard]] std::size_t BindingCount() const noexcept;

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

} // namespace se::ui
