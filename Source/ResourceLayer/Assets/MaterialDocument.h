#pragma once
#include "StaticModel.h"
#include <string_view>

namespace se
{
// 独立 CPU 材质资产；贴图只引用 AssetId，实际 Shader/PSO/描述符由渲染器决定。
struct MaterialDocument
{
    std::string name;
    std::array<float,4> baseColor{1,1,1,1};
    float metallic=0,roughness=1;
    bool doubleSided=false;
    AssetId baseColorTexture,normalTexture,metallicRoughnessTexture;
    float normalScale=1;
    // 可选用户 PBR Shader 产物（.sebin）；空表示内置默认 PBR，旧材质缺省行为不变。
    AssetId shader;
};
// 编辑器当前帧的自有 CPU 预览，不改变源文件或资源身份。
struct MaterialPreview { AssetId id;MaterialDocument document; };
bool IsMaterialDocumentPath(const std::filesystem::path& path);
bool ValidateMaterialDocument(const MaterialDocument& material);
std::vector<AssetId> MaterialDependencies(const MaterialDocument& material);
std::vector<AssetId> MaterialTextureDependencies(const MaterialDocument& material);
bool SerializeMaterialDocument(const MaterialDocument& material,std::string& output,std::vector<AssetDiagnostic>& diagnostics);
bool DeserializeMaterialDocument(std::string_view text,MaterialDocument& output,std::vector<AssetDiagnostic>& diagnostics);
bool SaveMaterialDocument(const std::filesystem::path& path,const MaterialDocument& material,std::vector<AssetDiagnostic>& diagnostics);
bool LoadMaterialDocument(const std::filesystem::path& path,MaterialDocument& output,std::vector<AssetDiagnostic>& diagnostics);
}
