#pragma once

#include "FunctionLayer/UI/Core/UiElement.h"

#include <cstdint>
#include <memory>
#include <string>
#include <string_view>
#include <vector>

namespace se::ui
{

struct UiColor
{
    float r = 0.0f;
    float g = 0.0f;
    float b = 0.0f;
    float a = 1.0f;

    bool operator==(const UiColor&) const = default;
};

enum class UiDisplay : std::uint8_t
{
    Flex = 0,
    None,
};

enum class UiFlexDirection : std::uint8_t
{
    Row = 0,
    Column,
};

enum class UiJustifyContent : std::uint8_t
{
    FlexStart = 0,
    Center,
    FlexEnd,
    SpaceBetween,
};

enum class UiAlignItems : std::uint8_t
{
    FlexStart = 0,
    Center,
    FlexEnd,
    Stretch,
};

enum class UiPosition : std::uint8_t
{
    Relative = 0,
    Absolute,
};

enum class UiPointerEvents : std::uint8_t
{
    Auto = 0,
    None,
};

enum class UiOverflow : std::uint8_t
{
    Visible = 0,
    Hidden,
    Scroll,
};

enum class UiUnit : std::uint8_t
{
    Auto = 0,
    Px,
    Percent,
};

// Auto 表示不指定；Percent 的 value 是 0..100 的百分比。
struct UiLength
{
    UiUnit unit = UiUnit::Auto;
    float value = 0.0f;

    [[nodiscard]] static constexpr UiLength Auto() noexcept
    {
        return {};
    }

    [[nodiscard]] static constexpr UiLength Px(float pixels) noexcept
    {
        return {UiUnit::Px, pixels};
    }

    [[nodiscard]] static constexpr UiLength Percent(float percent) noexcept
    {
        return {UiUnit::Percent, percent};
    }

    [[nodiscard]] constexpr bool IsAuto() const noexcept
    {
        return unit == UiUnit::Auto;
    }

    bool operator==(const UiLength&) const = default;
};

struct UiEdges
{
    float left = 0.0f;
    float top = 0.0f;
    float right = 0.0f;
    float bottom = 0.0f;

    bool operator==(const UiEdges&) const = default;
};

// v1 只实现布局子集；颜色/边框/字体族等在阶段 E 扩展。
struct UiStyle
{
    UiDisplay display = UiDisplay::Flex;
    UiFlexDirection flexDirection = UiFlexDirection::Row;
    UiJustifyContent justifyContent = UiJustifyContent::FlexStart;
    UiAlignItems alignItems = UiAlignItems::Stretch;
    UiPosition position = UiPosition::Relative;
    UiOverflow overflow = UiOverflow::Visible;
    UiPointerEvents pointerEvents = UiPointerEvents::Auto;

    float gap = 0.0f;
    UiEdges padding{};
    UiEdges margin{};

    UiLength width{};
    UiLength height{};
    UiLength minWidth{};
    UiLength minHeight{};
    UiLength maxWidth{};
    UiLength maxHeight{};
    UiLength flexBasis{};

    UiLength left{};
    UiLength top{};
    UiLength right{};
    UiLength bottom{};

    float flexGrow = 0.0f;
    float flexShrink = 1.0f;
    float fontSize = 14.0f;

    UiColor background{};
    UiColor color{1.0f, 1.0f, 1.0f, 1.0f};
    UiColor borderColor{};
    float borderWidth = 0.0f;
    float cornerRadius = 0.0f;
    std::string image;

