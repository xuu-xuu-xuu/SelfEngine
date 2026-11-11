#include "FunctionLayer/UI/Core/UiStyle.h"

#include "FunctionLayer/UI/Core/UiDocument.h"

#include <nlohmann/json.hpp>

#include <algorithm>
#include <array>
#include <cctype>
#include <cmath>
#include <cstdint>
#include <string>
#include <string_view>
#include <unordered_map>
#include <utility>
#include <vector>

namespace se::ui
{

UiTheme UiTheme::Dark() noexcept
{
    return {};
}

UiColor UiTheme::Color(UiThemeColor token) const noexcept
{
    switch (token)
    {
    case UiThemeColor::Background: return background;
    case UiThemeColor::Surface: return surface;
    case UiThemeColor::SurfaceHover: return surfaceHover;
    case UiThemeColor::Border: return border;
    case UiThemeColor::Text: return text;
    case UiThemeColor::TextMuted: return textMuted;
    case UiThemeColor::Accent: return accent;
    case UiThemeColor::Danger: return danger;
    }
    return {};
}

float UiTheme::Metric(UiThemeMetric token) const noexcept
{
    switch (token)
    {
    case UiThemeMetric::SpacingXs: return spacingXs;
    case UiThemeMetric::SpacingSm: return spacingSm;
    case UiThemeMetric::SpacingMd: return spacingMd;
    case UiThemeMetric::SpacingLg: return spacingLg;
    case UiThemeMetric::SpacingXl: return spacingXl;
    case UiThemeMetric::RadiusSm: return radiusSm;
    case UiThemeMetric::RadiusMd: return radiusMd;
    case UiThemeMetric::RadiusLg: return radiusLg;
    case UiThemeMetric::FontSm: return fontSm;
    case UiThemeMetric::FontMd: return fontMd;
    case UiThemeMetric::FontLg: return fontLg;
    case UiThemeMetric::FontXl: return fontXl;
    case UiThemeMetric::BorderWidth: return borderWidth;
    case UiThemeMetric::FocusRing: return focusRing;
    }
    return 0.0f;
}

namespace
{
using Json = nlohmann::ordered_json;

std::string_view Trim(std::string_view value) noexcept
{
    while (!value.empty() && std::isspace(static_cast<unsigned char>(value.front())))
    {
        value.remove_prefix(1);
    }
    while (!value.empty() && std::isspace(static_cast<unsigned char>(value.back())))
    {
        value.remove_suffix(1);
    }
    return value;
}

bool IsIdentifierCharacter(char character) noexcept
{
    const unsigned char value = static_cast<unsigned char>(character);
    return std::isalnum(value) != 0 || character == '_' || character == '-';
}

bool ParseHexColor(std::string_view text, UiColor& color) noexcept
{
    if (text.size() < 4 || text.front() != '#')
    {
        return false;
    }
    std::string digits(text.substr(1));
    for (char character : digits)
    {
        if (!std::isxdigit(static_cast<unsigned char>(character)))
        {
            return false;
        }
    }
    if (digits.size() == 3)
    {
        digits = {digits[0], digits[0], digits[1], digits[1], digits[2], digits[2]};
    }
    else if (digits.size() == 4)
    {
        digits = {digits[0], digits[0], digits[1], digits[1], digits[2], digits[2], digits[3], digits[3]};
    }
    if (digits.size() != 6 && digits.size() != 8)
    {
        return false;
    }
    const auto component = [&digits](std::size_t offset) {
        return static_cast<float>(std::stoi(digits.substr(offset, 2), nullptr, 16)) / 255.0f;
    };
    color = UiColor{component(0), component(2), component(4),
                    digits.size() == 8 ? component(6) : 1.0f};
    return true;
}

bool ColorToken(std::string_view name, UiThemeColor& token) noexcept
{
    if (name == "background") { token = UiThemeColor::Background; return true; }
    if (name == "surface") { token = UiThemeColor::Surface; return true; }
    if (name == "surface-hover" || name == "surfaceHover") { token = UiThemeColor::SurfaceHover; return true; }
    if (name == "border") { token = UiThemeColor::Border; return true; }
    if (name == "text") { token = UiThemeColor::Text; return true; }
    if (name == "text-muted" || name == "textMuted") { token = UiThemeColor::TextMuted; return true; }
    if (name == "accent") { token = UiThemeColor::Accent; return true; }
    if (name == "danger") { token = UiThemeColor::Danger; return true; }
    return false;
}

bool MetricToken(std::string_view name, UiThemeMetric& token) noexcept
{
    if (name == "spacing-xs") { token = UiThemeMetric::SpacingXs; return true; }
    if (name == "spacing-sm") { token = UiThemeMetric::SpacingSm; return true; }
    if (name == "spacing-md") { token = UiThemeMetric::SpacingMd; return true; }
    if (name == "spacing-lg") { token = UiThemeMetric::SpacingLg; return true; }
    if (name == "spacing-xl") { token = UiThemeMetric::SpacingXl; return true; }
    if (name == "radius-sm") { token = UiThemeMetric::RadiusSm; return true; }
    if (name == "radius-md") { token = UiThemeMetric::RadiusMd; return true; }
    if (name == "radius-lg") { token = UiThemeMetric::RadiusLg; return true; }
    if (name == "font-sm") { token = UiThemeMetric::FontSm; return true; }
    if (name == "font-md") { token = UiThemeMetric::FontMd; return true; }
    if (name == "font-lg") { token = UiThemeMetric::FontLg; return true; }
    if (name == "font-xl") { token = UiThemeMetric::FontXl; return true; }
    if (name == "border-width") { token = UiThemeMetric::BorderWidth; return true; }
    if (name == "focus-ring") { token = UiThemeMetric::FocusRing; return true; }
    return false;
}

bool ParseColor(const Json& value, const UiTheme& theme, UiColor& color)
{
    if (!value.is_string())
    {
        return false;
    }
    const std::string text = value.get<std::string>();
    if (text.size() > 1 && text.front() == '$')
    {
        UiThemeColor token = UiThemeColor::Background;
        if (!ColorToken(std::string_view(text).substr(1), token))
        {
            return false;
        }
        color = theme.Color(token);
        return true;
    }
    return ParseHexColor(text, color);
}

bool ParseNumber(const Json& value, const UiTheme& theme, float& number)
{
    if (value.is_number())
    {
        number = value.get<float>();
        return std::isfinite(number);
    }
    if (!value.is_string())
    {
        return false;
    }
    const std::string text = value.get<std::string>();
    if (text.size() > 1 && text.front() == '$')
    {
        UiThemeMetric token = UiThemeMetric::SpacingMd;
        if (MetricToken(std::string_view(text).substr(1), token))
        {
            number = theme.Metric(token);
            return true;
        }
    }
    return false;
}

bool ParseLength(const Json& value, const UiTheme& theme, UiLength& length)
{
    if (value.is_number())
    {
        length = UiLength::Px(value.get<float>());
        return std::isfinite(length.value);
    }
    if (!value.is_string())
    {
        return false;
    }
    const std::string text = value.get<std::string>();
    if (text == "auto")
    {
        length = UiLength::Auto();
        return true;
    }
    if (text.size() > 1 && text.front() == '$')
    {
        UiThemeMetric token = UiThemeMetric::SpacingMd;
        if (MetricToken(std::string_view(text).substr(1), token))
        {
            length = UiLength::Px(theme.Metric(token));
            return true;
        }
        return false;
    }
    if (!text.empty() && text.back() == '%')
    {
        try
        {
            length = UiLength::Percent(std::stof(text.substr(0, text.size() - 1)));
            return std::isfinite(length.value);
        }
        catch (...)
        {
            return false;
        }
    }
    return false;
}

bool ParseEdges(const Json& value, const UiTheme& theme, UiEdges& edges)
{
    float number = 0.0f;
    if (ParseNumber(value, theme, number))
    {
        edges = UiEdges{number, number, number, number};
        return true;
    }
    if (!value.is_array())
    {
        return false;
    }
    std::vector<float> numbers;
    numbers.reserve(value.size());
    for (const Json& item : value)
    {
        float component = 0.0f;
        if (!ParseNumber(item, theme, component))
        {
            return false;
        }
        numbers.push_back(component);
    }
    if (numbers.size() == 1)
    {
        edges = UiEdges{numbers[0], numbers[0], numbers[0], numbers[0]};
        return true;
    }
    if (numbers.size() == 2)
    {
        edges = UiEdges{numbers[1], numbers[0], numbers[1], numbers[0]};
        return true;
    }
    if (numbers.size() == 4)
    {
        edges = UiEdges{numbers[3], numbers[0], numbers[1], numbers[2]};
        return true;
    }
    return false;
}

bool ParseEnum(const Json& value, std::string_view property, UiStyle& style)
{
    if (!value.is_string())
    {
        return false;
    }
    const std::string text = value.get<std::string>();
    if (property == "display")
    {
        if (text == "flex") style.display = UiDisplay::Flex;
        else if (text == "none") style.display = UiDisplay::None;
        else return false;
        return true;
    }
    if (property == "flex-direction")
    {
        if (text == "row") style.flexDirection = UiFlexDirection::Row;
        else if (text == "column") style.flexDirection = UiFlexDirection::Column;
        else return false;
        return true;
    }
    if (property == "justify-content")
    {
        if (text == "flex-start") style.justifyContent = UiJustifyContent::FlexStart;
        else if (text == "center") style.justifyContent = UiJustifyContent::Center;
        else if (text == "flex-end") style.justifyContent = UiJustifyContent::FlexEnd;
        else if (text == "space-between") style.justifyContent = UiJustifyContent::SpaceBetween;
        else return false;
        return true;
    }
    if (property == "align-items")
    {
        if (text == "flex-start") style.alignItems = UiAlignItems::FlexStart;
        else if (text == "center") style.alignItems = UiAlignItems::Center;
        else if (text == "flex-end") style.alignItems = UiAlignItems::FlexEnd;
        else if (text == "stretch") style.alignItems = UiAlignItems::Stretch;
        else return false;
        return true;
    }
    if (property == "position")
    {
        if (text == "relative") style.position = UiPosition::Relative;
        else if (text == "absolute") style.position = UiPosition::Absolute;
        else return false;
        return true;
    }
    if (property == "pointer-events")
    {
        if (text == "auto") style.pointerEvents = UiPointerEvents::Auto;
        else if (text == "none") style.pointerEvents = UiPointerEvents::None;
        else return false;
        return true;
    }
    if (property == "overflow")
    {
        if (text == "visible") style.overflow = UiOverflow::Visible;
        else if (text == "hidden") style.overflow = UiOverflow::Hidden;
        else if (text == "scroll") style.overflow = UiOverflow::Scroll;
        else return false;
        return true;
    }
    return false;
}

bool ApplyProperty(std::string_view property, const Json& value, const UiTheme& theme, UiStyle& style)
{
    if (property == "display" || property == "flex-direction" || property == "justify-content" ||
        property == "align-items" || property == "position" || property == "overflow")
    {
        return ParseEnum(value, property, style);
    }
    float number = 0.0f;
    if (property == "gap")
    {
        if (!ParseNumber(value, theme, number)) return false;
        style.gap = number;
        return true;
    }
    if (property == "padding")
    {
        return ParseEdges(value, theme, style.padding);
    }
    if (property == "margin")
    {
        return ParseEdges(value, theme, style.margin);
    }
    if (property == "flex-grow")
    {
        if (!ParseNumber(value, theme, number)) return false;
        style.flexGrow = number;
        return true;
    }
    if (property == "flex-shrink")
    {
        if (!ParseNumber(value, theme, number)) return false;
        style.flexShrink = number;
        return true;
    }
    if (property == "font-size")
    {
        if (!ParseNumber(value, theme, number)) return false;
        style.fontSize = number;
        return true;
    }
    if (property == "border-width")
    {
        if (!ParseNumber(value, theme, number)) return false;
        style.borderWidth = number;
        return true;
    }
    if (property == "radius" || property == "corner-radius")
    {
        if (!ParseNumber(value, theme, number)) return false;
        style.cornerRadius = number;
        return true;
    }
    if (property == "image")
    {
        if (!value.is_string()) return false;
        style.image = value.get<std::string>();
        return true;
    }
    if (property == "background" || property == "background-color")
    {
        return ParseColor(value, theme, style.background);
    }
    if (property == "color")
    {
        return ParseColor(value, theme, style.color);
    }
    if (property == "border-color")
    {
        return ParseColor(value, theme, style.borderColor);
    }
    if (property == "border")
    {
        if (value.is_array() && value.size() == 2)
        {
            return ParseNumber(value[0], theme, style.borderWidth) && ParseColor(value[1], theme, style.borderColor);
        }
        if (!value.is_string())
        {
            return false;
        }
        std::string text = value.get<std::string>();
        const std::size_t split = text.find_first_of(" \t");
        if (split == std::string::npos)
        {
            return false;
        }
        const std::string widthText = text.substr(0, split);
        const std::string colorText(Trim(std::string_view(text).substr(split + 1)));
        try
        {
            style.borderWidth = std::stof(widthText);
        }
        catch (...)
        {
            return false;
        }
        return ParseColor(Json(colorText), theme, style.borderColor);
    }
    if (property == "width") return ParseLength(value, theme, style.width);
    if (property == "height") return ParseLength(value, theme, style.height);
    if (property == "min-width") return ParseLength(value, theme, style.minWidth);
    if (property == "min-height") return ParseLength(value, theme, style.minHeight);
    if (property == "max-width") return ParseLength(value, theme, style.maxWidth);
    if (property == "max-height") return ParseLength(value, theme, style.maxHeight);
    if (property == "flex-basis") return ParseLength(value, theme, style.flexBasis);
    if (property == "left") return ParseLength(value, theme, style.left);
    if (property == "top") return ParseLength(value, theme, style.top);
    if (property == "right") return ParseLength(value, theme, style.right);
    if (property == "bottom") return ParseLength(value, theme, style.bottom);
    return false;
}

bool KnownProperty(std::string_view property) noexcept
{
    return property == "display" || property == "flex-direction" || property == "justify-content" ||
        property == "align-items" || property == "position" || property == "overflow" || property == "pointer-events" ||
        property == "gap" || property == "padding" || property == "margin" ||
        property == "flex-grow" || property == "flex-shrink" || property == "font-size" ||
        property == "border-width" || property == "radius" || property == "corner-radius" ||
        property == "background" || property == "background-color" || property == "color" || property == "image" ||
        property == "border-color" || property == "border" ||
        property == "width" || property == "height" || property == "min-width" ||
        property == "min-height" || property == "max-width" || property == "max-height" ||
        property == "flex-basis" || property == "left" || property == "top" ||
        property == "right" || property == "bottom";
}

enum class Combinator : std::uint8_t
{
    Descendant = 0,
    Child,
};

struct SimpleSelector
{
    std::uint32_t type = 0;
    std::uint32_t id = 0;
    std::vector<std::uint32_t> classes;
    std::uint8_t pseudo = 0;
};

struct Selector
{
    std::vector<SimpleSelector> parts;
    std::vector<Combinator> combinators;
    std::uint32_t idCount = 0;
    std::uint32_t classCount = 0;
    std::uint32_t typeCount = 0;
};

struct Rule
{
    std::vector<Selector> selectors;
    Json declarations;
    std::string text;
    std::size_t order = 0;
};

} // namespace

struct UiStyleEngine::Impl
{
    UiDocument* document = nullptr;
    UiTheme theme;
    std::vector<Rule> rules;
    std::vector<UiStyleDiagnostic> diagnostics;
    std::unordered_map<std::string, std::uint32_t> names;
    std::uint32_t nextName = 1;
    std::array<std::uint32_t, 9> typeIds{};
    std::unordered_map<UiNameId, std::uint32_t> classIds;
    std::unordered_map<std::uint64_t, std::uint32_t> elementNames;
    std::array<std::unordered_map<std::uint64_t, UiStyle>, 16> cache;
    std::uint64_t lastStyleRevision = ~std::uint64_t{0};
    UiStyleStats stats{};

