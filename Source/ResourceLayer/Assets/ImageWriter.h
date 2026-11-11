#pragma once
#include "StaticModel.h"
#include <span>
namespace se
{
// RGBA8 转为 P6 RGB；文件整体原子发布，不依赖平台图像编码器。
bool WritePpmImage(const std::filesystem::path& path,std::uint32_t width,std::uint32_t height,std::span<const std::uint8_t> rgba8,std::vector<AssetDiagnostic>& diagnostics);
}
