#pragma once

#include "FunctionLayer/UI/Core/UiDocument.h"
#include "FunctionLayer/UI/Interfaces/UiTextService.h"

#include <cstdint>
#include <memory>

namespace se::ui
{

struct UiRect
{
    float x = 0.0f;
    float y = 0.0f;
    float width = 0.0f;
    float height = 0.0f;
};

struct UiLayoutStats
{
    std::uint32_t layoutPasses = 0;
    std::uint32_t nodesMeasured = 0;
    std::uint32_t nodesArranged = 0;
    std::uint32_t measureCacheHits = 0;
    std::uint32_t measureCacheMisses = 0;
};

// Flex 子集布局：measure + arrange，布局脏子树/宽度不变时复用测量缓存。
// 文本通过 UiTextService 注入；阶段 D 默认使用 UiStubTextService。
class UiLayoutEngine
{
public:
    explicit UiLayoutEngine(UiDocument& document, UiTextService* textService = nullptr);
    void SetStyleEngine(UiStyleEngine* styleEngine);
    ~UiLayoutEngine();

    UiLayoutEngine(UiLayoutEngine&&) noexcept;
    UiLayoutEngine& operator=(UiLayoutEngine&&) noexcept;
    UiLayoutEngine(const UiLayoutEngine&) = delete;
    UiLayoutEngine& operator=(const UiLayoutEngine&) = delete;

    // 返回 true 表示执行了一次布局；没有布局脏且视口不变时返回 false。
    bool Layout(float viewportWidth, float viewportHeight);
    [[nodiscard]] UiRect Rect(UiElementHandle element) const noexcept;
    [[nodiscard]] UiLayoutStats Stats() const noexcept;
    void Reset();

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

} // namespace se::ui
