#include "FunctionLayer/UI/Core/UiLayout.h"
#include "FunctionLayer/UI/Core/UiStyle.h"

#include <cmath>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <limits>
#include <random>
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>

namespace
{
using namespace se::ui;

void Check(bool condition, const char* message)
{
    if (!condition)
    {
        throw std::runtime_error(message);
    }
}

void Near(float actual, float expected, const char* message)
{
    if (std::abs(actual - expected) > 0.01f)
    {
        throw std::runtime_error(std::string(message) + " (actual " + std::to_string(actual) +
                                 ", expected " + std::to_string(expected) + ")");
    }
}

void CheckRect(const UiLayoutEngine& layout,
               UiElementHandle element,
               float x,
               float y,
               float width,
               float height,
               const char* message)
{
    const UiRect rect = layout.Rect(element);
    if (std::abs(rect.x - x) > 0.01f || std::abs(rect.y - y) > 0.01f ||
        std::abs(rect.width - width) > 0.01f || std::abs(rect.height - height) > 0.01f)
    {
        throw std::runtime_error(std::string(message) + " (rect " + std::to_string(rect.x) + "," +
                                 std::to_string(rect.y) + "," + std::to_string(rect.width) + "," +
                                 std::to_string(rect.height) + ")");
    }
}

void CheckFinite(const UiRect& rect, const char* message)
{
    Check(std::isfinite(rect.x) && std::isfinite(rect.y) &&
              std::isfinite(rect.width) && std::isfinite(rect.height),
          message);
    Check(rect.width >= 0.0f && rect.height >= 0.0f, "rect sizes are non-negative");
}

UiStyle Box(float width, float height)
{
    UiStyle style;
    style.width = UiLength::Px(width);
    style.height = UiLength::Px(height);
    return style;
}

struct Fixture
{
    UiDocument document;
    UiStubTextService text{8.0f, 16.0f};
    UiLayoutEngine layout{document, &text};
    UiElementHandle root = document.Root();

    UiElementHandle Add(UiElementKind kind, UiElementHandle parent, const UiStyle& style)
    {
        const UiElementHandle element = document.CreateElement(kind);
        Check(document.SetParent(element, parent), "attach layout element");
        Check(document.SetStyle(element, style), "set layout style");
        return element;
    }

