#pragma once
#include "StaticModel.h"
#include "CoreLayer/Math/Projection.h"
#include <array>
#include <cstdint>
#include <filesystem>
#include <string>
#include <vector>

namespace se
{
// G05.12：16 视角 Impostor 图集。离线把网格按 22.5° 方位角渲染到 4×4 图集，
// alpha 通道保存覆盖（运行时 alpha test），颜色为漫反射近似。运行时可插值相邻两视角。
inline constexpr std::uint32_t ImpostorViewCount=16;
struct ImpostorAtlas
{
    std::uint32_t tileSize=0,columns=0,rows=0;
    std::vector<std::uint8_t> rgba; // columns*tileSize × rows*tileSize，RGBA8，行优先。
    std::uint32_t Width() const noexcept {return columns*tileSize;}
    std::uint32_t Height() const noexcept {return rows*tileSize;}
    std::size_t PixelOffset(std::uint32_t view,std::uint32_t x,std::uint32_t y) const;
};
struct ImpostorBakeSettings
{
    std::uint32_t tileSize=64;
    Vec3 color{0.85f,0.35f,0.08f};
    Vec3 lightDirection{0.4f,0.6f,0.3f};
    float ambient=0.35f;
};
// 网格按 meshToWorld 后的世界空间烘焙；视角 k 的相机方位角为 k*2π/16（从 +Z 绕 +Y）。
bool BakeImpostorAtlas(const MeshData& mesh,const Mat4& meshToWorld,const ImpostorBakeSettings& settings,
    ImpostorAtlas& output,std::vector<AssetDiagnostic>& diagnostics);
// 方位角 → 最近视角 / 相邻视角插值对（first、second 为图集行内视角序号，blend∈[0,1)）。
std::uint32_t ImpostorViewIndex(float azimuthRadians);
void ImpostorViewPair(float azimuthRadians,std::uint32_t& first,std::uint32_t& second,float& blend);
// .seimp：magic/版本 + tile/columns/rows + 图集像素；读取时校验尺寸与尾部字节。
bool WriteImpostorAtlas(const std::filesystem::path& path,const ImpostorAtlas& atlas,std::string& error);
bool ReadImpostorAtlas(const std::filesystem::path& path,ImpostorAtlas& output,std::string& error);
}