    std::uint32_t Intern(std::string_view value)
    {
        if (value.empty())
        {
            return 0;
        }
        const std::string key(value);
        const auto found = names.find(key);
        if (found != names.end())
        {
            return found->second;
        }
        const std::uint32_t id = nextName++;
        names.emplace(key, id);
        return id;
    }

    std::uint32_t TypeId(UiElementKind kind)
    {
        const std::size_t index = static_cast<std::size_t>(kind);
        if (typeIds[index] == 0)
        {
            typeIds[index] = Intern(UiElementKindName(kind));
        }
        return typeIds[index];
    }

    std::uint32_t ClassId(UiNameId name)
    {
        if (name == kInvalidUiNameId)
        {
            return 0;
        }
        const auto found = classIds.find(name);
        if (found != classIds.end())
        {
            return found->second;
        }
        const std::uint32_t id = Intern(document->ClassName(name));
        classIds.emplace(name, id);
        return id;
    }

    std::uint32_t NameId(const UiElement& element)
    {
        const std::uint64_t key = (static_cast<std::uint64_t>(element.Handle().index) << 32) |
            static_cast<std::uint64_t>(element.Handle().generation);
        const auto found = elementNames.find(key);
        if (found != elementNames.end())
        {
            return found->second;
        }
        const std::uint32_t id = Intern(element.Name());
        elementNames.emplace(key, id);
        return id;
    }

