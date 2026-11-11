#include "PlatformLayer/Windows/UiDirectWriteTextService.h"

#include "ResourceLayer/Fonts/FontCache.h"

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <list>
#include <string>
#include <string_view>
#include <unordered_map>
#include <utility>

namespace se
{
namespace
{
constexpr std::size_t kCacheCapacity = 512;
constexpr float kUnboundedWidth = 100000.0f;

std::uint64_t HashBytes(const void* data, std::size_t size) noexcept
{
    const auto* bytes = static_cast<const unsigned char*>(data);
    std::uint64_t hash = 1469598103934665603ull;
    for (std::size_t index = 0; index < size; ++index)
    {
        hash ^= bytes[index];
        hash *= 1099511628211ull;
    }
    return hash;
}

std::uint64_t HashText(std::string_view text) noexcept
{
    return HashBytes(text.data(), text.size());
}

std::uint64_t HashFamily(const std::wstring& family) noexcept
{
    return HashBytes(family.data(), family.size() * sizeof(wchar_t));
}

float EffectiveWidth(float maxWidth, bool wrap) noexcept
{
    if (!wrap || !std::isfinite(maxWidth) || maxWidth <= 0.0f)
    {
        return kUnboundedWidth;
    }
    return std::min(maxWidth, kUnboundedWidth);
}

std::string_view Utf8Codepoint(std::string_view text, std::size_t byteOffset, std::size_t& length) noexcept
{
    if (byteOffset >= text.size())
    {
        length = 0;
        return {};
    }
    const unsigned char lead = static_cast<unsigned char>(text[byteOffset]);
    if ((lead & 0x80u) == 0)
    {
        length = 1;
    }
    else if ((lead & 0xE0u) == 0xC0u)
    {
        length = 2;
    }
    else if ((lead & 0xF0u) == 0xE0u)
    {
        length = 3;
    }
    else if ((lead & 0xF8u) == 0xF0u)
    {
        length = 4;
    }
    else
    {
        length = 1;
    }
    length = std::min(length, text.size() - byteOffset);
    return text.substr(byteOffset, length);
}

std::uint32_t ByteOffsetForCodepoint(std::string_view text, std::size_t codepointIndex) noexcept
{
    std::size_t byteOffset = 0;
    std::size_t current = 0;
    while (byteOffset < text.size() && current < codepointIndex)
    {
        std::size_t length = 0;
        Utf8Codepoint(text, byteOffset, length);
        byteOffset += length == 0 ? 1 : length;
        ++current;
    }
    return static_cast<std::uint32_t>(byteOffset);
}

std::size_t CodepointForByteOffset(std::string_view text, std::uint32_t byteOffset) noexcept
{
    std::size_t offset = 0;
    std::size_t codepoint = 0;
    const std::size_t limit = std::min<std::size_t>(byteOffset, text.size());
    while (offset < limit)
    {
        std::size_t length = 0;
        Utf8Codepoint(text, offset, length);
        offset += length == 0 ? 1 : length;
        ++codepoint;
    }
    return codepoint;
}

struct CacheKey
{
    std::uint64_t textHash = 0;
    std::size_t textLength = 0;
    float fontSize = 0.0f;
    float maxWidth = 0.0f;
    bool wrap = false;
    std::uint64_t familyHash = 0;

    bool operator==(const CacheKey&) const = default;
};

struct CacheKeyHash
{
    std::size_t operator()(const CacheKey& key) const noexcept
    {
        std::size_t hash = std::hash<std::uint64_t>{}(key.textHash);
        const auto combine = [&hash](std::size_t value) {
            hash ^= value + 0x9e3779b97f4a7c15ull + (hash << 6) + (hash >> 2);
        };
        combine(key.textLength);
        combine(std::hash<float>{}(key.fontSize));
        combine(std::hash<float>{}(key.maxWidth));
        combine(key.wrap ? 1u : 0u);
        combine(std::hash<std::uint64_t>{}(key.familyHash));
        return hash;
    }
};

struct CachedLayout
{
    std::string text;
    std::wstring family;
    TextLayout layout;
    std::list<CacheKey>::iterator position;
};
} // namespace

struct UiDirectWriteTextService::Impl
{
    mutable FontCache fontCache;
    std::wstring family;
    mutable std::list<CacheKey> order;
    mutable std::unordered_map<CacheKey, CachedLayout, CacheKeyHash> cache;
    mutable CacheStats stats;
    mutable std::string lastError;

    explicit Impl(std::wstring value)
        : family(std::move(value))
    {
    }

