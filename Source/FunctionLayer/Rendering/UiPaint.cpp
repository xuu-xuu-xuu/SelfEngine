#include "FunctionLayer/Rendering/UiPaint.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace se::ui
{
namespace
{
float Srgb(float value) noexcept
{
    value = std::clamp(value, 0.0f, 1.0f);
    return value <= 0.0031308f ? 12.92f * value : 1.055f * std::pow(value, 1.0f / 2.4f) - 0.055f;
}

std::uint32_t PackColor(const UiColor& color) noexcept
{
    const auto byte = [](float value) {
        return static_cast<std::uint32_t>(std::lround(std::clamp(value, 0.0f, 1.0f) * 255.0f));
    };
    return byte(Srgb(color.r)) | (byte(Srgb(color.g)) << 8) | (byte(Srgb(color.b)) << 16) |
        (byte(color.a) << 24);
}

bool Finite(const UiRect& rect) noexcept
{
    return std::isfinite(rect.x) && std::isfinite(rect.y) && std::isfinite(rect.width) &&
        std::isfinite(rect.height) && std::isfinite(rect.x + rect.width) &&
        std::isfinite(rect.y + rect.height) && rect.width >= 0.0f && rect.height >= 0.0f;
}

UiRect Intersect(const UiRect& lhs, const UiRect& rhs) noexcept
{
    const float x = std::max(lhs.x, rhs.x);
    const float y = std::max(lhs.y, rhs.y);
    return {x, y, std::max(0.0f, std::min(lhs.x + lhs.width, rhs.x + rhs.width) - x),
            std::max(0.0f, std::min(lhs.y + lhs.height, rhs.y + rhs.height) - y)};
}

bool Empty(const UiRect& rect) noexcept
{
    return rect.width <= 0.0f || rect.height <= 0.0f;
}

bool SameClip(const Vec4& clip, const UiRect& rect) noexcept
{
    return clip.x == rect.x && clip.y == rect.y && clip.z == rect.x + rect.width &&
        clip.w == rect.y + rect.height;
}
} // namespace

void UiDisplayList::Clear()
{
    commands.clear();
    width = 0.0f;
    height = 0.0f;
}

void UiDisplayList::PushClip(const UiRect& clip)
{
    UiPaintCommand command;
    command.kind = UiPaintCommandKind::ClipPush;
    command.rect = clip;
    commands.push_back(std::move(command));
}

void UiDisplayList::PopClip()
{
    UiPaintCommand command;
    command.kind = UiPaintCommandKind::ClipPop;
    commands.push_back(std::move(command));
}

void UiDisplayList::Rect(const UiRect& rect, const UiColor& color)
{
    UiPaintCommand command;
    command.kind = UiPaintCommandKind::Rect;
    command.rect = rect;
    command.color = color;
    commands.push_back(std::move(command));
}

void UiDisplayList::Border(const UiRect& rect, float borderWidth, const UiColor& color)
{
    UiPaintCommand command;
    command.kind = UiPaintCommandKind::Border;
    command.rect = rect;
    command.borderWidth = borderWidth;
    command.color = color;
    commands.push_back(std::move(command));
}

void UiDisplayList::NineSlice(const UiRect& rect,
                              const UiRect& uv,
                              float insetLeft,
                              float insetTop,
                              float insetRight,
                              float insetBottom,
                              std::uint64_t texture,
                              const UiColor& color)
{
    UiPaintCommand command;
    command.kind = UiPaintCommandKind::NineSlice;
    command.rect = rect;
    command.uv = uv;
    command.insetLeft = insetLeft;
    command.insetTop = insetTop;
    command.insetRight = insetRight;
    command.insetBottom = insetBottom;
    command.texture = texture;
    command.color = color;
    commands.push_back(std::move(command));
}

void UiDisplayList::Text(const UiRect& rect, std::string_view text, float fontSize, const UiColor& color)
{
    UiPaintCommand command;
    command.kind = UiPaintCommandKind::Text;
    command.rect = rect;
    command.text.assign(text);
    command.fontSize = fontSize;
    command.color = color;
    commands.push_back(std::move(command));
}

void UiDisplayList::Image(const UiRect& rect, std::string_view image, const UiColor& color)
{
    UiPaintCommand command;
    command.kind = UiPaintCommandKind::Image;
    command.rect = rect;
    command.image.assign(image);
    command.color = color;
    commands.push_back(std::move(command));
}

void UiDisplayList::Line(float x0, float y0, float x1, float y1, float lineWidth, const UiColor& color)
{
    UiPaintCommand command;
    command.kind = UiPaintCommandKind::Line;
    command.x0 = x0;
    command.y0 = y0;
    command.x1 = x1;
    command.y1 = y1;
    command.lineWidth = lineWidth;
    command.color = color;
    commands.push_back(std::move(command));
}

void UiDisplayList::CanvasRect(const UiRect& rect, const UiColor& color)
{
    UiPaintCommand command;
    command.kind = UiPaintCommandKind::CanvasRect;
    command.rect = rect;
    command.color = color;
    commands.push_back(std::move(command));
}

void UiDisplayList::CanvasCircle(float centerX, float centerY, float radius, const UiColor& color)
{
    UiPaintCommand command;
    command.kind = UiPaintCommandKind::CanvasCircle;
    command.centerX = centerX;
    command.centerY = centerY;
    command.radius = radius;
    command.color = color;
    commands.push_back(std::move(command));
}

UiPainter::UiPainter(UiDocument& document, UiLayoutEngine& layout, UiStyleEngine* styleEngine)
    : document_(&document), layout_(&layout), styleEngine_(styleEngine)
{
}

bool UiPainter::Build(UiElementHandle root, UiDisplayList& displayList, UiPaintStats& stats)
{
    stats = {};
    displayList.Clear();
    if (!document_ || !layout_ || !document_->Element(root))
    {
        return false;
    }
    const UiRect viewport = layout_->Rect(root);
    if (!Finite(viewport) || Empty(viewport))
    {
        return false;
    }
    displayList.width = viewport.width;
    displayList.height = viewport.height;
    displayList.PushClip({0.0f, 0.0f, viewport.width, viewport.height});
    Visit(root, displayList, stats);
    displayList.PopClip();
    stats.commands = static_cast<std::uint32_t>(displayList.commands.size());
    for (const UiPaintCommand& command : displayList.commands)
    {
        if (command.kind == UiPaintCommandKind::ClipPush)
        {
            ++stats.clipPushes;
        }
        else if (command.kind != UiPaintCommandKind::ClipPop)
        {
            ++stats.draws;
        }
    }
    return true;
}

void UiPainter::Visit(UiElementHandle element, UiDisplayList& displayList, UiPaintStats& stats)
{
    const UiElement* node = document_->Element(element);
    if (!node)
    {
        return;
    }
    if (!document_->Visible(element))
    {
        return;
    }
    const UiStyle& style = styleEngine_ ? styleEngine_->Compute(element) : document_->Style(element);
    if (style.display == UiDisplay::None)
    {
        return;
    }
    const UiRect rect = layout_->Rect(element);
    if (Empty(rect) || !Finite(rect))
    {
        return;
    }

    ++stats.nodes;
    const bool clipsChildren = style.overflow != UiOverflow::Visible;
    if (clipsChildren)
    {
        displayList.PushClip(rect);
    }
    if (style.background.a > 0.0f)
    {
        displayList.Rect(rect, style.background);
    }
    if (style.borderWidth > 0.0f && style.borderColor.a > 0.0f)
    {
        displayList.Border(rect, style.borderWidth, style.borderColor);
    }
    for (UiElementHandle child = document_->FirstChild(element); child.IsValid();
         child = document_->NextSibling(child))
    {
        Visit(child, displayList, stats);
    }
    const std::string_view text = document_->Text(element);
    if (!text.empty())
    {
        displayList.Text(rect, text, style.fontSize, style.color);
        ++stats.textRuns;
    }
    if (!style.image.empty())
    {
        displayList.Image(rect, style.image, style.color);
        ++stats.images;
    }
    if (clipsChildren)
    {
        displayList.PopClip();
    }
}

namespace
{
struct GeometryContext
{
    GuiGeometry& geometry;
    UiGeometryStats& stats;
    std::vector<std::string>& errors;
    std::vector<UiRect> clips;
    std::uint64_t whiteTexture = 1;

    bool Reject(const char* message)
    {
        ++stats.rejected;
        errors.emplace_back(message);
        return false;
    }

    bool AppendQuad(const UiRect& rect,
                    const UiRect& uv,
                    const UiColor& color,
                    std::uint64_t texture,
                    const UiRect& clip)
    {
        if (!Finite(rect) || Empty(rect) || Empty(Intersect(rect, clip)) || color.a <= 0.0f)
        {
            return true;
        }
        if (geometry.vertices.size() / 4 >= 65536)
        {
            return Reject("UI.PaintCapacity");
        }
        const std::uint32_t packed = PackColor(color);
        const std::uint32_t base = static_cast<std::uint32_t>(geometry.vertices.size());
        const std::uint32_t first = static_cast<std::uint32_t>(geometry.indices.size());
        geometry.vertices.push_back(GuiVertex{{rect.x, rect.y}, {uv.x, uv.y}, packed});
        geometry.vertices.push_back(GuiVertex{{rect.x + rect.width, rect.y}, {uv.x + uv.width, uv.y}, packed});
        geometry.vertices.push_back(GuiVertex{{rect.x + rect.width, rect.y + rect.height},
                                               {uv.x + uv.width, uv.y + uv.height}, packed});
        geometry.vertices.push_back(GuiVertex{{rect.x, rect.y + rect.height}, {uv.x, uv.y + uv.height}, packed});
        geometry.indices.insert(geometry.indices.end(), {base, base + 1, base + 2, base, base + 2, base + 3});
        const Vec4 scissor{clip.x, clip.y, clip.x + clip.width, clip.y + clip.height};
        if (!geometry.commands.empty() && geometry.commands.back().texture == texture &&
            geometry.commands.back().textureIsData == false && SameClip(geometry.commands.back().clip, clip))
        {
            geometry.commands.back().indexCount += 6;
        }
        else
        {
            geometry.commands.push_back({scissor, texture, first, 6, 0, false});
        }
        ++stats.quads;
        return true;
    }

    bool AppendTriangle(const Vec2& a,
                        const Vec2& b,
                        const Vec2& c,
                        const UiColor& color,
                        std::uint64_t texture,
                        const UiRect& clip)
    {
        const std::uint32_t packed = PackColor(color);
        if (geometry.vertices.size() / 3 >= 65536)
        {
            return Reject("UI.PaintCapacity");
        }
        const std::uint32_t base = static_cast<std::uint32_t>(geometry.vertices.size());
        const std::uint32_t first = static_cast<std::uint32_t>(geometry.indices.size());
        geometry.vertices.push_back(GuiVertex{a, {}, packed});
        geometry.vertices.push_back(GuiVertex{b, {}, packed});
        geometry.vertices.push_back(GuiVertex{c, {}, packed});
        geometry.indices.insert(geometry.indices.end(), {base, base + 1, base + 2});
        const Vec4 scissor{clip.x, clip.y, clip.x + clip.width, clip.y + clip.height};
        if (!geometry.commands.empty() && geometry.commands.back().texture == texture &&
            geometry.commands.back().textureIsData == false && SameClip(geometry.commands.back().clip, clip))
        {
            geometry.commands.back().indexCount += 3;
        }
        else
        {
            geometry.commands.push_back({scissor, texture, first, 3, 0, false});
        }
        ++stats.quads;
        return true;
    }
};
} // namespace

bool UiGeometryBuilder::Build(const UiDisplayList& displayList,
                              UiTextService* textService,
                              UiImageProvider* imageProvider,
                              const UiGeometryOptions& options,
                              GuiGeometry& geometry,
                              UiGeometryStats& stats,
                              std::vector<std::string>& errors)
{
    geometry = {};
    stats = {};
    errors.clear();
    if (!Finite({0.0f, 0.0f, displayList.width, displayList.height}) || displayList.width <= 0.0f ||
        displayList.height <= 0.0f)
    {
        errors.emplace_back("UI.PaintInvalidSize");
        return false;
    }
    geometry.displayPosition = {0.0f, 0.0f};
    geometry.displaySize = {displayList.width, displayList.height};
    geometry.framebufferScale = {1.0f, 1.0f};
    GeometryContext context{geometry, stats, errors, {}, options.whiteTexture};
    context.clips.push_back({0.0f, 0.0f, displayList.width, displayList.height});

    for (const UiPaintCommand& command : displayList.commands)
    {
        ++stats.commands;
        const UiRect clip = context.clips.empty()
            ? UiRect{0.0f, 0.0f, 0.0f, 0.0f}
            : context.clips.back();
        switch (command.kind)
        {
        case UiPaintCommandKind::ClipPush:
            context.clips.push_back(Intersect(clip, command.rect));
            break;
        case UiPaintCommandKind::ClipPop:
            if (context.clips.size() > 1)
            {
                context.clips.pop_back();
            }
            break;
        case UiPaintCommandKind::Rect:
            context.AppendQuad(command.rect, {options.whiteU, options.whiteV, options.whiteWidth, options.whiteHeight}, command.color,
                               options.whiteTexture, clip);
            break;
        case UiPaintCommandKind::Border:
        {
            const float width = std::min(command.borderWidth,
                                         std::min(command.rect.width, command.rect.height) * 0.5f);
            if (width <= 0.0f)
            {
                break;
            }
            context.AppendQuad({command.rect.x, command.rect.y, command.rect.width, width},
                               {options.whiteU, options.whiteV, options.whiteWidth, options.whiteHeight}, command.color, options.whiteTexture, clip);
            context.AppendQuad({command.rect.x, command.rect.y + command.rect.height - width,
                                command.rect.width, width},
                               {options.whiteU, options.whiteV, options.whiteWidth, options.whiteHeight}, command.color, options.whiteTexture, clip);
            context.AppendQuad({command.rect.x, command.rect.y, width, command.rect.height},
                               {options.whiteU, options.whiteV, options.whiteWidth, options.whiteHeight}, command.color, options.whiteTexture, clip);
            context.AppendQuad({command.rect.x + command.rect.width - width, command.rect.y,
                                width, command.rect.height},
                               {options.whiteU, options.whiteV, options.whiteWidth, options.whiteHeight}, command.color, options.whiteTexture, clip);
            break;
        }
        case UiPaintCommandKind::NineSlice:
        {
            if (command.texture == 0)
            {
                context.Reject("UI.PaintInvalidTextureToken");
                break;
            }
            float left = std::max(0.0f, command.insetLeft);
            float right = std::max(0.0f, command.insetRight);
            float top = std::max(0.0f, command.insetTop);
            float bottom = std::max(0.0f, command.insetBottom);
            const float width = command.rect.width;
            const float height = command.rect.height;
            if (left + right > width)
            {
                const float scale = width / (left + right);
                left *= scale;
                right *= scale;
            }
            if (top + bottom > height)
            {
                const float scale = height / (top + bottom);
                top *= scale;
                bottom *= scale;
            }
            const float x0 = command.rect.x;
            const float x1 = x0 + left;
            const float x2 = x0 + width - right;
            const float y0 = command.rect.y;
            const float y1 = y0 + top;
            const float y2 = y0 + height - bottom;
            const float u0 = command.uv.x;
            const float u1 = u0 + command.uv.width * (left / std::max(width, 1.0f));
            const float u2 = u0 + command.uv.width;
            const float v0 = command.uv.y;
            const float v1 = v0 + command.uv.height * (top / std::max(height, 1.0f));
            const float v2 = v0 + command.uv.height;
            const float xs[4] = {x0, x1, x2, x0 + width};
            const float ys[4] = {y0, y1, y2, y0 + height};
            const float us[3] = {u0, u1, u2};
            const float vs[3] = {v0, v1, v2};
            for (int row = 0; row < 3; ++row)
            {
                for (int column = 0; column < 3; ++column)
                {
                    const UiRect cell{xs[column], ys[row], xs[column + 1] - xs[column],
                                      ys[row + 1] - ys[row]};
                    const UiRect uv{us[column], vs[row], us[column + 1] - us[column],
                                    vs[row + 1] - vs[row]};
                    context.AppendQuad(cell, uv, command.color, command.texture, clip);
                }
            }
            break;
        }
        case UiPaintCommandKind::Text:
        {
            if (!textService)
            {
                context.Reject("UI.PaintTextUnavailable");
                break;
            }
            std::vector<UiGlyphQuad> glyphs;
            if (!textService->LayoutGlyphs(command.text, command.fontSize, command.rect.width, true, glyphs))
            {
                context.Reject("UI.PaintTextUnavailable");
                break;
            }
            ++stats.textRuns;
            for (const UiGlyphQuad& glyph : glyphs)
            {
                // v1 只上传第一张图集页；多页 token 会与 scene/white token 冲突，必须先拒绝。
                if (glyph.page != 0)
                {
                    context.Reject("UI.PaintFontPageUnsupported");
                    continue;
                }
                const std::uint64_t texture = options.fontPageBase + glyph.page;
                if (texture == 0)
                {
                    context.Reject("UI.PaintInvalidTextureToken");
                    continue;
                }
                context.AppendQuad({command.rect.x + glyph.x, command.rect.y + glyph.y, glyph.width, glyph.height},
                                   {glyph.u0, glyph.v0, glyph.u1 - glyph.u0, glyph.v1 - glyph.v0},
                                   command.color, texture, clip);
            }
            break;
        }
        case UiPaintCommandKind::Image:
        {
            UiImageInfo info;
            if (!imageProvider || !imageProvider->Resolve(command.image, info) || info.textureToken == 0)
            {
                context.Reject("UI.PaintInvalidTextureToken");
                break;
            }
            ++stats.images;
            context.AppendQuad(command.rect, {0.0f, 0.0f, 1.0f, 1.0f}, command.color, info.textureToken, clip);
            break;
        }
        case UiPaintCommandKind::Line:
        {
            const float dx = command.x1 - command.x0;
            const float dy = command.y1 - command.y0;
            const float length = std::hypot(dx, dy);
            if (length <= 0.0f || command.lineWidth <= 0.0f)
            {
                break;
            }
            const float ox = -dy / length * command.lineWidth * 0.5f;
            const float oy = dx / length * command.lineWidth * 0.5f;
            const UiRect bounds{std::min(command.x0, command.x1) - std::abs(ox),
                                std::min(command.y0, command.y1) - std::abs(oy),
                                std::abs(dx) + std::abs(ox) * 2.0f,
                                std::abs(dy) + std::abs(oy) * 2.0f};
            context.AppendTriangle({command.x0 + ox, command.y0 + oy},
                                   {command.x1 + ox, command.y1 + oy},
                                   {command.x1 - ox, command.y1 - oy},
                                   command.color, options.whiteTexture, clip);
            context.AppendTriangle({command.x0 + ox, command.y0 + oy},
                                   {command.x1 - ox, command.y1 - oy},
                                   {command.x0 - ox, command.y0 - oy},
                                   command.color, options.whiteTexture, clip);
            (void)bounds;
            break;
        }
        case UiPaintCommandKind::CanvasRect:
            context.AppendQuad(command.rect, {options.whiteU, options.whiteV, options.whiteWidth, options.whiteHeight}, command.color,
                               options.whiteTexture, clip);
            break;
        case UiPaintCommandKind::CanvasCircle:
        {
            constexpr int segments = 32;
            for (int index = 0; index < segments; ++index)
            {
                const float a0 = static_cast<float>(index) * 6.28318530718f / segments;
                const float a1 = static_cast<float>(index + 1) * 6.28318530718f / segments;
                context.AppendTriangle({command.centerX, command.centerY},
                                       {command.centerX + std::cos(a0) * command.radius,
                                        command.centerY + std::sin(a0) * command.radius},
                                       {command.centerX + std::cos(a1) * command.radius,
                                        command.centerY + std::sin(a1) * command.radius},
                                       command.color, options.whiteTexture, clip);
            }
            break;
        }
        }
    }
    stats.drawCalls = static_cast<std::uint32_t>(geometry.commands.size());
    return errors.empty();
}

} // namespace se::ui
