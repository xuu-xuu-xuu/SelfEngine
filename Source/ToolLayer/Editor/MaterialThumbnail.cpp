#include "MaterialThumbnail.h"

#include <algorithm>
#include <cmath>

namespace se
{
namespace
{
std::uint8_t Channel(float value)
{
    return static_cast<std::uint8_t>(std::clamp(value, 0.0f, 1.0f) * 255.0f + 0.5f);
}
} // namespace

MaterialThumbnail RenderMaterialThumbnail(const Vec4& baseColor, float metallic, float roughness,
    std::uint32_t size)
{
    MaterialThumbnail output;
    output.width = std::max(1u, size);
    output.height = output.width;
    output.rgba8.assign(static_cast<std::size_t>(output.width) * output.height * 4, 0);
    const Vec3 light = Normalize(Vec3{-0.4f, 0.6f, 0.7f});
    const Vec3 view{0.0f, 0.0f, 1.0f};
    const float shininess = 8.0f + (1.0f - std::clamp(roughness, 0.0f, 1.0f)) * 120.0f;
    const float specularStrength = (1.0f - std::clamp(roughness, 0.0f, 1.0f)) * 0.8f;
    const float metal = std::clamp(metallic, 0.0f, 1.0f);
    for (std::uint32_t y = 0; y < output.height; ++y)
    {
        for (std::uint32_t x = 0; x < output.width; ++x)
        {
            const float nx = (static_cast<float>(x) + 0.5f) / output.width * 2.0f - 1.0f;
            const float ny = 1.0f - (static_cast<float>(y) + 0.5f) / output.height * 2.0f;
            const float radius = nx * nx + ny * ny;
            std::size_t offset = (static_cast<std::size_t>(y) * output.width + x) * 4;
            if (radius > 1.0f)
            {
                continue;
            }
            const float nz = std::sqrt(std::max(0.0f, 1.0f - radius));
            const Vec3 normal{nx, ny, nz};
            const float diffuse = std::max(0.0f, Dot(normal, light));
            const Vec3 half = Normalize(light + view);
            const float specular = std::pow(std::max(0.0f, Dot(normal, half)), shininess) * specularStrength;
            const Vec3 albedo = {baseColor.x, baseColor.y, baseColor.z};
            const Vec3 diffuseColor = albedo * (1.0f - metal);
            const Vec3 specularColor = albedo * metal + Vec3{1.0f - metal, 1.0f - metal, 1.0f - metal};
            const Vec3 color = diffuseColor * (0.18f + 0.82f * diffuse) + specularColor * specular;
            output.rgba8[offset + 0] = Channel(color.x);
            output.rgba8[offset + 1] = Channel(color.y);
            output.rgba8[offset + 2] = Channel(color.z);
            output.rgba8[offset + 3] = 255;
        }
    }
    return output;
}
} // namespace se