    [[nodiscard]] const TextLayout* GetLayout(std::string_view text,
                                              float fontSize,
                                              float maxWidth,
                                              bool wrap) const
    {
        if (text.empty())
        {
            return nullptr;
        }
        const float width = EffectiveWidth(maxWidth, wrap);
        const CacheKey key{HashText(text), text.size(), fontSize, width, wrap, HashFamily(family)};
        const auto found = cache.find(key);
        if (found != cache.end())
        {
            if (found->second.text == text && found->second.family == family)
            {
                order.splice(order.begin(), order, found->second.position);
                ++stats.hits;
                return &found->second.layout;
            }
            order.erase(found->second.position);
            cache.erase(found);
        }

        ++stats.misses;
        TextLayout layout;
        std::string error;
        if (!fontCache.Layout(text, fontSize, width, layout, error, family))
        {
            lastError = std::move(error);
            return nullptr;
        }
        lastError.clear();
        if (cache.size() >= kCacheCapacity)
        {
            const CacheKey evicted = order.back();
            order.pop_back();
            cache.erase(evicted);
        }
        order.push_front(key);
        CachedLayout stored{std::string(text), family, std::move(layout), order.begin()};
        const auto inserted = cache.emplace(key, std::move(stored));
        return &inserted.first->second.layout;
    }
};

UiDirectWriteTextService::UiDirectWriteTextService(std::wstring family)
    : impl_(std::make_unique<Impl>(std::move(family)))
{
}

UiDirectWriteTextService::~UiDirectWriteTextService() = default;

ui::UiTextMeasure UiDirectWriteTextService::Measure(std::string_view text,
                                                    float fontSize,
                                                    float maxWidth,
                                                    bool wrap) const
{
    if (text.empty())
    {
        return {};
    }
    const TextLayout* layout = impl_->GetLayout(text, fontSize, maxWidth, wrap);
    if (!layout)
    {
        return {};
    }
    const std::uint32_t lines = layout->lineCount == 0 ? 1u : layout->lineCount;
    return {layout->width, layout->height, lines};
}

float UiDirectWriteTextService::LineHeight(float fontSize) const
{
    if (!std::isfinite(fontSize) || fontSize <= 0.0f)
    {
        return 0.0f;
    }
    const ui::UiTextMeasure measure = Measure("M", fontSize, kUnboundedWidth, false);
    return measure.height > 0.0f ? measure.height : fontSize * 1.2f;
}

std::uint32_t UiDirectWriteTextService::HitTestCaret(std::string_view text,
                                                     float fontSize,
                                                     float maxWidth,
                                                     bool wrap,
                                                     float x,
                                                     float y) const
{
    const TextLayout* layout = impl_->GetLayout(text, fontSize, maxWidth, wrap);
    if (!layout)
    {
        return 0;
    }
    return ByteOffsetForCodepoint(text, layout->HitTest(x, y));
}

ui::UiCaretRect UiDirectWriteTextService::CaretRect(std::string_view text,
                                                    float fontSize,
                                                    float maxWidth,
                                                    bool wrap,
                                                    std::uint32_t byteOffset) const
{
    const TextLayout* layout = impl_->GetLayout(text, fontSize, maxWidth, wrap);
    if (!layout)
    {
        return {};
    }
    const UiRect rect = layout->Caret(CodepointForByteOffset(text, byteOffset));
    return {rect.x, rect.y, rect.width, rect.height};
}

bool UiDirectWriteTextService::LayoutGlyphs(std::string_view text,
                                            float fontSize,
                                            float maxWidth,
                                            bool wrap,
                                            std::vector<ui::UiGlyphQuad>& glyphs) const
{
    glyphs.clear();
    const TextLayout* layout = impl_->GetLayout(text, fontSize, maxWidth, wrap);
    if (!layout)
    {
        return false;
    }
    const std::vector<FontPage>& pages = impl_->fontCache.Pages();
    for (const TextQuad& quad : layout->quads)
    {
        const float pageWidth = quad.page < pages.size() ? static_cast<float>(pages[quad.page].width) : 1024.0f;
        const float pageHeight = quad.page < pages.size() ? static_cast<float>(pages[quad.page].height) : 1024.0f;
        if (pageWidth <= 0.0f || pageHeight <= 0.0f || quad.destination.width <= 0.0f ||
            quad.destination.height <= 0.0f)
        {
            continue;
        }
        glyphs.push_back({quad.destination.x,
                          quad.destination.y,
                          quad.destination.width,
                          quad.destination.height,
                          quad.pixels.x / pageWidth,
                          quad.pixels.y / pageHeight,
                          (quad.pixels.x + quad.pixels.width) / pageWidth,
                          (quad.pixels.y + quad.pixels.height) / pageHeight,
                          quad.page});
    }
    return true;
}

bool UiDirectWriteTextService::FontAtlas(std::vector<unsigned char>& rgba, int& width, int& height, std::uint64_t& revision) const
{
    const std::vector<FontPage>& pages = impl_->fontCache.Pages();
    if (pages.empty() || pages.front().alpha.empty())
    {
        return false;
    }
    const FontPage& page = pages.front();
    width = static_cast<int>(page.width);
    height = static_cast<int>(page.height);
    revision = page.revision;
    rgba.resize(static_cast<std::size_t>(width) * static_cast<std::size_t>(height) * 4);
    for (std::size_t index = 0; index < page.alpha.size() && index * 4 + 3 < rgba.size(); ++index)
    {
        rgba[index * 4] = 255;
        rgba[index * 4 + 1] = 255;
        rgba[index * 4 + 2] = 255;
        rgba[index * 4 + 3] = page.alpha[index];
    }
    return true;
}

UiDirectWriteTextService::CacheStats UiDirectWriteTextService::Stats() const noexcept
{
    CacheStats stats = impl_->stats;
    stats.entries = impl_->cache.size();
    return stats;
}

void UiDirectWriteTextService::ClearCache()
{
    impl_->order.clear();
    impl_->cache.clear();
}

const std::string& UiDirectWriteTextService::LastError() const noexcept
{
    return impl_->lastError;
}

} // namespace se
