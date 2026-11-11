#include "FunctionLayer/UI/Core/UiLayout.h"

#include "FunctionLayer/UI/Core/UiStyle.h"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <limits>
#include <string_view>
#include <unordered_map>
#include <utility>
#include <vector>

namespace se::ui
{
namespace
{
constexpr float kInfinity = std::numeric_limits<float>::infinity();

float Sanitize(float value) noexcept
{
    return std::isfinite(value) ? value : 0.0f;
}

float Clamp(float value, float minimum, float maximum) noexcept
{
    value = Sanitize(value);
    if (maximum < minimum)
    {
        maximum = minimum;
    }
    return std::min(std::max(value, minimum), maximum);
}

float RoundQuarter(float value) noexcept
{
    return std::round(Sanitize(value) * 4.0f) / 4.0f;
}

UiRect RoundRect(UiRect rect) noexcept
{
    rect.x = RoundQuarter(rect.x);
    rect.y = RoundQuarter(rect.y);
    rect.width = std::max(0.0f, RoundQuarter(rect.width));
    rect.height = std::max(0.0f, RoundQuarter(rect.height));
    return rect;
}

bool NearlyEqual(float lhs, float rhs) noexcept
{
    return std::abs(lhs - rhs) <= 0.001f;
}

bool HasLayoutDirty(UiDirtyFlags flags) noexcept
{
    return HasFlag(flags, UiDirtyFlags::Layout) || HasFlag(flags, UiDirtyFlags::Structure);
}

bool ResolveLength(const UiLength& length, float reference, float& value) noexcept
{
    switch (length.unit)
    {
    case UiUnit::Px:
        value = Sanitize(length.value);
        return true;
    case UiUnit::Percent:
        value = Sanitize(reference) * Sanitize(length.value) / 100.0f;
        return true;
    case UiUnit::Auto:
    default:
        return false;
    }
}

float ResolveMinimum(const UiLength& length, float reference) noexcept
{
    float value = 0.0f;
    return ResolveLength(length, reference, value) ? std::max(0.0f, value) : 0.0f;
}

float ResolveMaximum(const UiLength& length, float reference) noexcept
{
    float value = 0.0f;
    return ResolveLength(length, reference, value) ? std::max(0.0f, value) : kInfinity;
}

float ResolveOr(const UiLength& length, float reference, float fallback) noexcept
{
    float value = 0.0f;
    return ResolveLength(length, reference, value) ? value : fallback;
}

struct Size
{
    float width = 0.0f;
    float height = 0.0f;
};
} // namespace

UiStubTextService::UiStubTextService(float charWidth, float lineHeight) noexcept
    : charWidth_(charWidth > 0.0f ? charWidth : 8.0f),
      lineHeight_(lineHeight > 0.0f ? lineHeight : 16.0f)
{
}

UiTextMeasure UiStubTextService::Measure(std::string_view text, float, float maxWidth, bool wrap) const
{
    if (text.empty())
    {
        return {};
    }

    float widest = 0.0f;
    float lineWidth = 0.0f;
    std::uint32_t lines = 1;
    for (const char character : text)
    {
        if (character == '\n')
        {
            widest = std::max(widest, lineWidth);
            lineWidth = 0.0f;
            ++lines;
            continue;
        }
        lineWidth += charWidth_;
    }
    widest = std::max(widest, lineWidth);

    if (wrap && maxWidth > 0.0f && widest > maxWidth)
    {
        lines += static_cast<std::uint32_t>(widest / maxWidth);
        widest = maxWidth;
    }
    return {widest, static_cast<float>(lines) * lineHeight_, lines};
}

float UiStubTextService::LineHeight(float) const
{
    return lineHeight_;
}

std::uint32_t UiStubTextService::HitTestCaret(std::string_view text,
                                              float,
                                              float maxWidth,
                                              bool wrap,
                                              float x,
                                              float) const
{
    if (wrap && maxWidth > 0.0f && x > maxWidth)
    {
        x = maxWidth;
    }
    const float clamped = std::max(0.0f, x);
    const std::uint32_t offset = static_cast<std::uint32_t>(clamped / charWidth_);
    return offset < text.size() ? offset : static_cast<std::uint32_t>(text.size());
}

UiCaretRect UiStubTextService::CaretRect(std::string_view text,
                                         float,
                                         float maxWidth,
                                         bool wrap,
                                         std::uint32_t byteOffset) const
{
    const std::uint32_t offset = byteOffset < text.size() ? byteOffset : static_cast<std::uint32_t>(text.size());
    float x = static_cast<float>(offset) * charWidth_;
    float y = 0.0f;
    if (wrap && maxWidth > 0.0f && x > maxWidth)
    {
        const float row = static_cast<float>(static_cast<std::uint32_t>(x / maxWidth));
        y = row * lineHeight_;
        x -= row * maxWidth;
    }
    return {x, y, 1.0f, lineHeight_};
}

float UiStubTextService::CharacterWidth() const noexcept
{
    return charWidth_;
}

float UiStubTextService::StubLineHeight() const noexcept
{
    return lineHeight_;
}

bool UiStubTextService::LayoutGlyphs(std::string_view text,
                                     float,
                                     float maxWidth,
                                     bool wrap,
                                     std::vector<UiGlyphQuad>& glyphs) const
{
    glyphs.clear();
    float x = 0.0f;
    float y = 0.0f;
    for (const char character : text)
    {
        if (character == '\n')
        {
            x = 0.0f;
            y += lineHeight_;
            continue;
        }
        if (wrap && maxWidth > 0.0f && x + charWidth_ > maxWidth)
        {
            x = 0.0f;
            y += lineHeight_;
        }
        glyphs.push_back({x, y, charWidth_, lineHeight_, 0.0f, 0.0f, 1.0f, 1.0f, 0});
        x += charWidth_;
    }
    return true;
}

struct UiLayoutEngine::Impl
{
    struct Record
    {
        bool measured = false;
        bool arranged = false;
        float measuredWidth = 0.0f;
        float measuredHeight = 0.0f;
        float availableWidth = -1.0f;
        float availableHeight = -1.0f;
        UiRect rect{};
    };

