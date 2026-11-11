#pragma once

#include <cstdint>
#include <string_view>

namespace se::ui
{

struct UiImageInfo
{
    std::uint64_t textureToken = 0;
    float width = 0.0f;
    float height = 0.0f;
};

// 图片 id -> 渲染器纹理 token。核心只使用不透明 token，不持有 RHI 资源。
class UiImageProvider
{
public:
    virtual ~UiImageProvider() = default;

    [[nodiscard]] virtual bool Resolve(std::string_view image, UiImageInfo& info) const = 0;
};

} // namespace se::ui
