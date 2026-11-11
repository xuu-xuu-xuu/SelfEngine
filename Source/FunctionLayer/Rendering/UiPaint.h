#pragma once

#include "CoreLayer/GuiGeometry.h"
#include "FunctionLayer/UI/Core/UiDocument.h"
#include "FunctionLayer/UI/Core/UiLayout.h"
#include "FunctionLayer/UI/Core/UiStyle.h"
#include "FunctionLayer/UI/Interfaces/UiImageProvider.h"
#include "FunctionLayer/UI/Interfaces/UiPaintSink.h"
#include "FunctionLayer/UI/Interfaces/UiTextService.h"

#include <cstdint>
#include <string>
#include <vector>

namespace se::ui
{

enum class UiPaintCommandKind : std::uint8_t
{
    ClipPush = 0,
    ClipPop,
    Rect,
    Border,
    NineSlice,
    Text,
    Image,
    Line,
    CanvasRect,
    CanvasCircle,
};

// 只存值/句柄；不存 UiDocument*、元素指针或 GPU 资源。
struct UiPaintCommand
{
    UiPaintCommandKind kind = UiPaintCommandKind::Rect;
    UiRect rect{};
    UiRect uv{};
    UiColor color{};
    float borderWidth = 1.0f;
    float insetLeft = 0.0f;
    float insetTop = 0.0f;
    float insetRight = 0.0f;
    float insetBottom = 0.0f;
    float x0 = 0.0f;
    float y0 = 0.0f;
    float x1 = 0.0f;
    float y1 = 0.0f;
    float lineWidth = 0.0f;
    float centerX = 0.0f;
    float centerY = 0.0f;
    float radius = 0.0f;
    float fontSize = 14.0f;
    std::uint64_t texture = 0;
    std::string text;
    std::string image;
};

struct UiPaintStats
{
    std::uint32_t nodes = 0;
    std::uint32_t commands = 0;
    std::uint32_t clipPushes = 0;
    std::uint32_t draws = 0;
    std::uint32_t textRuns = 0;
    std::uint32_t images = 0;
};

class UiDisplayList final : public UiPaintSink
{
public:
    std::vector<UiPaintCommand> commands;
    float width = 0.0f;
    float height = 0.0f;

    void Clear();
    void PushClip(const UiRect& clip) override;
    void PopClip() override;
    void Rect(const UiRect& rect, const UiColor& color) override;
    void Border(const UiRect& rect, float width, const UiColor& color) override;
    void NineSlice(const UiRect& rect,
                   const UiRect& uv,
                   float insetLeft,
                   float insetTop,
                   float insetRight,
                   float insetBottom,
                   std::uint64_t texture,
                   const UiColor& color) override;
    void Text(const UiRect& rect, std::string_view text, float fontSize, const UiColor& color) override;
    void Image(const UiRect& rect, std::string_view image, const UiColor& color) override;
    void Line(float x0, float y0, float x1, float y1, float width, const UiColor& color) override;
    void CanvasRect(const UiRect& rect, const UiColor& color) override;
    void CanvasCircle(float centerX, float centerY, float radius, const UiColor& color) override;
};

class UiPainter
{
public:
    UiPainter(UiDocument& document, UiLayoutEngine& layout, UiStyleEngine* styleEngine = nullptr);

    bool Build(UiElementHandle root, UiDisplayList& displayList, UiPaintStats& stats);

private:
    void Visit(UiElementHandle element, UiDisplayList& displayList, UiPaintStats& stats);

    UiDocument* document_;
    UiLayoutEngine* layout_;
    UiStyleEngine* styleEngine_;
};

struct UiGeometryOptions
{
    std::uint64_t whiteTexture = 1;
    std::uint64_t fontPageBase = 2;
    float whiteU = 0.0f;
    float whiteV = 0.0f;
    float whiteWidth = 1.0f;
    float whiteHeight = 1.0f;
};

struct UiGeometryStats
{
    std::uint32_t quads = 0;
    std::uint32_t drawCalls = 0;
    std::uint32_t commands = 0;
    std::uint32_t textRuns = 0;
    std::uint32_t images = 0;
    std::uint32_t rejected = 0;
};

// DisplayList -> GuiGeometry：20 字节顶点、uint32 索引、scissor。
// 纹理 token 由调用方/渲染器解析；无效 token 拒绝并写 error。
class UiGeometryBuilder
{
public:
    bool Build(const UiDisplayList& displayList,
               UiTextService* textService,
               UiImageProvider* imageProvider,
               const UiGeometryOptions& options,
               GuiGeometry& geometry,
               UiGeometryStats& stats,
               std::vector<std::string>& errors);
};

} // namespace se::ui
