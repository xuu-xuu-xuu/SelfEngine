#pragma once

#include "FunctionLayer/UI/Interfaces/UiTextService.h"

#include <cstddef>
#include <cstdint>
#include <memory>
#include <string>
#include <string_view>
#include <vector>

namespace se
{

// DirectWrite 适配器：只存在于平台侧；核心只 include UiTextService。
// 使用现有 FontCache 做整形/回退/折行，不新增第二套字体栈。
class UiDirectWriteTextService final : public ui::UiTextService
{
public:
    struct CacheStats
    {
        std::uint64_t hits = 0;
        std::uint64_t misses = 0;
        std::size_t entries = 0;
    };

    explicit UiDirectWriteTextService(std::wstring family = L"Microsoft YaHei UI");
    ~UiDirectWriteTextService() override;

    UiDirectWriteTextService(const UiDirectWriteTextService&) = delete;
    UiDirectWriteTextService& operator=(const UiDirectWriteTextService&) = delete;

    [[nodiscard]] ui::UiTextMeasure Measure(std::string_view text,
                                            float fontSize,
                                            float maxWidth,
                                            bool wrap) const override;
    [[nodiscard]] float LineHeight(float fontSize) const override;
    [[nodiscard]] std::uint32_t HitTestCaret(std::string_view text,
                                             float fontSize,
                                             float maxWidth,
                                             bool wrap,
                                             float x,
                                             float y) const override;
    [[nodiscard]] ui::UiCaretRect CaretRect(std::string_view text,
                                            float fontSize,
                                            float maxWidth,
                                            bool wrap,
                                            std::uint32_t byteOffset) const override;
    [[nodiscard]] bool LayoutGlyphs(std::string_view text,
                                    float fontSize,
                                    float maxWidth,
                                    bool wrap,
                                    std::vector<ui::UiGlyphQuad>& glyphs) const override;

    [[nodiscard]] bool FontAtlas(std::vector<unsigned char>& rgba, int& width, int& height, std::uint64_t& revision) const;
    [[nodiscard]] CacheStats Stats() const noexcept;
    void ClearCache();
    // 字形以物理像素栅格化，布局、光标和命中测试继续使用逻辑坐标。
    bool SetRasterScale(float pixelsPerDip);
    [[nodiscard]] const std::string& LastError() const noexcept;

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

} // namespace se