    UiElementHandle Box(UiElementHandle parent, float width, float height)
    {
        return Add(UiElementKind::Panel, parent, ::Box(width, height));
    }
};

void FixedAndPadding()
{
    Fixture fixture;
    UiStyle rootStyle;
    rootStyle.padding = UiEdges{10.0f, 20.0f, 30.0f, 40.0f};
    Check(fixture.document.SetStyle(fixture.root, rootStyle), "root padding");
    const UiElementHandle child = fixture.Box(fixture.root, 100.0f, 50.0f);
    Check(fixture.layout.Layout(400.0f, 300.0f), "fixed layout");
    CheckRect(fixture.layout, child, 10.0f, 20.0f, 100.0f, 50.0f, "fixed child with padding");
    CheckRect(fixture.layout, fixture.root, 0.0f, 0.0f, 400.0f, 300.0f, "root fills viewport");
}

void GapAndMargin()
{
    Fixture fixture;
    UiStyle rootStyle;
    rootStyle.gap = 6.0f;
    Check(fixture.document.SetStyle(fixture.root, rootStyle), "root gap");
    const UiElementHandle first = fixture.Box(fixture.root, 50.0f, 20.0f);
    const UiElementHandle second = fixture.Box(fixture.root, 70.0f, 30.0f);
    const UiElementHandle third = fixture.Box(fixture.root, 40.0f, 10.0f);
    UiStyle thirdStyle = fixture.document.Style(third);
    thirdStyle.margin = UiEdges{5.0f, 7.0f, 0.0f, 0.0f};
    Check(fixture.document.SetStyle(third, thirdStyle), "third margin");
    Check(fixture.layout.Layout(400.0f, 300.0f), "gap layout");
    CheckRect(fixture.layout, first, 0.0f, 0.0f, 50.0f, 20.0f, "first gap item");
    CheckRect(fixture.layout, second, 56.0f, 0.0f, 70.0f, 30.0f, "second gap item");
    CheckRect(fixture.layout, third, 137.0f, 7.0f, 40.0f, 10.0f, "margin item");
}

void PercentAndViewport()
{
    Fixture fixture;
    UiStyle halfStyle;
    halfStyle.width = UiLength::Percent(50.0f);
    halfStyle.height = UiLength::Px(20.0f);
    const UiElementHandle half = fixture.Add(UiElementKind::Panel, fixture.root, halfStyle);
    UiStyle tallStyle;
    tallStyle.width = UiLength::Px(30.0f);
    tallStyle.height = UiLength::Percent(50.0f);
    const UiElementHandle tall = fixture.Add(UiElementKind::Panel, fixture.root, tallStyle);
    Check(fixture.layout.Layout(400.0f, 300.0f), "percentage layout");
    CheckRect(fixture.layout, half, 0.0f, 0.0f, 200.0f, 20.0f, "percent width");
    CheckRect(fixture.layout, tall, 200.0f, 0.0f, 30.0f, 150.0f, "percent height");
    Check(fixture.layout.Layout(800.0f, 600.0f), "viewport relayout");
    CheckRect(fixture.layout, half, 0.0f, 0.0f, 400.0f, 20.0f, "percent width after resize");
    CheckRect(fixture.layout, tall, 400.0f, 0.0f, 30.0f, 300.0f, "percent height after resize");
}

void GrowAndShrink()
{
    {
        Fixture fixture;
        UiStyle firstStyle = Box(100.0f, 20.0f);
        firstStyle.flexGrow = 1.0f;
        UiStyle secondStyle = Box(100.0f, 20.0f);
        secondStyle.flexGrow = 3.0f;
        const UiElementHandle first = fixture.Add(UiElementKind::Panel, fixture.root, firstStyle);
        const UiElementHandle second = fixture.Add(UiElementKind::Panel, fixture.root, secondStyle);
        Check(fixture.layout.Layout(400.0f, 300.0f), "grow layout");
        CheckRect(fixture.layout, first, 0.0f, 0.0f, 150.0f, 20.0f, "grow 1");
        CheckRect(fixture.layout, second, 150.0f, 0.0f, 250.0f, 20.0f, "grow 3");
    }
    {
        Fixture fixture;
        UiStyle style = Box(150.0f, 20.0f);
        const UiElementHandle first = fixture.Add(UiElementKind::Panel, fixture.root, style);
        const UiElementHandle second = fixture.Add(UiElementKind::Panel, fixture.root, style);
        Check(fixture.layout.Layout(200.0f, 300.0f), "shrink layout");
        CheckRect(fixture.layout, first, 0.0f, 0.0f, 100.0f, 20.0f, "shrink 1");
        CheckRect(fixture.layout, second, 100.0f, 0.0f, 100.0f, 20.0f, "shrink 2");
    }
}

void MinMax()
{
    Fixture fixture;
    UiStyle minimumStyle = Box(100.0f, 20.0f);
    minimumStyle.minWidth = UiLength::Px(150.0f);
    UiStyle maximumStyle = Box(100.0f, 20.0f);
    maximumStyle.maxWidth = UiLength::Px(50.0f);
    const UiElementHandle minimum = fixture.Add(UiElementKind::Panel, fixture.root, minimumStyle);
    const UiElementHandle maximum = fixture.Add(UiElementKind::Panel, fixture.root, maximumStyle);
    Check(fixture.layout.Layout(400.0f, 300.0f), "min/max layout");
    CheckRect(fixture.layout, minimum, 0.0f, 0.0f, 150.0f, 20.0f, "min width");
    CheckRect(fixture.layout, maximum, 150.0f, 0.0f, 50.0f, 20.0f, "max width");
}

void JustifyAndAlign()
{
    {
        Fixture fixture;
        UiStyle rootStyle;
        rootStyle.justifyContent = UiJustifyContent::Center;
        fixture.document.SetStyle(fixture.root, rootStyle);
        const UiElementHandle first = fixture.Box(fixture.root, 100.0f, 50.0f);
        const UiElementHandle second = fixture.Box(fixture.root, 100.0f, 50.0f);
        fixture.layout.Layout(400.0f, 300.0f);
        CheckRect(fixture.layout, first, 100.0f, 0.0f, 100.0f, 50.0f, "justify center 1");
        CheckRect(fixture.layout, second, 200.0f, 0.0f, 100.0f, 50.0f, "justify center 2");
    }
    {
        Fixture fixture;
        UiStyle rootStyle;
        rootStyle.justifyContent = UiJustifyContent::FlexEnd;
        fixture.document.SetStyle(fixture.root, rootStyle);
        const UiElementHandle first = fixture.Box(fixture.root, 100.0f, 50.0f);
        const UiElementHandle second = fixture.Box(fixture.root, 100.0f, 50.0f);
        fixture.layout.Layout(400.0f, 300.0f);
        CheckRect(fixture.layout, first, 200.0f, 0.0f, 100.0f, 50.0f, "justify end 1");
        CheckRect(fixture.layout, second, 300.0f, 0.0f, 100.0f, 50.0f, "justify end 2");
    }
    {
        Fixture fixture;
        UiStyle rootStyle;
        rootStyle.justifyContent = UiJustifyContent::SpaceBetween;
        fixture.document.SetStyle(fixture.root, rootStyle);
        const UiElementHandle first = fixture.Box(fixture.root, 100.0f, 50.0f);
        const UiElementHandle second = fixture.Box(fixture.root, 100.0f, 50.0f);
        fixture.layout.Layout(400.0f, 300.0f);
        CheckRect(fixture.layout, first, 0.0f, 0.0f, 100.0f, 50.0f, "space-between 1");
        CheckRect(fixture.layout, second, 300.0f, 0.0f, 100.0f, 50.0f, "space-between 2");
    }
    {
        Fixture fixture;
        UiStyle rootStyle;
        rootStyle.alignItems = UiAlignItems::Center;
        fixture.document.SetStyle(fixture.root, rootStyle);
        const UiElementHandle child = fixture.Box(fixture.root, 100.0f, 50.0f);
        fixture.layout.Layout(400.0f, 300.0f);
        CheckRect(fixture.layout, child, 0.0f, 125.0f, 100.0f, 50.0f, "align center");
    }
    {
        Fixture fixture;
        UiStyle rootStyle;
        rootStyle.alignItems = UiAlignItems::FlexEnd;
        fixture.document.SetStyle(fixture.root, rootStyle);
        const UiElementHandle child = fixture.Box(fixture.root, 100.0f, 50.0f);
        fixture.layout.Layout(400.0f, 300.0f);
        CheckRect(fixture.layout, child, 0.0f, 250.0f, 100.0f, 50.0f, "align end");
    }
    {
        Fixture fixture;
        UiStyle rootStyle;
        rootStyle.alignItems = UiAlignItems::Stretch;
        fixture.document.SetStyle(fixture.root, rootStyle);
        const UiElementHandle child = fixture.Add(UiElementKind::Panel, fixture.root, [] {
            UiStyle style;
            style.width = UiLength::Px(100.0f);
            return style;
        }());
        fixture.layout.Layout(400.0f, 300.0f);
        CheckRect(fixture.layout, child, 0.0f, 0.0f, 100.0f, 300.0f, "align stretch");
    }
}

void ColumnStack()
{
    Fixture fixture;
    UiStyle rootStyle;
    rootStyle.flexDirection = UiFlexDirection::Column;
    fixture.document.SetStyle(fixture.root, rootStyle);
    const UiElementHandle first = fixture.Box(fixture.root, 50.0f, 20.0f);
    const UiElementHandle second = fixture.Box(fixture.root, 70.0f, 30.0f);
    fixture.layout.Layout(400.0f, 300.0f);
    CheckRect(fixture.layout, first, 0.0f, 0.0f, 50.0f, 20.0f, "column first");
    CheckRect(fixture.layout, second, 0.0f, 20.0f, 70.0f, 30.0f, "column second");
}

void ZeroAndNegative()
{
    Fixture fixture;
    UiStyle negative;
    negative.width = UiLength::Px(-10.0f);
    negative.height = UiLength::Px(-5.0f);
    const UiElementHandle child = fixture.Add(UiElementKind::Panel, fixture.root, negative);
    Check(fixture.layout.Layout(300.0f, 300.0f), "negative size layout");
    CheckRect(fixture.layout, child, 0.0f, 0.0f, 0.0f, 0.0f, "negative size clamps to zero");
    Check(fixture.layout.Layout(-100.0f, -100.0f), "negative viewport layout");
    CheckRect(fixture.layout, fixture.root, 0.0f, 0.0f, 0.0f, 0.0f, "negative viewport clamps");
}

void AbsoluteAndDisplay()
{
    Fixture fixture;
    UiStyle absolute;
    absolute.position = UiPosition::Absolute;
    absolute.width = UiLength::Px(80.0f);
    absolute.height = UiLength::Px(40.0f);
    absolute.left = UiLength::Px(20.0f);
    absolute.top = UiLength::Px(30.0f);
    const UiElementHandle first = fixture.Add(UiElementKind::Panel, fixture.root, absolute);
    UiStyle secondStyle = absolute;
    secondStyle.left = UiLength::Auto();
    secondStyle.top = UiLength::Auto();
    secondStyle.right = UiLength::Px(10.0f);
    secondStyle.bottom = UiLength::Px(20.0f);
    const UiElementHandle second = fixture.Add(UiElementKind::Panel, fixture.root, secondStyle);
    Check(fixture.layout.Layout(400.0f, 300.0f), "absolute layout");
    CheckRect(fixture.layout, first, 20.0f, 30.0f, 80.0f, 40.0f, "absolute left/top");
    CheckRect(fixture.layout, second, 310.0f, 240.0f, 80.0f, 40.0f, "absolute right/bottom");

    UiStyle hiddenStyle = Box(50.0f, 20.0f);
    hiddenStyle.display = UiDisplay::None;
    const UiElementHandle hidden = fixture.Add(UiElementKind::Panel, fixture.root, hiddenStyle);
    Check(fixture.layout.Layout(400.0f, 300.0f), "display none layout");
    CheckRect(fixture.layout, hidden, 0.0f, 0.0f, 0.0f, 0.0f, "display none rect");
}

void TextAndStub()
{
    UiStubTextService stub(10.0f, 20.0f);
    const UiTextMeasure empty = stub.Measure("", 14.0f, 100.0f, true);
    Check(empty.width == 0.0f && empty.height == 0.0f && empty.lineCount == 0, "stub empty text");
    const UiTextMeasure simple = stub.Measure("abc", 14.0f, 100.0f, true);
    Near(simple.width, 30.0f, "stub text width");
    Near(simple.height, 20.0f, "stub line height");
    const UiTextMeasure wrappedText = stub.Measure("abc", 14.0f, 15.0f, true);
    Near(wrappedText.width, 15.0f, "stub wrapped width");
    Near(wrappedText.height, 60.0f, "stub wrapped height");
    Check(stub.HitTestCaret("abc", 14.0f, 100.0f, false, 25.0f, 0.0f) == 2, "stub caret hit");
    const UiCaretRect caret = stub.CaretRect("abc", 14.0f, 100.0f, false, 2);
    Near(caret.x, 20.0f, "stub caret x");
    Near(caret.height, 20.0f, "stub caret height");

    Fixture fixture;
    UiStyle rootStyle;
    rootStyle.alignItems = UiAlignItems::FlexStart;
    fixture.document.SetStyle(fixture.root, rootStyle);
    const UiElementHandle label = fixture.document.CreateElement(UiElementKind::Label);
    fixture.document.SetParent(label, fixture.root);
    fixture.document.SetText(label, "abcd");
    const UiElementHandle wrapped = fixture.document.CreateElement(UiElementKind::Label);
    fixture.document.SetParent(wrapped, fixture.root);
    fixture.document.SetText(wrapped, "abcdefgh");
    UiStyle wrappedStyle;
    wrappedStyle.width = UiLength::Px(40.0f);
    wrappedStyle.alignItems = UiAlignItems::FlexStart;
    fixture.document.SetStyle(wrapped, wrappedStyle);
    Check(fixture.layout.Layout(400.0f, 300.0f), "text layout");
    CheckRect(fixture.layout, label, 0.0f, 0.0f, 32.0f, 16.0f, "auto text size");
    CheckRect(fixture.layout, wrapped, 32.0f, 0.0f, 40.0f, 32.0f, "wrapped text size");
}

void NestedAndDeep()
{
    {
        Fixture fixture;
        UiStyle rootStyle;
        rootStyle.padding = UiEdges{10.0f, 10.0f, 0.0f, 0.0f};
        fixture.document.SetStyle(fixture.root, rootStyle);
        UiStyle containerStyle = Box(200.0f, 100.0f);
        containerStyle.padding = UiEdges{5.0f, 5.0f, 0.0f, 0.0f};
        const UiElementHandle container = fixture.Add(UiElementKind::Panel, fixture.root, containerStyle);
        const UiElementHandle child = fixture.Box(container, 50.0f, 20.0f);
        fixture.layout.Layout(400.0f, 300.0f);
        CheckRect(fixture.layout, container, 10.0f, 10.0f, 200.0f, 100.0f, "nested container");
        CheckRect(fixture.layout, child, 15.0f, 15.0f, 50.0f, 20.0f, "nested child");
    }
    {
        Fixture fixture;
        UiStyle rootStyle;
        rootStyle.flexDirection = UiFlexDirection::Column;
        fixture.document.SetStyle(fixture.root, rootStyle);
        UiElementHandle parent = fixture.root;
        UiElementHandle deepest = fixture.root;
        for (int index = 0; index < 128; ++index)
        {
            UiStyle style;
            style.width = UiLength::Px(100.0f);
            style.flexDirection = UiFlexDirection::Column;
            deepest = fixture.Add(UiElementKind::Panel, parent, style);
            parent = deepest;
        }
        const UiElementHandle leaf = fixture.Box(parent, 10.0f, 10.0f);
        Check(fixture.layout.Layout(200.0f, 200.0f), "deep layout");
        CheckFinite(fixture.layout.Rect(deepest), "deep container finite");
        CheckFinite(fixture.layout.Rect(leaf), "deep leaf finite");
        CheckRect(fixture.layout, leaf, 0.0f, 0.0f, 10.0f, 10.0f, "deep leaf rect");
    }
}

void DirtyAndCache()
{
    Fixture fixture;
    std::vector<UiElementHandle> children;
    for (int index = 0; index < 100; ++index)
    {
        children.push_back(fixture.Box(fixture.root, 50.0f, 20.0f));
    }
    Check(fixture.layout.Layout(8000.0f, 300.0f), "cache first layout");
    Check(fixture.layout.Stats().nodesMeasured == 101, "first layout measures the tree");
    Check(fixture.layout.Stats().layoutPasses == 1, "first layout pass");

    Check(!fixture.layout.Layout(8000.0f, 300.0f), "stable frame skips layout");
    Check(fixture.layout.Stats().layoutPasses == 0, "stable frame has zero passes");
    Check(fixture.layout.Stats().nodesMeasured == 0, "stable frame measures nothing");

    UiStyle changed = fixture.document.Style(children[42]);
    changed.width = UiLength::Px(60.0f);
    Check(fixture.document.SetStyle(children[42], changed), "change one width");
    Check(fixture.layout.Layout(8000.0f, 300.0f), "dirty subtree layout");
    const UiLayoutStats stats = fixture.layout.Stats();
    Check(stats.nodesMeasured >= 2 && stats.nodesMeasured < 101, "only dirty path measured");
    Check(stats.measureCacheHits >= 99, "siblings reuse measure cache");
    CheckRect(fixture.layout, children[42], 50.0f * 42.0f, 0.0f, 60.0f, 20.0f, "changed width");

    Check(fixture.layout.Layout(9000.0f, 300.0f), "viewport resize");
    Check(fixture.layout.Stats().nodesMeasured == 101, "viewport resize measures all");
    CheckRect(fixture.layout, fixture.root, 0.0f, 0.0f, 9000.0f, 300.0f, "root after resize");
}

void Fuzz()
{
    std::mt19937 rng(1337);
    const auto nextFloat = [&rng](float minimum, float maximum) {
        std::uniform_real_distribution<float> distribution(minimum, maximum);
        return distribution(rng);
    };
    const auto nextInt = [&rng](int minimum, int maximum) {
        std::uniform_int_distribution<int> distribution(minimum, maximum);
        return distribution(rng);
    };

    Fixture fixture;
    std::vector<UiElementHandle> nodes{fixture.root};
    for (int index = 0; index < 80; ++index)
    {
        const UiElementHandle parent = nodes[static_cast<std::size_t>(nextInt(0, static_cast<int>(nodes.size()) - 1))];
        UiStyle style;
        style.width = nextInt(0, 2) == 0 ? UiLength::Auto() : UiLength::Px(nextFloat(0.0f, 200.0f));
        style.height = nextInt(0, 2) == 0 ? UiLength::Auto() : UiLength::Px(nextFloat(0.0f, 120.0f));
        style.padding = UiEdges{nextFloat(0.0f, 12.0f), nextFloat(0.0f, 12.0f),
                                nextFloat(0.0f, 12.0f), nextFloat(0.0f, 12.0f)};
        style.margin = UiEdges{nextFloat(0.0f, 6.0f), nextFloat(0.0f, 6.0f),
                               nextFloat(0.0f, 6.0f), nextFloat(0.0f, 6.0f)};
        style.gap = nextFloat(0.0f, 8.0f);
        style.flexDirection = nextInt(0, 1) == 0 ? UiFlexDirection::Row : UiFlexDirection::Column;
        style.justifyContent = static_cast<UiJustifyContent>(nextInt(0, 3));
        style.alignItems = static_cast<UiAlignItems>(nextInt(0, 3));
        style.flexGrow = nextFloat(0.0f, 2.0f);
        style.flexShrink = nextFloat(0.0f, 2.0f);
        style.display = nextInt(0, 20) == 0 ? UiDisplay::None : UiDisplay::Flex;
        if (nextInt(0, 10) == 0)
        {
            style.position = UiPosition::Absolute;
            style.left = UiLength::Px(nextFloat(0.0f, 80.0f));
            style.top = UiLength::Px(nextFloat(0.0f, 80.0f));
        }
        if (nextInt(0, 8) == 0)
        {
            style.minWidth = UiLength::Px(nextFloat(0.0f, 80.0f));
            style.maxWidth = UiLength::Px(nextFloat(80.0f, 240.0f));
        }
        const UiElementHandle node = fixture.Add(UiElementKind::Panel, parent, style);
        nodes.push_back(node);
    }

    Check(fixture.layout.Layout(600.0f, 400.0f), "fuzz layout");
    for (const UiElementHandle node : nodes)
    {
        CheckFinite(fixture.layout.Rect(node), "fuzz rect finite");
    }
    Check(!fixture.layout.Layout(600.0f, 400.0f), "fuzz stable second frame");

    UiStyle changed = fixture.document.Style(nodes[40]);
    changed.width = UiLength::Px(75.0f);
    Check(fixture.document.SetStyle(nodes[40], changed), "fuzz style change");
    Check(fixture.layout.Layout(600.0f, 400.0f), "fuzz relayout after change");
}

} // namespace

int main(int argc, char** argv)
{
    try
    {
        FixedAndPadding();
        GapAndMargin();
        PercentAndViewport();
        GrowAndShrink();
        MinMax();
        JustifyAndAlign();
        ColumnStack();
        ZeroAndNegative();
        AbsoluteAndDisplay();
        TextAndStub();
        NestedAndDeep();
        DirtyAndCache();
        Fuzz();

        if (argc > 1)
        {
            const std::filesystem::path proof = argv[1];
            if (!proof.parent_path().empty())
            {
                std::filesystem::create_directories(proof.parent_path());
            }
            std::ofstream(proof)
                << "{\"success\":true,\"fixed\":true,\"percent\":true,\"growShrink\":true,"
                   "\"gapMargin\":true,\"nested\":true,\"absolute\":true,\"minMax\":true,"
                   "\"zeroNegative\":true,\"deep\":true,\"cache\":true,\"fuzz\":true,"
                   "\"textStub\":true}\n";
        }
        std::cout << "UiCoreLayoutTests passed (flex subset, percentages, grow/shrink, absolute, cache, fuzz).\n";
        return 0;
    }
    catch (const std::exception& exception)
    {
        std::cerr << "[FAIL] " << exception.what() << '\n';
        return 1;
    }
}
