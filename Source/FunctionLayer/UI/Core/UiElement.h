#pragma once

#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

namespace se::ui
{

struct UiId
{
    std::uint32_t value = 0;

    [[nodiscard]] constexpr bool IsValid() const noexcept
    {
        return value != 0;
    }

    friend constexpr bool operator==(UiId lhs, UiId rhs) noexcept
    {
        return lhs.value == rhs.value;
    }

    friend constexpr bool operator!=(UiId lhs, UiId rhs) noexcept
    {
        return !(lhs == rhs);
    }
};

struct UiElementHandle
{
    std::uint32_t index = 0;
    std::uint32_t generation = 0;

    [[nodiscard]] constexpr bool IsValid() const noexcept
    {
        return index != 0 && generation != 0;
    }

    friend constexpr bool operator==(UiElementHandle lhs, UiElementHandle rhs) noexcept
    {
        return lhs.index == rhs.index && lhs.generation == rhs.generation;
    }

    friend constexpr bool operator!=(UiElementHandle lhs, UiElementHandle rhs) noexcept
    {
        return !(lhs == rhs);
    }
};

using UiNameId = std::uint32_t;
inline constexpr UiNameId kInvalidUiNameId = 0;

enum class UiElementKind : std::uint8_t
{
    Panel = 0,
    Label,
    Button,
    Image,
    Checkbox,
    Combo,
    TextEdit,
    ScrollView,
    Custom,
};

enum class UiDirtyFlags : std::uint8_t
{
    None = 0,
    Structure = 1u << 0,
    Style = 1u << 1,
    Layout = 1u << 2,
    Paint = 1u << 3,
    All = 0x0F,
};

[[nodiscard]] constexpr UiDirtyFlags operator|(UiDirtyFlags lhs, UiDirtyFlags rhs) noexcept
{
    return static_cast<UiDirtyFlags>(static_cast<std::uint8_t>(lhs) | static_cast<std::uint8_t>(rhs));
}

[[nodiscard]] constexpr UiDirtyFlags operator&(UiDirtyFlags lhs, UiDirtyFlags rhs) noexcept
{
    return static_cast<UiDirtyFlags>(static_cast<std::uint8_t>(lhs) & static_cast<std::uint8_t>(rhs));
}

[[nodiscard]] constexpr UiDirtyFlags operator~(UiDirtyFlags value) noexcept
{
    return static_cast<UiDirtyFlags>(~static_cast<std::uint8_t>(value));
}

constexpr UiDirtyFlags& operator|=(UiDirtyFlags& lhs, UiDirtyFlags rhs) noexcept
{
    lhs = lhs | rhs;
    return lhs;
}

constexpr UiDirtyFlags& operator&=(UiDirtyFlags& lhs, UiDirtyFlags rhs) noexcept
{
    lhs = lhs & rhs;
    return lhs;
}

[[nodiscard]] constexpr bool HasFlag(UiDirtyFlags value, UiDirtyFlags flag) noexcept
{
    return (value & flag) == flag;
}

class UiElement
{
public:
    UiElement() noexcept = default;

    explicit constexpr UiElement(UiElementHandle handle) noexcept
        : handle_(handle)
    {
    }

    [[nodiscard]] UiId Id() const noexcept { return id_; }
    [[nodiscard]] UiElementHandle Handle() const noexcept { return handle_; }
    [[nodiscard]] bool IsValid() const noexcept { return handle_.IsValid(); }
    [[nodiscard]] UiElementKind Kind() const noexcept { return kind_; }
    [[nodiscard]] UiElementHandle Parent() const noexcept { return parent_; }
    [[nodiscard]] UiElementHandle FirstChild() const noexcept { return firstChild_; }
    [[nodiscard]] UiElementHandle LastChild() const noexcept { return lastChild_; }
    [[nodiscard]] UiElementHandle PreviousSibling() const noexcept { return previousSibling_; }
    [[nodiscard]] UiElementHandle NextSibling() const noexcept { return nextSibling_; }
    [[nodiscard]] std::string_view Name() const noexcept { return name_; }
    [[nodiscard]] const std::vector<UiNameId>& Classes() const noexcept { return classes_; }
    [[nodiscard]] UiDirtyFlags Dirty() const noexcept { return dirty_; }
    [[nodiscard]] std::uint32_t InlineStyle() const noexcept { return inlineStyle_; }

private:
    friend class UiDocument;

    UiId id_{};
    UiElementHandle handle_{};
    UiElementHandle parent_{};
    UiElementHandle firstChild_{};
    UiElementHandle lastChild_{};
    UiElementHandle previousSibling_{};
    UiElementHandle nextSibling_{};
    UiElementKind kind_ = UiElementKind::Panel;
    UiDirtyFlags dirty_ = UiDirtyFlags::None;
    std::uint32_t inlineStyle_ = 0;
    std::vector<UiNameId> classes_;
    std::string name_;
};

[[nodiscard]] inline const char* UiElementKindName(UiElementKind kind) noexcept
{
    switch (kind)
    {
    case UiElementKind::Panel: return "panel";
    case UiElementKind::Label: return "label";
    case UiElementKind::Button: return "button";
    case UiElementKind::Image: return "image";
    case UiElementKind::Checkbox: return "checkbox";
    case UiElementKind::Combo: return "combo";
    case UiElementKind::TextEdit: return "textedit";
    case UiElementKind::ScrollView: return "scrollview";
    case UiElementKind::Custom: return "custom";
    }
    return "custom";
}

} // namespace se::ui
