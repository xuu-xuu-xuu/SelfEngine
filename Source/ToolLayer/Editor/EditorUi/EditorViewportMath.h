#pragma once

namespace se::ui::editor
{

struct ViewportRectF
{
    float x = 0.0f;
    float y = 0.0f;
    float width = 0.0f;
    float height = 0.0f;
};

[[nodiscard]] inline bool ViewportContains(const ViewportRectF& rect, float x, float y) noexcept
{
    return rect.width > 0.0f && rect.height > 0.0f && x >= rect.x && y >= rect.y &&
        x < rect.x + rect.width && y < rect.y + rect.height;
}

// PickEditorBounds 使用 [0,1]；调用方必须先归一化。
inline void ViewportNormalize(const ViewportRectF& rect, float x, float y, float& u, float& v) noexcept
{
    u = rect.width > 0.0f ? (x - rect.x) / rect.width : 0.0f;
    v = rect.height > 0.0f ? (y - rect.y) / rect.height : 0.0f;
}

// Win32 客户端坐标是物理像素；Play 输入需要视口逻辑坐标。
inline void ViewportOffsetPlayInput(const ViewportRectF& rect, float dpi, float& x, float& y) noexcept
{
    const float scale = dpi > 0.0f ? dpi : 1.0f;
    x -= rect.x * scale;
    y -= rect.y * scale;
}

} // namespace se::ui::editor