    bool operator==(const UiStyle&) const = default;
};

using UiStyleMask = std::uint64_t;

namespace UiStyleBits
{
enum : UiStyleMask
{
    Background = 1ull << 0,
    Color = 1ull << 1,
    BorderColor = 1ull << 2,
    BorderWidth = 1ull << 3,
    CornerRadius = 1ull << 4,
    Display = 1ull << 5,
    FlexDirection = 1ull << 6,
    JustifyContent = 1ull << 7,
    AlignItems = 1ull << 8,
    Position = 1ull << 9,
    Overflow = 1ull << 10,
    Gap = 1ull << 11,
    Padding = 1ull << 12,
    Margin = 1ull << 13,
    Width = 1ull << 14,
    Height = 1ull << 15,
    MinWidth = 1ull << 16,
    MinHeight = 1ull << 17,
    MaxWidth = 1ull << 18,
    MaxHeight = 1ull << 19,
    FlexBasis = 1ull << 20,
    Left = 1ull << 21,
    Top = 1ull << 22,
    Right = 1ull << 23,
    Bottom = 1ull << 24,
    FlexGrow = 1ull << 25,
    FlexShrink = 1ull << 26,
    FontSize = 1ull << 27,
    Image = 1ull << 28,
    PointerEvents = 1ull << 29,
};
} // namespace UiStyleBits

// 内联样式目前用“与默认值不同的字段”推导；显式写回默认值的字段不会被标记。
[[nodiscard]] inline UiStyleMask DiffStyleMask(const UiStyle& style)
{
    const UiStyle defaults{};
    UiStyleMask mask = 0;
    if (!(style.background == defaults.background)) mask |= UiStyleBits::Background;
    if (!(style.color == defaults.color)) mask |= UiStyleBits::Color;
    if (!(style.borderColor == defaults.borderColor)) mask |= UiStyleBits::BorderColor;
    if (style.borderWidth != defaults.borderWidth) mask |= UiStyleBits::BorderWidth;
    if (style.cornerRadius != defaults.cornerRadius) mask |= UiStyleBits::CornerRadius;
    if (style.display != defaults.display) mask |= UiStyleBits::Display;
    if (style.flexDirection != defaults.flexDirection) mask |= UiStyleBits::FlexDirection;
    if (style.justifyContent != defaults.justifyContent) mask |= UiStyleBits::JustifyContent;
    if (style.alignItems != defaults.alignItems) mask |= UiStyleBits::AlignItems;
    if (style.position != defaults.position) mask |= UiStyleBits::Position;
    if (style.overflow != defaults.overflow) mask |= UiStyleBits::Overflow;
    if (style.gap != defaults.gap) mask |= UiStyleBits::Gap;
    if (!(style.padding == defaults.padding)) mask |= UiStyleBits::Padding;
    if (!(style.margin == defaults.margin)) mask |= UiStyleBits::Margin;
    if (!(style.width == defaults.width)) mask |= UiStyleBits::Width;
    if (!(style.height == defaults.height)) mask |= UiStyleBits::Height;
    if (!(style.minWidth == defaults.minWidth)) mask |= UiStyleBits::MinWidth;
    if (!(style.minHeight == defaults.minHeight)) mask |= UiStyleBits::MinHeight;
    if (!(style.maxWidth == defaults.maxWidth)) mask |= UiStyleBits::MaxWidth;
    if (!(style.maxHeight == defaults.maxHeight)) mask |= UiStyleBits::MaxHeight;
    if (!(style.flexBasis == defaults.flexBasis)) mask |= UiStyleBits::FlexBasis;
    if (!(style.left == defaults.left)) mask |= UiStyleBits::Left;
    if (!(style.top == defaults.top)) mask |= UiStyleBits::Top;
    if (!(style.right == defaults.right)) mask |= UiStyleBits::Right;
    if (!(style.bottom == defaults.bottom)) mask |= UiStyleBits::Bottom;
    if (style.flexGrow != defaults.flexGrow) mask |= UiStyleBits::FlexGrow;
    if (style.flexShrink != defaults.flexShrink) mask |= UiStyleBits::FlexShrink;
    if (style.fontSize != defaults.fontSize) mask |= UiStyleBits::FontSize;
    if (style.image != defaults.image) mask |= UiStyleBits::Image;
    if (style.pointerEvents != defaults.pointerEvents) mask |= UiStyleBits::PointerEvents;
    return mask;
}

inline void ApplyStyleMask(UiStyle& target, const UiStyle& source, UiStyleMask mask)
{
    if (mask & UiStyleBits::Background) target.background = source.background;
    if (mask & UiStyleBits::Color) target.color = source.color;
    if (mask & UiStyleBits::BorderColor) target.borderColor = source.borderColor;
    if (mask & UiStyleBits::BorderWidth) target.borderWidth = source.borderWidth;
    if (mask & UiStyleBits::CornerRadius) target.cornerRadius = source.cornerRadius;
    if (mask & UiStyleBits::Display) target.display = source.display;
    if (mask & UiStyleBits::FlexDirection) target.flexDirection = source.flexDirection;
    if (mask & UiStyleBits::JustifyContent) target.justifyContent = source.justifyContent;
    if (mask & UiStyleBits::AlignItems) target.alignItems = source.alignItems;
    if (mask & UiStyleBits::Position) target.position = source.position;
    if (mask & UiStyleBits::Overflow) target.overflow = source.overflow;
    if (mask & UiStyleBits::Gap) target.gap = source.gap;
    if (mask & UiStyleBits::Padding) target.padding = source.padding;
    if (mask & UiStyleBits::Margin) target.margin = source.margin;
    if (mask & UiStyleBits::Width) target.width = source.width;
    if (mask & UiStyleBits::Height) target.height = source.height;
    if (mask & UiStyleBits::MinWidth) target.minWidth = source.minWidth;
    if (mask & UiStyleBits::MinHeight) target.minHeight = source.minHeight;
    if (mask & UiStyleBits::MaxWidth) target.maxWidth = source.maxWidth;
    if (mask & UiStyleBits::MaxHeight) target.maxHeight = source.maxHeight;
    if (mask & UiStyleBits::FlexBasis) target.flexBasis = source.flexBasis;
    if (mask & UiStyleBits::Left) target.left = source.left;
    if (mask & UiStyleBits::Top) target.top = source.top;
    if (mask & UiStyleBits::Right) target.right = source.right;
    if (mask & UiStyleBits::Bottom) target.bottom = source.bottom;
    if (mask & UiStyleBits::FlexGrow) target.flexGrow = source.flexGrow;
    if (mask & UiStyleBits::FlexShrink) target.flexShrink = source.flexShrink;
    if (mask & UiStyleBits::FontSize) target.fontSize = source.fontSize;
    if (mask & UiStyleBits::Image) target.image = source.image;
    if (mask & UiStyleBits::PointerEvents) target.pointerEvents = source.pointerEvents;
}

enum class UiInteractionState : std::uint8_t
{
    None = 0,
    Hover = 1u << 0,
    Active = 1u << 1,
    Focus = 1u << 2,
    Disabled = 1u << 3,
};

[[nodiscard]] constexpr UiInteractionState operator|(UiInteractionState lhs, UiInteractionState rhs) noexcept
{
    return static_cast<UiInteractionState>(static_cast<std::uint8_t>(lhs) | static_cast<std::uint8_t>(rhs));
}

[[nodiscard]] constexpr bool HasState(UiInteractionState value, UiInteractionState flag) noexcept
{
    return (static_cast<std::uint8_t>(value) & static_cast<std::uint8_t>(flag)) ==
        static_cast<std::uint8_t>(flag);
}

enum class UiThemeColor : std::uint8_t
{
    Background = 0,
    Surface,
    SurfaceHover,
    Border,
    Text,
    TextMuted,
    Accent,
    Danger,
};

enum class UiThemeMetric : std::uint8_t
{
    SpacingXs = 0,
    SpacingSm,
    SpacingMd,
    SpacingLg,
    SpacingXl,
    RadiusSm,
    RadiusMd,
    RadiusLg,
    FontSm,
    FontMd,
    FontLg,
    FontXl,
    BorderWidth,
    FocusRing,
};

// 核心只定义通用 Dark token；编辑器色板由 Tool 层映射，不进入这里。
struct UiTheme
{
    UiColor background{0.078f, 0.086f, 0.102f, 1.0f};
    UiColor surface{0.118f, 0.133f, 0.165f, 1.0f};
    UiColor surfaceHover{0.145f, 0.169f, 0.212f, 1.0f};
    UiColor border{0.165f, 0.184f, 0.227f, 1.0f};
    UiColor text{0.949f, 0.957f, 0.973f, 1.0f};
    UiColor textMuted{0.604f, 0.643f, 0.698f, 1.0f};
    UiColor accent{0.310f, 0.549f, 1.0f, 1.0f};
    UiColor danger{0.898f, 0.282f, 0.302f, 1.0f};

