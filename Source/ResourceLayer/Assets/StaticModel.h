#pragma once

#include <array>
#include <cstdint>
#include <filesystem>
#include <string>
#include <vector>

namespace se
{
using AssetId = std::string; // 128 位随机 ID，以 32 个小写十六进制字符保存。
// CPU 中间数据，右手 Y-up、米，矩阵按列存储并乘列向量；不是 GPU 顶点布局。
struct MeshVertex
{
    std::array<float, 3> position{};
    std::array<float, 3> normal{ 0.0f, 1.0f, 0.0f };
    std::array<float, 2> uv{};
    std::array<float, 4> tangent{ 1.0f, 0.0f, 0.0f, 1.0f };
};

struct TextureReference
{
    std::string relativePath; // 相对模型文件目录；不保存个人机器绝对路径。
    std::string mimeType;
    std::vector<std::uint8_t> embeddedBytes;
    bool missing = false;
};

struct MaterialData
{
    std::string name;
    std::array<float, 4> baseColor{ 1.0f, 1.0f, 1.0f, 1.0f };
    float metallic = 0.0f;
    float roughness = 1.0f;
    std::int32_t baseColorTexture = -1;
    std::int32_t normalTexture = -1;
    std::int32_t metallicRoughnessTexture = -1;
    bool doubleSided = false;
    bool needsFallbackTexture = false;
    float normalScale = 1.0f;
};

struct MeshData
{
    std::string name;
    std::vector<MeshVertex> vertices;
    std::vector<std::uint32_t> indices;
    std::int32_t material = -1;
};

struct ModelNode
{
    std::string name;
    std::int32_t parent = -1;
    std::array<float, 16> localMatrix{
        1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1 };
    std::vector<std::uint32_t> meshes;
};

struct StaticModel
{
    std::vector<MeshData> meshes;
    std::vector<MaterialData> materials;
    std::vector<ModelNode> nodes;
    std::vector<TextureReference> textures;
    std::vector<std::string> dependencies; // 相对模型目录的外部缓冲/贴图，包含缺失项。
};

struct AssetDiagnostic
{
    std::string key;
    std::string detail;
    bool warning = false;
};

struct ImportOptions
{
    float scale = 1.0f; // 在源单位归一到米之后，额外缩放几何及节点平移。
    bool generateTangents = true;
    std::filesystem::path allowedRoot; // 空值表示模型所在目录；外部引用不得越界。
};

inline constexpr std::uint32_t StaticModelFormatVersion = 1;
inline constexpr const char* StaticModelImporterVersion = "SelfEngine.StaticModel.2/ufbx-0.17.1/cgltf-1.15";

// 所有数据由引擎独立拥有；失败不改变 output。诊断只携带稳定文本键与细节。
bool ImportStaticModel(const std::filesystem::path& path, const ImportOptions& options,
    StaticModel& output, std::vector<AssetDiagnostic>& diagnostics);
bool ValidateStaticModel(const StaticModel& model);
}