    struct FlowItem
    {
        UiElementHandle handle;
        float baseMain = 0.0f;
        float mainSize = 0.0f;
        float crossSize = 0.0f;
        float marginMainStart = 0.0f;
        float marginMainEnd = 0.0f;
        float marginCrossStart = 0.0f;
        float marginCrossEnd = 0.0f;
        float grow = 0.0f;
        float shrink = 0.0f;
        bool stretchCross = false;
    };

    UiDocument* document = nullptr;
    UiStubTextService stub;
    UiTextService* textService = nullptr;
    UiStyleEngine* styleEngine = nullptr;
    std::unordered_map<std::uint64_t, Record> records;
    std::uint64_t lastRevision = ~std::uint64_t{0};
    float viewportWidth = -1.0f;
    float viewportHeight = -1.0f;
    bool initialized = false;
    bool forceFull = true;
    UiLayoutStats stats{};

    [[nodiscard]] static std::uint64_t Key(UiElementHandle handle) noexcept
    {
        return (static_cast<std::uint64_t>(handle.index) << 32) |
            static_cast<std::uint64_t>(handle.generation);
    }

    Record& RecordFor(UiElementHandle handle)
    {
        return records[Key(handle)];
    }

    [[nodiscard]] const Record* FindRecord(UiElementHandle handle) const
    {
        const auto found = records.find(Key(handle));
        return found == records.end() ? nullptr : &found->second;
    }

