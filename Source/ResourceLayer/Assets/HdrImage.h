#pragma once
#include "StaticModel.h"
#include "CoreLayer/Math/Math.h"
#include <span>

namespace se
{
struct FloatImage
{
    std::uint32_t width=0,height=0;
    std::vector<Vec3> pixels; // 顶行开始，线性 RGB，保留 HDR 范围。
};
bool DecodeRadiance(std::span<const std::uint8_t> bytes,FloatImage& output,std::vector<AssetDiagnostic>& diagnostics);
bool LoadRadiance(const std::filesystem::path& path,FloatImage& output,std::vector<AssetDiagnostic>& diagnostics);
bool WriteRadiance(const std::filesystem::path& path,const FloatImage& image,std::vector<AssetDiagnostic>& diagnostics);
}
