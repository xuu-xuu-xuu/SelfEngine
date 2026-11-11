#pragma once
#include "HdrImage.h"
#include <array>

namespace se
{
inline constexpr const char* EnvironmentCookerVersion="SelfEngine.Environment.1";
struct EnvironmentSettings
{
    std::uint32_t cubeSize=64,irradianceSize=16,lutSize=64,samples=512;
    bool operator==(const EnvironmentSettings&) const=default;
};
struct EnvironmentTexture
{
    std::uint32_t size=0,mipLevels=1;
    bool cube=false;
    // 每项为紧密 RGBA binary16，索引 mip + face*mipLevels；普通 LUT 只有一层。
    std::vector<std::vector<std::uint16_t>> subresources;
};
struct EnvironmentAsset
{
    EnvironmentSettings settings;
    std::string sourceHash,cookKey;
    std::array<EnvironmentTexture,3> textures; // irradiance、prefiltered radiance、BRDF LUT。
};
bool ValidateEnvironmentSettings(const EnvironmentSettings& settings);
bool ValidateEnvironment(const EnvironmentAsset& asset);
std::string EnvironmentCookKey(std::string_view sourceHash,const EnvironmentSettings& settings);
Vec3 EnvironmentCubeDirection(std::uint32_t face,float u,float v); // u/v 为 0..1，+X/-X/+Y/-Y/+Z/-Z，顶行开始。
Vec3 SampleEquirectangular(const FloatImage& image,Vec3 direction);
bool BakeEnvironment(const FloatImage& image,std::string sourceHash,const EnvironmentSettings& settings,
    EnvironmentAsset& output,std::vector<AssetDiagnostic>& diagnostics);
bool SaveEnvironment(const std::filesystem::path& path,const EnvironmentAsset& asset,std::vector<AssetDiagnostic>& diagnostics);
bool DecodeEnvironment(std::span<const std::uint8_t> bytes,EnvironmentAsset& output,std::vector<AssetDiagnostic>& diagnostics);
bool LoadEnvironment(const std::filesystem::path& path,EnvironmentAsset& output,std::vector<AssetDiagnostic>& diagnostics);
bool CookEnvironment(const std::filesystem::path& source,const std::filesystem::path& destination,
    const EnvironmentSettings& settings,bool& cacheHit,std::vector<AssetDiagnostic>& diagnostics);
}