    Size MeasureNode(UiElementHandle handle, float availableWidth, float availableHeight);
    void ArrangeNode(UiElementHandle handle, const UiRect& rect);
    const UiStyle& StyleFor(UiElementHandle handle);
    void ClearLayoutDirty(UiElementHandle handle);
};

UiLayoutEngine::UiLayoutEngine(UiDocument& document, UiTextService* textService)
    : impl_(std::make_unique<Impl>())
{
    impl_->document = &document;
    impl_->textService = textService ? textService : &impl_->stub;
}

UiLayoutEngine::~UiLayoutEngine() = default;

void UiLayoutEngine::SetStyleEngine(UiStyleEngine* styleEngine)
{
    if (!impl_)
    {
        return;
    }
    impl_->styleEngine = styleEngine;
    Reset();
}

UiLayoutEngine::UiLayoutEngine(UiLayoutEngine&&) noexcept = default;

UiLayoutEngine& UiLayoutEngine::operator=(UiLayoutEngine&&) noexcept = default;

bool UiLayoutEngine::Layout(float viewportWidth, float viewportHeight)
{
    if (!impl_ || !impl_->document || !impl_->document->IsValid())
    {
        return false;
    }

    const float width = std::max(0.0f, Sanitize(viewportWidth));
    const float height = std::max(0.0f, Sanitize(viewportHeight));
    const bool viewportChanged = !impl_->initialized ||
        !NearlyEqual(width, impl_->viewportWidth) ||
        !NearlyEqual(height, impl_->viewportHeight);
    const std::uint64_t revision = impl_->document->LayoutRevision();
    if (impl_->initialized && !viewportChanged && revision == impl_->lastRevision)
    {
        impl_->stats = {};
        return false;
    }

    if (viewportChanged)
    {
        impl_->forceFull = true;
        impl_->viewportWidth = width;
        impl_->viewportHeight = height;
    }
    impl_->initialized = true;
    impl_->stats = {};
    impl_->stats.layoutPasses = 1;

    const UiElementHandle root = impl_->document->Root();
    if (!impl_->document->Element(root))
    {
        impl_->lastRevision = revision;
        impl_->forceFull = false;
        return false;
    }

    impl_->MeasureNode(root, width, height);
    impl_->ArrangeNode(root, UiRect{0.0f, 0.0f, width, height});
    impl_->lastRevision = impl_->document->LayoutRevision();
    impl_->forceFull = false;
    return true;
}

UiRect UiLayoutEngine::Rect(UiElementHandle element) const noexcept
{
    if (!impl_)
    {
        return {};
    }
    const Impl::Record* record = impl_->FindRecord(element);
    return record ? record->rect : UiRect{};
}

UiLayoutStats UiLayoutEngine::Stats() const noexcept
{
    return impl_ ? impl_->stats : UiLayoutStats{};
}

void UiLayoutEngine::Reset()
{
    if (!impl_)
    {
        return;
    }
    impl_->records.clear();
    impl_->lastRevision = ~std::uint64_t{0};
    impl_->viewportWidth = -1.0f;
    impl_->viewportHeight = -1.0f;
    impl_->initialized = false;
    impl_->forceFull = true;
    impl_->stats = {};
}

Size UiLayoutEngine::Impl::MeasureNode(UiElementHandle handle, float availableWidth, float availableHeight)
{
    const UiElement* element = document->Element(handle);
    if (!element)
    {
        return {};
    }

    const UiStyle& style = StyleFor(handle);
    const float safeAvailableWidth = std::max(0.0f, Sanitize(availableWidth));
    const float safeAvailableHeight = std::max(0.0f, Sanitize(availableHeight));
    Record& record = RecordFor(handle);

    if (style.display == UiDisplay::None)
    {
        record.measured = true;
        record.measuredWidth = 0.0f;
        record.measuredHeight = 0.0f;
        record.availableWidth = safeAvailableWidth;
        record.availableHeight = safeAvailableHeight;
        return {};
    }

    const bool dirty = HasLayoutDirty(document->Dirty(handle));
    if (!forceFull && !dirty && record.measured &&
        NearlyEqual(record.availableWidth, safeAvailableWidth) &&
        NearlyEqual(record.availableHeight, safeAvailableHeight))
    {
        ++stats.measureCacheHits;
        return {record.measuredWidth, record.measuredHeight};
    }

    ++stats.measureCacheMisses;
    ++stats.nodesMeasured;

    const float paddingLeft = std::max(0.0f, Sanitize(style.padding.left));
    const float paddingTop = std::max(0.0f, Sanitize(style.padding.top));
    const float paddingRight = std::max(0.0f, Sanitize(style.padding.right));
    const float paddingBottom = std::max(0.0f, Sanitize(style.padding.bottom));
    const float paddingHorizontal = paddingLeft + paddingRight;
    const float paddingVertical = paddingTop + paddingBottom;
    const float contentAvailableWidth = std::max(0.0f, safeAvailableWidth - paddingHorizontal);
    const float contentAvailableHeight = std::max(0.0f, safeAvailableHeight - paddingVertical);

    float explicitWidth = 0.0f;
    float explicitHeight = 0.0f;
    const bool hasWidth = ResolveLength(style.width, contentAvailableWidth, explicitWidth);
    const bool hasHeight = ResolveLength(style.height, contentAvailableHeight, explicitHeight);
    if (hasWidth)
    {
        explicitWidth = std::max(0.0f, Sanitize(explicitWidth));
    }
    if (hasHeight)
    {
        explicitHeight = std::max(0.0f, Sanitize(explicitHeight));
    }
    const float childAvailableWidth = hasWidth
        ? std::max(0.0f, explicitWidth - paddingHorizontal)
        : contentAvailableWidth;
    const float childAvailableHeight = hasHeight
        ? std::max(0.0f, explicitHeight - paddingVertical)
        : contentAvailableHeight;

    const float textMaxWidth = hasWidth ? std::max(0.0f, explicitWidth - paddingHorizontal) : contentAvailableWidth;
    const std::string_view text = document->Text(handle);
    const UiTextMeasure textMeasure = text.empty()
        ? UiTextMeasure{}
        : textService->Measure(text, style.fontSize, textMaxWidth, true);

    const bool row = style.flexDirection == UiFlexDirection::Row;
    const float gap = std::max(0.0f, Sanitize(style.gap));
    int itemCount = text.empty() ? 0 : 1;
    float mainTotal = 0.0f;
    float crossMaximum = 0.0f;

    if (!text.empty())
    {
        mainTotal += row ? textMeasure.width : textMeasure.height;
        crossMaximum = std::max(crossMaximum, row ? textMeasure.height : textMeasure.width);
    }

    for (UiElementHandle child = document->FirstChild(handle); child.IsValid(); child = document->NextSibling(child))
    {
        const UiStyle& childStyle = StyleFor(child);
        if (childStyle.display == UiDisplay::None || childStyle.position == UiPosition::Absolute)
        {
            continue;
        }
        const Size childSize = MeasureNode(child, childAvailableWidth, childAvailableHeight);
        const float marginHorizontal = Sanitize(childStyle.margin.left) + Sanitize(childStyle.margin.right);
        const float marginVertical = Sanitize(childStyle.margin.top) + Sanitize(childStyle.margin.bottom);
        ++itemCount;
        if (row)
        {
            mainTotal += childSize.width + marginHorizontal;
            crossMaximum = std::max(crossMaximum, childSize.height + marginVertical);
        }
        else
        {
            mainTotal += childSize.height + marginVertical;
            crossMaximum = std::max(crossMaximum, childSize.width + marginHorizontal);
        }
    }

    if (itemCount > 1)
    {
        mainTotal += gap * static_cast<float>(itemCount - 1);
    }

    const float contentWidth = row ? mainTotal : crossMaximum;
    const float contentHeight = row ? crossMaximum : mainTotal;
    float desiredWidth = hasWidth ? explicitWidth : contentWidth + paddingHorizontal;
    float desiredHeight = hasHeight ? explicitHeight : contentHeight + paddingVertical;

    const float minimumWidth = ResolveMinimum(style.minWidth, contentAvailableWidth);
    const float maximumWidth = ResolveMaximum(style.maxWidth, contentAvailableWidth);
    const float minimumHeight = ResolveMinimum(style.minHeight, contentAvailableHeight);
    const float maximumHeight = ResolveMaximum(style.maxHeight, contentAvailableHeight);
    desiredWidth = Clamp(desiredWidth, minimumWidth, maximumWidth);
    desiredHeight = Clamp(desiredHeight, minimumHeight, maximumHeight);

    record.measured = true;
    record.measuredWidth = desiredWidth;
    record.measuredHeight = desiredHeight;
    record.availableWidth = safeAvailableWidth;
    record.availableHeight = safeAvailableHeight;
    return {desiredWidth, desiredHeight};
}

void UiLayoutEngine::Impl::ClearLayoutDirty(UiElementHandle handle)
{
    document->ClearDirty(handle, UiDirtyFlags::Layout | UiDirtyFlags::Structure);
}

const UiStyle& UiLayoutEngine::Impl::StyleFor(UiElementHandle handle)
{
    return styleEngine ? styleEngine->Compute(handle) : document->Style(handle);
}

void UiLayoutEngine::Impl::ArrangeNode(UiElementHandle handle, const UiRect& rawRect)
{
    const UiElement* element = document->Element(handle);
    if (!element)
    {
        return;
    }

    const UiStyle& style = StyleFor(handle);
    const UiRect rect = RoundRect(rawRect);
    Record& current = RecordFor(handle);
    if (!forceFull && current.arranged && !HasLayoutDirty(document->Dirty(handle)) &&
        NearlyEqual(current.rect.x, rect.x) && NearlyEqual(current.rect.y, rect.y) &&
        NearlyEqual(current.rect.width, rect.width) && NearlyEqual(current.rect.height, rect.height))
    {
        return;
    }
    current.rect = rect;
    current.arranged = true;
    ++stats.nodesArranged;

    if (style.display == UiDisplay::None)
    {
        RecordFor(handle).rect = UiRect{};
        ClearLayoutDirty(handle);
        return;
    }

    const float paddingLeft = std::max(0.0f, Sanitize(style.padding.left));
    const float paddingTop = std::max(0.0f, Sanitize(style.padding.top));
    const float paddingRight = std::max(0.0f, Sanitize(style.padding.right));
    const float paddingBottom = std::max(0.0f, Sanitize(style.padding.bottom));
    const UiRect content{
        rect.x + paddingLeft,
        rect.y + paddingTop,
        std::max(0.0f, rect.width - paddingLeft - paddingRight),
        std::max(0.0f, rect.height - paddingTop - paddingBottom)};
    const bool row = style.flexDirection == UiFlexDirection::Row;
    const float contentMain = row ? content.width : content.height;
    const float contentCross = row ? content.height : content.width;
    const float gap = std::max(0.0f, Sanitize(style.gap));

    std::vector<FlowItem> flow;
    std::vector<UiElementHandle> absolute;
    for (UiElementHandle child = document->FirstChild(handle); child.IsValid(); child = document->NextSibling(child))
    {
        const UiStyle& childStyle = StyleFor(child);
        if (childStyle.display == UiDisplay::None)
        {
            RecordFor(child).rect = UiRect{};
            ClearLayoutDirty(child);
            continue;
        }
        if (childStyle.position == UiPosition::Absolute)
        {
            absolute.push_back(child);
        }
        else
        {
            flow.push_back(FlowItem{child});
        }
    }

    for (FlowItem& item : flow)
    {
        const UiStyle& childStyle = StyleFor(item.handle);
        const Record* record = FindRecord(item.handle);
        const Size childSize = record && record->measured
            ? Size{record->measuredWidth, record->measuredHeight}
            : MeasureNode(item.handle, content.width, content.height);
        const float marginLeft = Sanitize(childStyle.margin.left);
        const float marginTop = Sanitize(childStyle.margin.top);
        const float marginRight = Sanitize(childStyle.margin.right);
        const float marginBottom = Sanitize(childStyle.margin.bottom);
        item.marginMainStart = row ? marginLeft : marginTop;
        item.marginMainEnd = row ? marginRight : marginBottom;
        item.marginCrossStart = row ? marginTop : marginLeft;
        item.marginCrossEnd = row ? marginBottom : marginRight;
        item.baseMain = row ? childSize.width : childSize.height;
        if (!childStyle.flexBasis.IsAuto())
        {
            item.baseMain = std::max(0.0f, ResolveOr(childStyle.flexBasis, contentMain, item.baseMain));
        }
        item.baseMain = std::max(0.0f, item.baseMain);
        item.mainSize = item.baseMain;
        item.crossSize = std::max(0.0f, row ? childSize.height : childSize.width);
        item.grow = std::max(0.0f, Sanitize(childStyle.flexGrow));
        item.shrink = std::max(0.0f, Sanitize(childStyle.flexShrink));
        item.stretchCross = style.alignItems == UiAlignItems::Stretch &&
            (row ? childStyle.height.IsAuto() : childStyle.width.IsAuto());
    }

    float used = flow.empty() ? 0.0f : gap * static_cast<float>(flow.size() - 1);
    float growSum = 0.0f;
    float shrinkWeightSum = 0.0f;
    for (const FlowItem& item : flow)
    {
        used += item.mainSize + item.marginMainStart + item.marginMainEnd;
        growSum += item.grow;
        shrinkWeightSum += item.shrink * item.baseMain;
    }

    float free = contentMain - used;
    if (free > 0.0f && growSum > 0.0f)
    {
        for (FlowItem& item : flow)
        {
            item.mainSize += free * item.grow / growSum;
        }
        free = 0.0f;
    }
    else if (free < 0.0f && shrinkWeightSum > 0.0f)
    {
        for (FlowItem& item : flow)
        {
            const float weight = item.shrink * item.baseMain;
            item.mainSize = std::max(0.0f, item.mainSize + free * weight / shrinkWeightSum);
        }
    }

    used = flow.empty() ? 0.0f : gap * static_cast<float>(flow.size() - 1);
    for (FlowItem& item : flow)
    {
        const UiStyle& childStyle = StyleFor(item.handle);
        const float minMain = ResolveMinimum(row ? childStyle.minWidth : childStyle.minHeight, contentMain);
        const float maxMain = ResolveMaximum(row ? childStyle.maxWidth : childStyle.maxHeight, contentMain);
        item.mainSize = Clamp(item.mainSize, minMain, maxMain);
        used += item.mainSize + item.marginMainStart + item.marginMainEnd;
    }
    free = contentMain - used;

    float mainOffset = 0.0f;
    float extraGap = 0.0f;
    if (free > 0.0f)
    {
        switch (style.justifyContent)
        {
        case UiJustifyContent::Center:
            mainOffset = free * 0.5f;
            break;
        case UiJustifyContent::FlexEnd:
            mainOffset = free;
            break;
        case UiJustifyContent::SpaceBetween:
            if (flow.size() > 1)
            {
                extraGap = free / static_cast<float>(flow.size() - 1);
            }
            break;
        case UiJustifyContent::FlexStart:
        default:
            break;
        }
    }

    float mainCursor = (row ? content.x : content.y) + mainOffset;
    for (FlowItem& item : flow)
    {
        const UiStyle& childStyle = StyleFor(item.handle);
        float crossSize = item.crossSize;
        if (item.stretchCross)
        {
            crossSize = std::max(0.0f, contentCross - item.marginCrossStart - item.marginCrossEnd);
        }
        const float minCross = ResolveMinimum(row ? childStyle.minHeight : childStyle.minWidth, contentCross);
        const float maxCross = ResolveMaximum(row ? childStyle.maxHeight : childStyle.maxWidth, contentCross);
        crossSize = Clamp(crossSize, minCross, maxCross);

        float crossOffset = 0.0f;
        switch (style.alignItems)
        {
        case UiAlignItems::Center:
            crossOffset = (contentCross - crossSize - item.marginCrossStart - item.marginCrossEnd) * 0.5f;
            break;
        case UiAlignItems::FlexEnd:
            crossOffset = contentCross - crossSize - item.marginCrossStart - item.marginCrossEnd;
            break;
        case UiAlignItems::FlexStart:
        case UiAlignItems::Stretch:
        default:
            break;
        }
        crossOffset = std::max(0.0f, crossOffset);
        const float crossPosition = (row ? content.y : content.x) + crossOffset + item.marginCrossStart;
        const float mainPosition = mainCursor + item.marginMainStart;
        const UiRect childRect = row
            ? UiRect{mainPosition, crossPosition, item.mainSize, crossSize}
            : UiRect{crossPosition, mainPosition, crossSize, item.mainSize};
        ArrangeNode(item.handle, childRect);
        mainCursor += item.marginMainStart + item.mainSize + item.marginMainEnd + gap + extraGap;
    }

    for (const UiElementHandle child : absolute)
    {
        const UiStyle& childStyle = StyleFor(child);
        const Size childSize = MeasureNode(child, content.width, content.height);
        const float marginLeft = Sanitize(childStyle.margin.left);
        const float marginTop = Sanitize(childStyle.margin.top);
        const float marginRight = Sanitize(childStyle.margin.right);
        const float marginBottom = Sanitize(childStyle.margin.bottom);
        float left = 0.0f;
        float right = 0.0f;
        float top = 0.0f;
        float bottom = 0.0f;
        const bool hasLeft = ResolveLength(childStyle.left, content.width, left);
        const bool hasRight = ResolveLength(childStyle.right, content.width, right);
        const bool hasTop = ResolveLength(childStyle.top, content.height, top);
        const bool hasBottom = ResolveLength(childStyle.bottom, content.height, bottom);
        float x = content.x + marginLeft;
        if (hasLeft)
        {
            x = content.x + left + marginLeft;
        }
        else if (hasRight)
        {
            x = content.x + content.width - childSize.width - right - marginRight;
        }
        float y = content.y + marginTop;
        if (hasTop)
        {
            y = content.y + top + marginTop;
        }
        else if (hasBottom)
        {
            y = content.y + content.height - childSize.height - bottom - marginBottom;
        }
        ArrangeNode(child, UiRect{x, y, childSize.width, childSize.height});
    }

    ClearLayoutDirty(handle);
}

} // namespace se::ui
