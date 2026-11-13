#include "FunctionLayer/UI/Core/UiDocument.h"
#include "FunctionLayer/UI/Core/UiLayout.h"
#include "FunctionLayer/UI/Core/UiStyle.h"
#include "FunctionLayer/UI/Interfaces/UiTextService.h"
#include "PlatformLayer/Windows/UiDirectWriteTextService.h"
#include "ResourceLayer/Fonts/FontCache.h"

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <stdexcept>
#include <string>
#include <string_view>

namespace
{
using namespace se;

void Check(bool condition, const char* message)
{
    if (!condition)
    {
        throw std::runtime_error(message);
    }
}

void Near(float actual, float expected, float tolerance, const char* message)
{
    if (std::abs(actual - expected) > tolerance)
    {
        throw std::runtime_error(std::string(message) + " (actual " + std::to_string(actual) +
                                 ", expected " + std::to_string(expected) + ")");
    }
}

std::uint32_t ByteOffsetForCodepoint(std::string_view text, std::size_t codepointIndex)
{
    std::size_t byteOffset = 0;
    std::size_t current = 0;
    while (byteOffset < text.size() && current < codepointIndex)
    {
        const unsigned char lead = static_cast<unsigned char>(text[byteOffset]);
        std::size_t length = 1;
        if ((lead & 0xE0u) == 0xC0u) length = 2;
        else if ((lead & 0xF0u) == 0xE0u) length = 3;
        else if ((lead & 0xF8u) == 0xF0u) length = 4;
        byteOffset += std::min(length, text.size() - byteOffset);
        ++current;
    }
    return static_cast<std::uint32_t>(byteOffset);
}

void CompareWithFontCache()
{
    FontCache fonts;
    UiDirectWriteTextService service;
    const struct Case
    {
        const char* text;
        float size;
        float width;
    } cases[] = {
        {"Hello", 14.0f, 1000.0f},
        {"中文ABC", 18.0f, 160.0f},
        {"A中😀B", 16.0f, 1000.0f},
        {"混合 mixed 文本", 20.0f, 120.0f},
    };
    for (const Case& item : cases)
    {
        se::TextLayout layout;
        std::string error;
        Check(fonts.Layout(item.text, item.size, item.width, layout, error), error.c_str());
        const ui::UiTextMeasure measure = service.Measure(item.text, item.size, item.width, true);
        Near(measure.width, layout.width, 0.5f, "adapter and FontCache width");
        Near(measure.height, layout.height, 0.5f, "adapter and FontCache height");
        Check(measure.lineCount == layout.lineCount, "adapter and FontCache line count");
    }

    const ui::UiTextMeasure empty = service.Measure("", 14.0f, 100.0f, true);
    Check(empty.width == 0.0f && empty.height == 0.0f && empty.lineCount == 0, "empty text");
    Check(service.LineHeight(16.0f) > 0.0f, "line height positive");
    Check(service.LastError().empty(), "valid text clears last error");

    const std::string longText(4000, 'W');
    const ui::UiTextMeasure longMeasure = service.Measure(longText, 16.0f, 200.0f, true);
    Check(longMeasure.width > 0.0f && longMeasure.height > 0.0f && longMeasure.lineCount > 1, "long wrapped text");
}

void CacheAndDpi()
{
    UiDirectWriteTextService service;
    service.ClearCache();
    const auto before = service.Stats();
    const ui::UiTextMeasure first = service.Measure("cache", 14.0f, 100.0f, true);
    Check(first.width > 0.0f, "cache first measure");
    const auto afterFirst = service.Stats();
    Check(afterFirst.misses == before.misses + 1 && afterFirst.entries == 1, "cache miss and entry");
    const ui::UiTextMeasure second = service.Measure("cache", 14.0f, 100.0f, true);
    Check(second.width == first.width, "cache hit returns same width");
    Check(service.Stats().hits == afterFirst.hits + 1, "cache hit counted");
    (void)service.Measure("cache", 14.0f, 120.0f, true);
    Check(service.Stats().misses == afterFirst.misses + 1, "width change misses cache");
    (void)service.Measure("cache", 16.0f, 100.0f, true);
    Check(service.Stats().misses == afterFirst.misses + 2, "size change misses cache");
    (void)service.Measure("cache", 14.0f, 100.0f, false);
    Check(service.Stats().misses == afterFirst.misses + 3, "wrap change misses cache");
    service.ClearCache();
    Check(service.Stats().entries == 0, "clear cache entries");

    const ui::UiTextMeasure dpi = service.Measure("cache", 28.0f, 100.0f, true);
    Check(dpi.height > first.height, "font size affects line height");
}

void CaretAndHit()
{
    UiDirectWriteTextService service;
    FontCache fonts;
    const std::string text = "A中B";
    const float size = 16.0f;
    const float width = 1000.0f;
    se::TextLayout layout;
    std::string error;
    Check(fonts.Layout(text, size, width, layout, error), error.c_str());

    const std::uint32_t firstByte = ByteOffsetForCodepoint(text, 1);
    const std::uint32_t secondByte = ByteOffsetForCodepoint(text, 2);
    Check(firstByte == 1 && secondByte == 4, "UTF-8 byte offsets");

    const se::UiRect firstCaret = layout.Caret(1);
    const ui::UiCaretRect caret = service.CaretRect(text, size, width, true, firstByte);
    Near(caret.x, firstCaret.x, 0.1f, "caret x");
    Near(caret.y, firstCaret.y, 0.1f, "caret y");
    Near(caret.height, firstCaret.height, 0.1f, "caret height");

    const float hitX = firstCaret.x + 0.1f;
    const float hitY = firstCaret.y + firstCaret.height * 0.5f;
    const std::size_t codepoint = layout.HitTest(hitX, hitY);
    Check(service.HitTestCaret(text, size, width, true, hitX, hitY) == ByteOffsetForCodepoint(text, codepoint),
          "hit test matches FontCache codepoint");
    Check(service.HitTestCaret(text, size, width, true, -10.0f, -10.0f) == 0, "hit clamps to start");
}

void LayoutIntegration()
{
    ui::UiDocument document;
    UiDirectWriteTextService service;
    ui::UiLayoutEngine layout(document, &service);
    const ui::UiElementHandle root = document.Root();
    ui::UiStyle rootStyle;
    rootStyle.alignItems = ui::UiAlignItems::FlexStart;
    Check(document.SetStyle(root, rootStyle), "root style");

    const ui::UiElementHandle label = document.CreateElement(ui::UiElementKind::Label);
    const ui::UiElementHandle sibling = document.CreateElement(ui::UiElementKind::Label);
    Check(document.SetParent(label, root), "label parent");
    Check(document.SetParent(sibling, root), "sibling parent");
    Check(document.SetText(label, "中文ABC"), "label text");
    Check(document.SetText(sibling, "Hello"), "sibling text");
    Check(layout.Layout(600.0f, 400.0f), "text layout");

    const ui::UiTextMeasure labelMeasure = service.Measure("中文ABC", 14.0f, 600.0f, true);
    Near(layout.Rect(label).width, labelMeasure.width, 0.5f, "layout label width");
    Near(layout.Rect(label).height, labelMeasure.height, 0.5f, "layout label height");
    const float siblingWidth = layout.Rect(sibling).width;
    Check(document.SetText(label, "Hello world"), "change text");
    Check(layout.Layout(600.0f, 400.0f), "text change relayout");
    Check(layout.Rect(label).width != labelMeasure.width, "changed text changes width");
    Near(layout.Rect(sibling).width, siblingWidth, 0.5f, "sibling width unchanged");
    Check(!layout.Layout(600.0f, 400.0f), "stable text frame");
}

void ImeTypes()
{
    ui::UiTextComposition composition;
    composition.utf8 = "中";
    composition.caretByte = 3;
    composition.active = true;
    Check(composition.active && composition.caretByte == 3, "IME composition value type");
    const ui::UiImeCandidateAnchor anchor{10.0f, 20.0f, 30.0f, 40.0f};
    Near(anchor.x, 10.0f, 0.0f, "IME candidate x");
    Near(anchor.height, 40.0f, 0.0f, "IME candidate height");
}

void FontAtlasRevision()
{
    UiDirectWriteTextService service;
    std::vector<ui::UiGlyphQuad> glyphs;
    Check(service.LayoutGlyphs("Console ready", 18.0f, 400.0f, true, glyphs), "font atlas initial layout");
    std::vector<unsigned char> rgba;
    int width = 0;
    int height = 0;
    std::uint64_t first = 0;
    Check(service.FontAtlas(rgba, width, height, first), "font atlas initial build");
    Check(width > 0 && height > 0 && !rgba.empty(), "font atlas pixels");
    Check(service.LayoutGlyphs("测试字形修订", 18.0f, 400.0f, true, glyphs), "font atlas new glyphs");
    std::uint64_t second = 0;
    Check(service.FontAtlas(rgba, width, height, second), "font atlas rebuild");
    Check(second > first, "font atlas revision must change after new glyphs");
}

void PhysicalRasterScale()
{
    UiDirectWriteTextService service;
    const std::string text="Editor 属性";
    const auto original=service.Measure(text,14.0f,500.0f,false);
    const auto caret=service.CaretRect(text,14.0f,500.0f,false,3);
    for(const float scale:{1.0f,1.25f,1.5f,2.0f})
    {
        Check(service.SetRasterScale(scale),"set physical raster density");
        const auto measured=service.Measure(text,14.0f,500.0f,false);
        Near(measured.width,original.width,0.01f,"DPI cannot change logical text width");
        Near(measured.height,original.height,0.01f,"DPI cannot change logical line height");
        Near(service.CaretRect(text,14.0f,500.0f,false,3).x,caret.x,0.01f,"DPI preserves caret position");
        std::vector<ui::UiGlyphQuad> glyphs;
        Check(service.LayoutGlyphs(text,14.0f,500.0f,false,glyphs) && !glyphs.empty(),"physical glyph layout");
        std::vector<unsigned char> rgba;int width=0,height=0;std::uint64_t revision=0;
        Check(service.FontAtlas(rgba,width,height,revision),"physical atlas");
        for(const auto& glyph:glyphs)
        {
            Near((glyph.u1-glyph.u0)*width,glyph.width*scale,0.01f,"one texture texel per physical glyph pixel");
            Near((glyph.v1-glyph.v0)*height,glyph.height*scale,0.01f,"physical glyph height is not upscaled");
        }
    }
    const auto entries=service.Stats().entries;
    Check(service.SetRasterScale(2.0f) && service.Stats().entries==entries,"unchanged DPI retains the layout cache");
    Check(!service.SetRasterScale(0.0f),"invalid raster scale rejected");
}

} // namespace

int main(int argc, char** argv)
{
    try
    {
        CompareWithFontCache();
        CacheAndDpi();
        CaretAndHit();
        LayoutIntegration();
        ImeTypes();
        FontAtlasRevision();
        PhysicalRasterScale();

        if (argc > 1)
        {
            const std::filesystem::path proof = argv[1];
            if (!proof.parent_path().empty())
            {
                std::filesystem::create_directories(proof.parent_path());
            }
            std::ofstream(proof)
                << "{\"success\":true,\"fontCacheCompare\":true,\"cache\":true,\"caret\":true,"
                   "\"layoutIntegration\":true,\"imeTypes\":true,\"dpi\":true,\"fontRevision\":true}\n";
        }
        std::cout << "UiCoreTextTests passed (DirectWrite adapter, FontCache parity, cache, caret, layout, IME types).\n";
        return 0;
    }
    catch (const std::exception& exception)
    {
        std::cerr << "[FAIL] " << exception.what() << '\n';
        return 1;
    }
}
