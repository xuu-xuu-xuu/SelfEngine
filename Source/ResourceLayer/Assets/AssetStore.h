#pragma once

#include "StaticModel.h"
#include "MaterialDocument.h"
#include "Environment.h"
#include "ResourceLayer/Environment/TODProfile.h"
#include <memory>
#include <span>
#include <unordered_map>

namespace se
{
struct ImageData
{
    std::uint32_t width = 0;
    std::uint32_t height = 0;
    std::vector<std::uint8_t> rgba8;
};

// 在目录所有者线程复制，后台只持有此值；不借用 AssetStore 的记录或缓存。
struct AssetSourceSnapshot
{
    AssetId id;
    std::filesystem::path source, sourceRoot, cacheRoot;
    ImportOptions options;
};

// 已注册的用户 Shader 产物：只暴露路径与内容身份，容器内容由渲染端严格验证。
struct ShaderArtifactSource
{
    std::filesystem::path source;
    std::string content_sha256;
};

// CPU 图片解码；颜色空间用途由渲染端决定，不把法线图等数据自动转为 sRGB。
bool DecodeImage(const std::filesystem::path& path, ImageData& output,
    std::vector<AssetDiagnostic>& diagnostics, std::uint64_t maximumRgbaBytes = 256ull * 1024 * 1024);
bool DecodeImage(std::span<const std::uint8_t> encoded, ImageData& output,
    std::vector<AssetDiagnostic>& diagnostics, std::uint64_t maximumRgbaBytes = 256ull * 1024 * 1024); // 支持嵌入图片。

// 单线程目录与共享 CPU 缓存。跨线程任务只调用独立导入/解码函数，再由主线程发布。
class AssetStore
{
public:
    AssetStore(std::filesystem::path sourceRoot, std::filesystem::path cacheRoot);
    bool Scan(std::vector<AssetDiagnostic>& diagnostics);
    bool RegisterSource(const std::filesystem::path& source, const ImportOptions& options,
        AssetId& id, std::vector<AssetDiagnostic>& diagnostics);
    std::shared_ptr<const StaticModel> LoadModel(const AssetId& id,
        std::vector<AssetDiagnostic>& diagnostics);
    std::shared_ptr<const ImageData> LoadTextureImage(const AssetId& id,
        std::vector<AssetDiagnostic>& diagnostics, std::uint64_t maximumRgbaBytes = 256ull * 1024 * 1024);
    std::shared_ptr<const MaterialDocument> LoadMaterial(const AssetId& id,
        std::vector<AssetDiagnostic>& diagnostics);
    // 只读校验并返回 .sebin 产物路径与内容 SHA-256；不做容器解析，不读取 HLSL。
    bool LoadShaderArtifact(const AssetId& id, ShaderArtifactSource& output,
        std::vector<AssetDiagnostic>& diagnostics) const;
    std::shared_ptr<const EnvironmentAsset> LoadEnvironment(const AssetId& id,
        std::vector<AssetDiagnostic>& diagnostics);
    std::shared_ptr<const TODProfile> LoadTODProfile(const AssetId& id,std::vector<AssetDiagnostic>& diagnostics);
    bool DescribeSource(const AssetId& id, AssetSourceSnapshot& output,
        std::vector<AssetDiagnostic>& diagnostics) const;
    static std::shared_ptr<const StaticModel> LoadModelSnapshot(const AssetSourceSnapshot& source,
        std::vector<AssetDiagnostic>& diagnostics);
    static std::shared_ptr<const ImageData> LoadImageSnapshot(const AssetSourceSnapshot& source,
        std::vector<AssetDiagnostic>& diagnostics, std::uint64_t maximumRgbaBytes = 256ull * 1024 * 1024);
    bool SaveMaterialSource(const std::filesystem::path& path,const MaterialDocument& material,
        AssetId& id,std::vector<AssetDiagnostic>& diagnostics);
    const std::filesystem::path& SourceRoot() const noexcept { return sourceRoot_; }
    std::filesystem::path Resolve(const AssetId& id) const;
    std::vector<AssetId> ListIds() const;
    void CollectUnused();

private:
    struct Record { std::filesystem::path source; ImportOptions options; };
    struct ModelEntry { std::string key; std::weak_ptr<const StaticModel> data; };
    struct ImageEntry { std::string key; std::weak_ptr<const ImageData> data; };
    struct MaterialEntry { std::string key; std::weak_ptr<const MaterialDocument> data; };
    struct EnvironmentEntry { std::string key; std::weak_ptr<const EnvironmentAsset> data; };
    struct TODEntry { std::string key;std::weak_ptr<const TODProfile> data; };
    std::filesystem::path sourceRoot_;
    std::filesystem::path cacheRoot_;
    std::unordered_map<AssetId, Record> records_;
    std::unordered_map<AssetId, ModelEntry> models_;
    std::unordered_map<AssetId, ImageEntry> images_;
    std::unordered_map<AssetId, MaterialEntry> materials_;
    std::unordered_map<AssetId, EnvironmentEntry> environments_;
    std::unordered_map<AssetId,TODEntry> todProfiles_;
};
}