    bool ParseSelector(std::string_view text, Selector& selector);
    bool ParseSelectorList(std::string_view text, std::vector<Selector>& selectors);
    bool MatchesSimple(const SimpleSelector& simple, const UiElement& element, std::uint8_t state);
    bool MatchesSelector(const Selector& selector, UiElementHandle handle, std::uint8_t state);
    void ClearCache();
};

bool UiStyleEngine::Impl::ParseSelector(std::string_view text, Selector& selector)
{
    std::vector<std::string> compounds;
    std::vector<Combinator> combinators;
    std::size_t index = 0;
    bool haveCompound = false;
    bool childPending = false;
    while (index < text.size())
    {
        while (index < text.size() && std::isspace(static_cast<unsigned char>(text[index])))
        {
            ++index;
        }
        if (index >= text.size())
        {
            break;
        }
        if (text[index] == '>')
        {
            if (!haveCompound || childPending)
            {
                diagnostics.push_back({std::string(text), "", "invalid child combinator"});
                return false;
            }
            childPending = true;
            ++index;
            continue;
        }
        const std::size_t start = index;
        while (index < text.size() && !std::isspace(static_cast<unsigned char>(text[index])) && text[index] != '>')
        {
            ++index;
        }
        const std::string compound(text.substr(start, index - start));
        if (haveCompound)
        {
            combinators.push_back(childPending ? Combinator::Child : Combinator::Descendant);
        }
        compounds.push_back(compound);
        haveCompound = true;
        childPending = false;
    }
    if (!haveCompound || childPending)
    {
        diagnostics.push_back({std::string(text), "", "empty or dangling selector"});
        return false;
    }

    selector = {};
    selector.parts.reserve(compounds.size());
    selector.combinators = combinators;
    for (const std::string& compound : compounds)
    {
        SimpleSelector simple;
        bool any = false;
        std::size_t cursor = 0;
        if (cursor < compound.size() && compound[cursor] != '.' && compound[cursor] != '#' && compound[cursor] != ':')
        {
            const std::size_t start = cursor;
            while (cursor < compound.size() && IsIdentifierCharacter(compound[cursor]))
            {
                ++cursor;
            }
            if (cursor == start)
            {
                diagnostics.push_back({std::string(text), "", "invalid type selector"});
                return false;
            }
            const std::string type = compound.substr(start, cursor - start);
            if (type != "*")
            {
                simple.type = Intern(type);
                ++selector.typeCount;
            }
            any = true;
        }
        while (cursor < compound.size())
        {
            const char marker = compound[cursor++];
            if (marker != '.' && marker != '#' && marker != ':')
            {
                diagnostics.push_back({std::string(text), "", "unexpected selector character"});
                return false;
            }
            const std::size_t start = cursor;
            while (cursor < compound.size() && IsIdentifierCharacter(compound[cursor]))
            {
                ++cursor;
            }
            if (cursor == start)
            {
                diagnostics.push_back({std::string(text), "", "empty selector name"});
                return false;
            }
            const std::string name = compound.substr(start, cursor - start);
            any = true;
            if (marker == '.')
            {
                simple.classes.push_back(Intern(name));
                ++selector.classCount;
            }
            else if (marker == '#')
            {
                simple.id = Intern(name);
                ++selector.idCount;
            }
            else
            {
                ++selector.classCount;
                if (name == "hover") simple.pseudo |= static_cast<std::uint8_t>(UiInteractionState::Hover);
                else if (name == "active") simple.pseudo |= static_cast<std::uint8_t>(UiInteractionState::Active);
                else if (name == "focus") simple.pseudo |= static_cast<std::uint8_t>(UiInteractionState::Focus);
                else if (name == "disabled") simple.pseudo |= static_cast<std::uint8_t>(UiInteractionState::Disabled);
                else
                {
                    diagnostics.push_back({std::string(text), ":" + name, "unknown pseudo state"});
                    return false;
                }
            }
        }
        if (!any)
        {
            diagnostics.push_back({std::string(text), "", "empty compound selector"});
            return false;
        }
        selector.parts.push_back(std::move(simple));
    }
    return true;
}

bool UiStyleEngine::Impl::ParseSelectorList(std::string_view text, std::vector<Selector>& selectors)
{
    std::size_t start = 0;
    while (start <= text.size())
    {
        const std::size_t comma = text.find(',', start);
        const std::string_view part = Trim(text.substr(start, comma == std::string_view::npos ? text.size() - start : comma - start));
        if (!part.empty())
        {
            Selector selector;
            if (ParseSelector(part, selector))
            {
                selectors.push_back(std::move(selector));
            }
        }
        if (comma == std::string_view::npos)
        {
            break;
        }
        start = comma + 1;
    }
    return !selectors.empty();
}

bool UiStyleEngine::Impl::MatchesSimple(const SimpleSelector& simple,
                                        const UiElement& element,
                                        std::uint8_t state)
{
    if (simple.type != 0 && TypeId(element.Kind()) != simple.type)
    {
        return false;
    }
    if (simple.id != 0 && NameId(element) != simple.id)
    {
        return false;
    }
    for (const std::uint32_t requiredClass : simple.classes)
    {
        bool found = false;
        for (const UiNameId elementClass : element.Classes())
        {
            if (ClassId(elementClass) == requiredClass)
            {
                found = true;
                break;
            }
        }
        if (!found)
        {
            return false;
        }
    }
    if ((state & simple.pseudo) != simple.pseudo)
    {
        return false;
    }
    return true;
}

bool UiStyleEngine::Impl::MatchesSelector(const Selector& selector, UiElementHandle handle, std::uint8_t state)
{
    const UiElement* element = document->Element(handle);
    if (!element || selector.parts.empty())
    {
        return false;
    }
    std::size_t partIndex = selector.parts.size() - 1;
    if (!MatchesSimple(selector.parts[partIndex], *element, state))
    {
        return false;
    }
    while (partIndex > 0)
    {
        const std::size_t previous = partIndex - 1;
        const Combinator combinator = selector.combinators[partIndex - 1];
        if (combinator == Combinator::Child)
        {
            element = document->Element(element->Parent());
            if (!element || !MatchesSimple(selector.parts[previous], *element, state))
            {
                return false;
            }
        }
        else
        {
            bool found = false;
            UiElementHandle parent = element->Parent();
            while (parent.IsValid())
            {
                const UiElement* candidate = document->Element(parent);
                if (!candidate)
                {
                    break;
                }
                if (MatchesSimple(selector.parts[previous], *candidate, state))
                {
                    element = candidate;
                    found = true;
                    break;
                }
                parent = candidate->Parent();
            }
            if (!found)
            {
                return false;
            }
        }
        partIndex = previous;
    }
    return true;
}

void UiStyleEngine::Impl::ClearCache()
{
    for (auto& bucket : cache)
    {
        bucket.clear();
    }
    elementNames.clear();
}

namespace
{
std::uint64_t Specificity(const Selector& selector) noexcept
{
    return (static_cast<std::uint64_t>(selector.idCount) << 40) |
        (static_cast<std::uint64_t>(selector.classCount) << 20) |
        static_cast<std::uint64_t>(selector.typeCount);
}

void ApplyDeclarations(const Json& declarations, const UiTheme& theme, UiStyle& style)
{
    for (auto declaration = declarations.begin(); declaration != declarations.end(); ++declaration)
    {
        ApplyProperty(declaration.key(), declaration.value(), theme, style);
    }
}
} // namespace

UiStyleEngine::UiStyleEngine(UiDocument& document, UiTheme theme)
    : impl_(std::make_unique<Impl>())
{
    impl_->document = &document;
    impl_->theme = std::move(theme);
}

UiStyleEngine::~UiStyleEngine() = default;

UiStyleEngine::UiStyleEngine(UiStyleEngine&&) noexcept = default;

UiStyleEngine& UiStyleEngine::operator=(UiStyleEngine&&) noexcept = default;

bool UiStyleEngine::LoadStylesJson(std::string_view stylesJson, std::string& error)
{
    error.clear();
    if (!impl_ || !impl_->document)
    {
        error = "UiStyleEngine: document is not available";
        return false;
    }

    Json parsed;
    try
    {
        parsed = Json::parse(stylesJson.begin(), stylesJson.end());
    }
    catch (const std::exception& exception)
    {
        error = std::string("UiStyleEngine: invalid JSON: ") + exception.what();
        return false;
    }
    if (!parsed.is_object())
    {
        error = "UiStyleEngine: styles must be an object";
        return false;
    }

    impl_->rules.clear();
    impl_->diagnostics.clear();
    impl_->ClearCache();
    impl_->lastStyleRevision = ~std::uint64_t{0};
    impl_->stats = {};

    for (auto entry = parsed.begin(); entry != parsed.end(); ++entry)
    {
        const std::string selectorText = entry.key();
        if (!entry.value().is_object())
        {
            impl_->diagnostics.push_back({selectorText, "", "declarations must be an object"});
            continue;
        }
        Rule rule;
        rule.text = selectorText;
        rule.declarations = entry.value();
        if (!impl_->ParseSelectorList(selectorText, rule.selectors))
        {
            continue;
        }
        for (auto declaration = rule.declarations.begin(); declaration != rule.declarations.end(); ++declaration)
        {
            if (!KnownProperty(declaration.key()))
            {
                impl_->diagnostics.push_back({selectorText, declaration.key(), "unknown property ignored"});
                continue;
            }
            UiStyle probe;
            if (!ApplyProperty(declaration.key(), declaration.value(), impl_->theme, probe))
            {
                impl_->diagnostics.push_back({selectorText, declaration.key(), "invalid value ignored"});
            }
        }
        rule.order = impl_->rules.size();
        impl_->rules.push_back(std::move(rule));
    }
    return true;
}

const UiStyle& UiStyleEngine::Compute(UiElementHandle element, UiInteractionState state)
{
    static const UiStyle fallback{};
    if (!impl_ || !impl_->document || !impl_->document->Element(element))
    {
        return fallback;
    }

    const std::uint64_t revision = impl_->document->StyleRevision();
    if (revision != impl_->lastStyleRevision)
    {
        impl_->ClearCache();
        impl_->lastStyleRevision = revision;
    }

    std::uint8_t stateBits = static_cast<std::uint8_t>(state);
    if (!impl_->document->Enabled(element))
    {
        stateBits |= static_cast<std::uint8_t>(UiInteractionState::Disabled);
    }
    stateBits &= 15u;
    const std::uint64_t key = (static_cast<std::uint64_t>(element.index) << 32) |
        static_cast<std::uint64_t>(element.generation);
    auto& bucket = impl_->cache[stateBits];
    const auto cached = bucket.find(key);
    if (cached != bucket.end())
    {
        ++impl_->stats.cacheHits;
        return cached->second;
    }

    ++impl_->stats.cacheMisses;
    ++impl_->stats.evaluations;

    struct MatchedRule
    {
        std::uint64_t specificity = 0;
        std::size_t order = 0;
        const Rule* rule = nullptr;
    };
    std::vector<MatchedRule> matched;
    for (const Rule& rule : impl_->rules)
    {
        std::uint64_t best = 0;
        bool any = false;
        for (const Selector& selector : rule.selectors)
        {
            if (impl_->MatchesSelector(selector, element, stateBits))
            {
                any = true;
                best = std::max(best, Specificity(selector));
            }
        }
        if (any)
        {
            ++impl_->stats.ruleMatches;
            matched.push_back(MatchedRule{best, rule.order, &rule});
        }
    }
    std::sort(matched.begin(), matched.end(), [](const MatchedRule& lhs, const MatchedRule& rhs) {
        if (lhs.specificity != rhs.specificity)
        {
            return lhs.specificity < rhs.specificity;
        }
        return lhs.order < rhs.order;
    });

    UiStyle style{};
    for (const MatchedRule& match : matched)
    {
        ApplyDeclarations(match.rule->declarations, impl_->theme, style);
    }
    ApplyStyleMask(style, impl_->document->Style(element), impl_->document->InlineStyleMask(element));

    const auto inserted = bucket.emplace(key, style);
    return inserted.first->second;
}

void UiStyleEngine::SetTheme(const UiTheme& theme)
{
    if (!impl_ || impl_->theme == theme)
    {
        return;
    }
    impl_->theme = theme;
    impl_->ClearCache();
}

const UiTheme& UiStyleEngine::Theme() const noexcept
{
    static const UiTheme fallback = UiTheme::Dark();
    return impl_ ? impl_->theme : fallback;
}

void UiStyleEngine::InvalidateAll()
{
    if (!impl_)
    {
        return;
    }
    impl_->ClearCache();
    impl_->lastStyleRevision = ~std::uint64_t{0};
}

UiStyleStats UiStyleEngine::Stats() const noexcept
{
    return impl_ ? impl_->stats : UiStyleStats{};
}

const std::vector<UiStyleDiagnostic>& UiStyleEngine::Diagnostics() const noexcept
{
    static const std::vector<UiStyleDiagnostic> empty;
    return impl_ ? impl_->diagnostics : empty;
}

} // namespace se::ui
