#include "FunctionLayer/Rendering/UiPaint.h"
#include "FunctionLayer/UI/Core/UiDocument.h"
#include "FunctionLayer/UI/Core/UiLayout.h"
#include "FunctionLayer/UI/Core/UiStyle.h"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>

namespace
{
using namespace se;
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

struct ImageProvider final : UiImageProvider
{
    mutable int calls = 0;
    bool Resolve(std::string_view image, UiImageInfo& info) const override
    {
        ++calls;
        if (image == "hero")
        {
            info = {42, 64.0f, 64.0f};
            return true;
        }
        if (image == "bad")
        {
            info = {0, 0.0f, 0.0f};
            return true;
        }
        return false;
    }
};

void CommandOrder()
{
    UiDocument document;
    UiStubTextService text(8.0f, 16.0f);
    UiLayoutEngine layout(document, &text);
    const UiElementHandle root = document.Root();
    UiStyle rootStyle;
    rootStyle.background = UiColor{0.1f, 0.1f, 0.1f, 1.0f};
    rootStyle.flexDirection = UiFlexDirection::Column;
    rootStyle.alignItems = UiAlignItems::FlexStart;
    document.SetStyle(root, rootStyle);
    const UiElementHandle child = document.CreateElement(UiElementKind::Panel);
    document.SetParent(child, root);
    UiStyle childStyle;
    childStyle.width = UiLength::Px(100.0f);
    childStyle.height = UiLength::Px(50.0f);
    childStyle.background = UiColor{0.2f, 0.3f, 0.4f, 1.0f};
    document.SetStyle(child, childStyle);
    document.SetText(child, "Ab");

    Check(layout.Layout(200.0f, 100.0f), "layout order");
    UiPainter painter(document, layout);
    UiDisplayList list;
    UiPaintStats stats;
    Check(painter.Build(root, list, stats), "paint order");
    Check(stats.nodes == 2, "paint node count");
    Check(list.width == 200.0f && list.height == 100.0f, "display size");
    Check(list.commands.size() >= 4, "display commands");
    Check(list.commands.front().kind == UiPaintCommandKind::ClipPush, "root clip first");
    Check(list.commands.back().kind == UiPaintCommandKind::ClipPop, "root clip last");

    std::vector<UiPaintCommandKind> kinds;
    for (const UiPaintCommand& command : list.commands)
    {
        if (command.kind != UiPaintCommandKind::ClipPush && command.kind != UiPaintCommandKind::ClipPop)
        {
            kinds.push_back(command.kind);
        }
    }
    Check(kinds.size() >= 3, "background/text commands");
    Check(kinds[0] == UiPaintCommandKind::Rect, "root background first");
    Check(kinds[1] == UiPaintCommandKind::Rect, "child background before text");
    Check(kinds.back() == UiPaintCommandKind::Text, "text after children");
}

void ClipAndCapacity()
{
    UiDisplayList list;
    list.width = 100.0f;
    list.height = 100.0f;
    list.PushClip({0.0f, 0.0f, 100.0f, 100.0f});
    list.PushClip({25.0f, 25.0f, 50.0f, 50.0f});
    list.Rect({0.0f, 0.0f, 100.0f, 100.0f}, UiColor{1.0f, 1.0f, 1.0f, 1.0f});
    list.PopClip();
    list.PopClip();
    GuiGeometry geometry;
    UiGeometryStats stats;
    std::vector<std::string> errors;
    Check(UiGeometryBuilder{}.Build(list, nullptr, nullptr, {}, geometry, stats, errors), "clip build");
    Check(geometry.commands.size() == 1, "one clipped command");
    Near(geometry.commands[0].clip.x, 25.0f, "clip left");
    Near(geometry.commands[0].clip.y, 25.0f, "clip top");
    Near(geometry.commands[0].clip.z, 75.0f, "clip right");
    Near(geometry.commands[0].clip.w, 75.0f, "clip bottom");

    UiDisplayList empty;
    empty.width = 100.0f;
    empty.height = 100.0f;
    GuiGeometry emptyGeometry;
    UiGeometryStats emptyStats;
    Check(UiGeometryBuilder{}.Build(empty, nullptr, nullptr, {}, emptyGeometry, emptyStats, errors), "empty build");
    Check(emptyGeometry.commands.empty() && emptyGeometry.vertices.empty(), "empty geometry");

    UiDisplayList huge;
    huge.width = 100.0f;
    huge.height = 100.0f;
    huge.Rect({-50.0f, -50.0f, 10000.0f, 10000.0f}, UiColor{1.0f, 0.0f, 0.0f, 1.0f});
    GuiGeometry hugeGeometry;
    UiGeometryStats hugeStats;
    Check(UiGeometryBuilder{}.Build(huge, nullptr, nullptr, {}, hugeGeometry, hugeStats, errors), "huge node build");
    Check(hugeGeometry.commands.size() == 1, "huge node clipped");
    Near(hugeGeometry.commands[0].clip.z, 100.0f, "huge clip right");
}

void NineSliceAndIndices()
{
    UiDisplayList list;
    list.width = 100.0f;
    list.height = 100.0f;
    list.NineSlice({0.0f, 0.0f, 90.0f, 90.0f}, {0.0f, 0.0f, 1.0f, 1.0f}, 10.0f, 10.0f, 10.0f, 10.0f,
                   7, UiColor{1.0f, 1.0f, 1.0f, 1.0f});
    GuiGeometry geometry;
    UiGeometryStats stats;
    std::vector<std::string> errors;
    Check(UiGeometryBuilder{}.Build(list, nullptr, nullptr, {}, geometry, stats, errors), "nine slice build");
    Check(geometry.vertices.size() == 36, "nine slice vertices");
    Check(geometry.indices.size() == 54, "nine slice indices");
    Check(stats.quads == 9, "nine slice quads");
    Near(geometry.vertices[0].uv.x, 0.0f, "nine slice u0");
    Near(geometry.vertices[0].uv.y, 0.0f, "nine slice v0");
    Near(geometry.vertices[2].uv.x, 10.0f / 90.0f, "nine slice u1");

    for (const GuiCommand& command : geometry.commands)
    {
        Check(command.indexOffset + command.indexCount <= geometry.indices.size(), "index range");
        for (std::uint32_t index = command.indexOffset; index < command.indexOffset + command.indexCount; ++index)
        {
            Check(geometry.indices[index] < geometry.vertices.size(), "index points to vertex");
        }
    }
}

void CanvasAndInvalidImage()
{
    UiDisplayList list;
    list.width = 200.0f;
    list.height = 200.0f;
    list.Line(10.0f, 10.0f, 100.0f, 50.0f, 2.0f, UiColor{1.0f, 1.0f, 1.0f, 1.0f});
    list.CanvasRect({20.0f, 20.0f, 30.0f, 40.0f}, UiColor{0.0f, 1.0f, 0.0f, 1.0f});
    list.CanvasCircle(100.0f, 100.0f, 20.0f, UiColor{1.0f, 0.0f, 0.0f, 1.0f});
    GuiGeometry geometry;
    UiGeometryStats stats;
    std::vector<std::string> errors;
    Check(UiGeometryBuilder{}.Build(list, nullptr, nullptr, {}, geometry, stats, errors), "canvas build");
    Check(stats.quads > 30 && !geometry.indices.empty(), "canvas geometry");

    UiDisplayList invalid;
    invalid.width = 100.0f;
    invalid.height = 100.0f;
    invalid.Image({0.0f, 0.0f, 50.0f, 50.0f}, "bad", UiColor{1.0f, 1.0f, 1.0f, 1.0f});
    ImageProvider provider;
    GuiGeometry invalidGeometry;
    UiGeometryStats invalidStats;
    std::vector<std::string> invalidErrors;
    Check(!UiGeometryBuilder{}.Build(invalid, nullptr, &provider, {}, invalidGeometry, invalidStats, invalidErrors),
          "invalid image rejected");
    Check(invalidStats.rejected == 1 && !invalidErrors.empty(), "invalid image diagnostic");
    Check(invalidGeometry.commands.empty(), "invalid image no geometry");
}

std::uint64_t HashPpm(const std::vector<std::uint8_t>& pixels)
{
    std::uint64_t hash = 1469598103934665603ull;
    for (const std::uint8_t value : pixels)
    {
        hash ^= value;
        hash *= 1099511628211ull;
    }
    return hash;
}

void BuildGoldenPpm(GuiGeometry& geometry)
{
    UiDocument document;
    UiStubTextService text(8.0f, 16.0f);
    UiLayoutEngine layout(document, &text);
    const UiElementHandle root = document.Root();
    UiStyle rootStyle;
    rootStyle.background = UiColor{0.078f, 0.086f, 0.102f, 1.0f};
    rootStyle.flexDirection = UiFlexDirection::Column;
    rootStyle.alignItems = UiAlignItems::FlexStart;
    rootStyle.padding = UiEdges{40.0f, 40.0f, 40.0f, 40.0f};
    document.SetStyle(root, rootStyle);

    const UiElementHandle panel = document.CreateElement(UiElementKind::Panel);
    document.SetParent(panel, root);
    UiStyle panelStyle;
    panelStyle.width = UiLength::Px(800.0f);
    panelStyle.height = UiLength::Px(500.0f);
    panelStyle.background = UiColor{0.118f, 0.133f, 0.165f, 1.0f};
    panelStyle.borderWidth = 2.0f;
    panelStyle.borderColor = UiColor{0.165f, 0.184f, 0.227f, 1.0f};
    panelStyle.padding = UiEdges{24.0f, 24.0f, 24.0f, 24.0f};
    panelStyle.flexDirection = UiFlexDirection::Column;
    document.SetStyle(panel, panelStyle);

    const UiElementHandle title = document.CreateElement(UiElementKind::Label);
    document.SetParent(title, panel);
    document.SetText(title, "UI Toolkit");
    UiStyle titleStyle;
    titleStyle.width = UiLength::Px(300.0f);
    titleStyle.height = UiLength::Px(40.0f);
    titleStyle.fontSize = 32.0f;
    titleStyle.color = UiColor{0.949f, 0.957f, 0.973f, 1.0f};
    document.SetStyle(title, titleStyle);

    const UiElementHandle image = document.CreateElement(UiElementKind::Image);
    document.SetParent(image, panel);
    UiStyle imageStyle;
    imageStyle.width = UiLength::Px(96.0f);
    imageStyle.height = UiLength::Px(96.0f);
    imageStyle.image = "hero";
    document.SetStyle(image, imageStyle);

    Check(layout.Layout(1920.0f, 1080.0f), "golden layout");
    UiPainter painter(document, layout);
    UiDisplayList list;
    UiPaintStats paintStats;
    Check(painter.Build(root, list, paintStats), "golden paint");

    ImageProvider provider;
    UiGeometryStats stats;
    std::vector<std::string> errors;
    Check(UiGeometryBuilder{}.Build(list, &text, &provider, {}, geometry, stats, errors), "golden geometry");
    Check(geometry.vertices.size() > 0 && !geometry.commands.empty(), "golden geometry populated");
}

void Golden(const char* path)
{
    GuiGeometry geometry;
    BuildGoldenPpm(geometry);
    const std::uint32_t width = 1920;
    const std::uint32_t height = 1080;
    std::vector<std::uint8_t> pixels(static_cast<std::size_t>(width) * height * 3, 0);
    const auto blend = [&pixels, width](int x, int y, std::uint32_t color) {
        if (x < 0 || y < 0 || x >= static_cast<int>(width) || y >= static_cast<int>(height))
        {
            return;
        }
        const float alpha = static_cast<float>((color >> 24) & 0xFFu) / 255.0f;
        if (alpha <= 0.0f)
        {
            return;
        }
        const std::size_t offset = (static_cast<std::size_t>(y) * width + static_cast<std::size_t>(x)) * 3;
        const float source[3] = {static_cast<float>(color & 0xFFu), static_cast<float>((color >> 8) & 0xFFu),
                                 static_cast<float>((color >> 16) & 0xFFu)};
        for (int channel = 0; channel < 3; ++channel)
        {
            const float destination = static_cast<float>(pixels[offset + channel]);
            pixels[offset + channel] = static_cast<std::uint8_t>(std::lround(source[channel] * alpha + destination * (1.0f - alpha)));
        }
    };

    for (const GuiCommand& command : geometry.commands)
    {
        for (std::uint32_t index = command.indexOffset; index + 5 < command.indexOffset + command.indexCount; index += 6)
        {
            const GuiVertex& v0 = geometry.vertices[geometry.indices[index]];
            const GuiVertex& v1 = geometry.vertices[geometry.indices[index + 1]];
            const GuiVertex& v2 = geometry.vertices[geometry.indices[index + 2]];
            const GuiVertex& v3 = geometry.vertices[geometry.indices[index + 5]];
            (void)v1;
            (void)v2;
            const float left = std::max({v0.position.x, v1.position.x, v2.position.x, v3.position.x, command.clip.x});
            const float top = std::max({v0.position.y, v1.position.y, v2.position.y, v3.position.y, command.clip.y});
            const float right = std::min({v0.position.x, v1.position.x, v2.position.x, v3.position.x, command.clip.z});
            const float bottom = std::min({v0.position.y, v1.position.y, v2.position.y, v3.position.y, command.clip.w});
            const int x0 = static_cast<int>(std::floor(left));
            const int y0 = static_cast<int>(std::floor(top));
            const int x1 = static_cast<int>(std::ceil(right));
            const int y1 = static_cast<int>(std::ceil(bottom));
            for (int y = y0; y < y1; ++y)
            {
                for (int x = x0; x < x1; ++x)
                {
                    blend(x, y, v0.color);
                }
            }
        }
    }

    const std::uint64_t hash = HashPpm(pixels);
    std::cout << "UiPaint golden hash=" << std::hex << hash << std::dec << '\n';
    const std::uint64_t expected = 0xe33e2d8dffc1383ull;
    Check(hash == expected, "golden hash baseline");

    if (path && *path)
    {
        const std::filesystem::path outputPath(path);
        if (!outputPath.parent_path().empty())
        {
            std::filesystem::create_directories(outputPath.parent_path());
        }
        std::ofstream output(outputPath, std::ios::binary);
        output << "P6\n" << width << ' ' << height << "\n255\n";
        output.write(reinterpret_cast<const char*>(pixels.data()), static_cast<std::streamsize>(pixels.size()));
    }
}

} // namespace

int main(int argc, char** argv)
{
    try
    {
        CommandOrder();
        ClipAndCapacity();
        NineSliceAndIndices();
        CanvasAndInvalidImage();
        Golden(argc > 2 ? argv[2] : nullptr);

        if (argc > 1)
        {
            const std::filesystem::path proof = argv[1];
            if (!proof.parent_path().empty())
            {
                std::filesystem::create_directories(proof.parent_path());
            }
            std::ofstream(proof)
                << "{\"success\":true,\"commandOrder\":true,\"clip\":true,\"nineSlice\":true,"
                   "\"indices\":true,\"empty\":true,\"huge\":true,\"invalidToken\":true,"
                   "\"canvas\":true,\"golden\":true}\n";
        }
        std::cout << "UiCorePaintTests passed (display list, clip, 9-slice, geometry, canvas, golden).\n";
        return 0;
    }
    catch (const std::exception& exception)
    {
        std::cerr << "[FAIL] " << exception.what() << '\n';
        return 1;
    }
}
