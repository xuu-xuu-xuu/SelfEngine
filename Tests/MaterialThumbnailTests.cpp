#include "ToolLayer/Editor/MaterialThumbnail.h"
#include <cmath>
#include <iostream>

#define CHECK(condition) do { if (!(condition)) { std::cerr << "FAIL line " << __LINE__ << ": " << #condition << '\n'; return 1; } } while (false)

int main()
{
    const auto red = se::RenderMaterialThumbnail({1.0f, 0.0f, 0.0f, 1.0f}, 0.0f, 0.35f, 32);
    CHECK(red.width == 32 && red.height == 32, "thumbnail dimensions");
    CHECK(red.rgba8.size() == 32u * 32u * 4u, "thumbnail bytes");
    const auto center = (16u * 32u + 16u) * 4u;
    CHECK(red.rgba8[center + 3] == 255, "center opaque");
    CHECK(red.rgba8[center] > red.rgba8[center + 1], "base color contributes red");
    CHECK(red.rgba8[0] == 0 && red.rgba8[3] == 0, "corner transparent");

    const auto smooth = se::RenderMaterialThumbnail({1.0f, 1.0f, 1.0f, 1.0f}, 0.0f, 0.05f, 32);
    const auto rough = se::RenderMaterialThumbnail({1.0f, 1.0f, 1.0f, 1.0f}, 0.0f, 0.95f, 32);
    long smoothSum = 0, roughSum = 0;
    for (std::size_t index = 0; index < smooth.rgba8.size(); index += 4)
    {
        smoothSum += smooth.rgba8[index];
        roughSum += rough.rgba8[index];
    }
    CHECK(smoothSum != roughSum, "roughness changes highlight");

    const auto metal = se::RenderMaterialThumbnail({0.8f, 0.6f, 0.2f, 1.0f}, 1.0f, 0.2f, 32);
    CHECK(metal.rgba8.size() == red.rgba8.size(), "metal thumbnail size");
    std::cout << "MaterialThumbnailTests passed (sphere shape, base color, roughness highlight).\n";
    return 0;
}