    float spacingXs = 4.0f;
    float spacingSm = 8.0f;
    float spacingMd = 12.0f;
    float spacingLg = 16.0f;
    float spacingXl = 24.0f;
    float radiusSm = 4.0f;
    float radiusMd = 6.0f;
    float radiusLg = 8.0f;
    float fontSm = 12.0f;
    float fontMd = 14.0f;
    float fontLg = 18.0f;
    float fontXl = 24.0f;
    float borderWidth = 1.0f;
    float focusRing = 2.0f;

    [[nodiscard]] static UiTheme Dark() noexcept;
    [[nodiscard]] UiColor Color(UiThemeColor token) const noexcept;
    [[nodiscard]] float Metric(UiThemeMetric token) const noexcept;

    bool operator==(const UiTheme&) const = default;
};

struct UiStyleDiagnostic
{
    std::string selector;
    std::string property;
    std::string message;
};

struct UiStyleStats
{
    std::uint32_t evaluations = 0;
    std::uint32_t cacheHits = 0;
    std::uint32_t cacheMisses = 0;
    std::uint32_t ruleMatches = 0;
};

class UiDocument;

// 选择器/级联/计算样式缓存。样式表输入是 styles 对象（选择器 -> 声明）。
// 计算样式按 (元素, 交互状态) 缓存；主题切换和文档样式修订会清空缓存。
class UiStyleEngine
{
public:
    explicit UiStyleEngine(UiDocument& document, UiTheme theme = UiTheme::Dark());
    ~UiStyleEngine();

    UiStyleEngine(UiStyleEngine&&) noexcept;
    UiStyleEngine& operator=(UiStyleEngine&&) noexcept;
    UiStyleEngine(const UiStyleEngine&) = delete;
    UiStyleEngine& operator=(const UiStyleEngine&) = delete;

    bool LoadStylesJson(std::string_view stylesJson, std::string& error);
    [[nodiscard]] const UiStyle& Compute(UiElementHandle element,
                                        UiInteractionState state = UiInteractionState::None);
    void SetTheme(const UiTheme& theme);
    [[nodiscard]] const UiTheme& Theme() const noexcept;
    void InvalidateAll();

    [[nodiscard]] UiStyleStats Stats() const noexcept;
    [[nodiscard]] const std::vector<UiStyleDiagnostic>& Diagnostics() const noexcept;

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

} // namespace se::ui
