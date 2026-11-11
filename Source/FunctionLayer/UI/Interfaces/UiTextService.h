#pragma once

#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

namespace se::ui
{

struct UiTextMeasure
{
    float width = 0.0f;
    float height = 0.0f;
    std::uint32_t lineCount = 0;
};

struct UiCaretRect
{
    float x = 0.0f;
    float y = 0.0f;
    float width = 0.0f;
    float height = 0.0f;
};

// IME 组合串与候选位置的值类型；Win32 适配在输入阶段实现。
struct UiTextComposition
{
    std::string utf8;
    std::uint32_t caretByte = 0;
    bool active = false;
};

struct UiImeCandidateAnchor
{
    float x = 0.0f;
    float y = 0.0f;
    float width = 0.0f;
    float height = 0.0f;
};

struct UiGlyphQuad
{
    float x = 0.0f;
    float y = 0.0f;
    float width = 0.0f;
    float height = 0.0f;
    float u0 = 0.0f;
    float v0 = 0.0f;
    float u1 = 1.0f;
    float v1 = 1.0f;
    std::uint32_t page = 0;
};

// 文本测量/整形接口。平台实现（DirectWrite）在阶段 F 注入；
// 核心和布局只依赖这些值类型与方法，不 include 平台头。
class UiTextService
{
public:
    virtual ~UiTextService() = default;

    [[nodiscard]] virtual UiTextMeasure Measure(std::string_view text,
                                                float fontSize,
                                                float maxWidth,
                                                bool wrap) const = 0;
    [[nodiscard]] virtual float LineHeight(float fontSize) const = 0;
    [[nodiscard]] virtual std::uint32_t HitTestCaret(std::string_view text,
                                                     float fontSize,
                                                     float maxWidth,
                                                     bool wrap,
                                                     float x,
                                                     float y) const = 0;
    [[nodiscard]] virtual UiCaretRect CaretRect(std::string_view text,
                                                float fontSize,
                                                float maxWidth,
                                                bool wrap,
                                                std::uint32_t byteOffset) const = 0;
    // 字形几何由平台实现提供；默认不支持，避免强制所有服务实现。
    virtual bool LayoutGlyphs(std::string_view text,
                              float fontSize,
                              float maxWidth,
                              bool wrap,
                              std::vector<UiGlyphQuad>& glyphs) const
    {
        (void)text;
        (void)fontSize;
        (void)maxWidth;
        (void)wrap;
        glyphs.clear();
        return false;
    }
};

// 阶段 D 的固定宽高 Stub：按 UTF-8 字节数近似，不替代阶段 F 的真实整形。
class UiStubTextService final : public UiTextService
{
public:
    explicit UiStubTextService(float charWidth = 8.0f, float lineHeight = 16.0f) noexcept;

    [[nodiscard]] UiTextMeasure Measure(std::string_view text,
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
    [[nodiscard]] UiCaretRect CaretRect(std::string_view text,
                                        float fontSize,
                                        float maxWidth,
                                        bool wrap,
                                        std::uint32_t byteOffset) const override;
    [[nodiscard]] bool LayoutGlyphs(std::string_view text,
                                    float fontSize,
                                    float maxWidth,
                                    bool wrap,
                                    std::vector<UiGlyphQuad>& glyphs) const override;

    [[nodiscard]] float CharacterWidth() const noexcept;
    [[nodiscard]] float StubLineHeight() const noexcept;

private:
    float charWidth_ = 8.0f;
    float lineHeight_ = 16.0f;
};

} // namespace se::ui
