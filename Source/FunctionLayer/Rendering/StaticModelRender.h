#pragma once

#include "ResourceLayer/Assets/AssetStore.h"
#include "ResourceLayer/Assets/StaticModelPreparation.h"
#include "ResourceLayer/Assets/TextureBake.h"
#include "ResourceLayer/Assets/LodBake.h"
#include "CoreLayer/Math/Projection.h"
#include "PlatformLayer/Graphics/Rhi.h"
#include "TextureResidency.h"
#include <memory>
#include <optional>
#include <unordered_map>

namespace se
{
enum class ModelRenderState { Ready, Fallback };
struct PbrRenderMaterial
{
    rhi::Texture baseColor,normal,metallicRoughness;
    Vec4 color{1,1,1,1};
    float metallic=0,roughness=1,normalScale=1;
    bool normalMapping=false,doubleSided=false;
    std::uint32_t featureMask=0; // G02：由贴图槽推导，供变体清单与后续 PSO 选择。
    // 材质引用的用户 PBR 产物；空表示使用调用方传入的默认 PBR Pipeline。
    rhi::Pipeline pipeline;
};
struct StaticRenderMesh {rhi::Buffer vertices,indices;std::vector<rhi::Buffer> lodIndices;std::shared_ptr<const LodChain> lod;rhi::Texture texture;std::shared_ptr<const PbrRenderMaterial> material;std::uint32_t indexCount=0;std::uint32_t vertexLayoutVersion=rhi::VertexLayoutVersionLegacy;Vec3 quantizationMinimum{};float quantizationExtent=1;};
void ConfigurePbrMaterial(const MaterialData* material,PbrRenderMaterial& output,
    std::string_view name,std::vector<AssetDiagnostic>& diagnostics);
struct StaticRenderModel
{
    std::shared_ptr<const StaticModel> source;
    StaticModelLayout layout;
    std::vector<StaticRenderMesh> meshes;
    std::vector<bool> fallbackMeshes;
    ModelRenderState state=ModelRenderState::Ready;
    bool compressed=false;
};
class StaticModelRenderCache
{
public:
    explicit StaticModelRenderCache(bool pbr=false):pbr_(pbr) {}
    // 仅呈现线程；失败时返回已有有效版本或空指针，诊断仍记录本次失败。
    std::shared_ptr<const StaticRenderModel> Load(AssetStore& assets,const AssetId& id,
        rhi::Device& device,std::vector<AssetDiagnostic>& diagnostics);
    bool Upload(std::shared_ptr<const StaticModel> model,const std::filesystem::path& source,
        const std::filesystem::path& allowedRoot,rhi::Device& device,
        std::shared_ptr<const StaticRenderModel>& output,std::vector<AssetDiagnostic>& diagnostics,bool compress=false,
        std::uint32_t lodLevels=0);
    // 调用方先等待最后的 GPU 使用结束。
    std::shared_ptr<const PbrRenderMaterial> LoadMaterial(AssetStore& assets,const AssetId& id,
        rhi::Device& device,std::vector<AssetDiagnostic>& diagnostics,const MaterialDocument* preview=nullptr);
    void Clear() noexcept { models_.clear();attempted_.clear();materials_.clear();textures_.clear();bakeCache_.Clear();compressedUploads_=0;residency_.Clear();residencyFrame_=0;pendingDegradations_=0; }
    void Prune(const AssetStore* assets,std::span<const AssetId> needed,std::span<const AssetId> neededMaterials={});
    std::size_t TextureCount() const noexcept;
    std::size_t ModelCount() const noexcept { return models_.size(); }
    // G04：压缩派生纹理。默认关闭保持 RGBA8；目录非空时读写 .sebc 跨进程缓存。
    // bc1Color → baseColor 用 BC1 sRGB；bc1Data → MR 用 BC1 线性；bc5Normal → 法线用 BC5（XY，Shader 重建 Z）。
    void EnableBc1Textures(bool enabled,std::filesystem::path bakeDirectory={},bool dataTextures=false);
    void SetBc5Normals(bool enabled){bc5Normals_=enabled;}
    bool Bc1TexturesEnabled() const noexcept {return bc1Textures_;}
    bool Bc1DataTexturesEnabled() const noexcept {return bc1DataTextures_;}
    bool Bc5NormalsEnabled() const noexcept {return bc5Normals_;}
    std::size_t BakedTextureCount() const noexcept {return bakeCache_.Count();}
    std::uint64_t BakedTextureBytes() const noexcept {return bakeCache_.Bytes();}
    std::uint64_t CompressedUploads() const noexcept {return compressedUploads_;}
    // G04.06：压缩纹理驻留。预算按最久未用逐级降级并钳制 SRV；物理 mip 仍在（流送后续实现）。
    void SetTextureBudget(std::uint64_t bytes){residency_.SetBudget(bytes);}
    std::uint64_t TextureBudget() const noexcept {return residency_.Budget();}
    struct ResidencyUpdate {std::size_t degraded=0;std::uint64_t uploadedBytes=0,loadedBytes=0,budgetBytes=0;};
    // uploadBudgetBytes：单次更新最多补传的字节数（默认 24 MiB），剩余留到后续调用。
    ResidencyUpdate UpdateTextureResidency(rhi::Device& device,std::uint64_t uploadBudgetBytes=24ull*1024*1024);
    const TextureResidency& Residency() const noexcept {return residency_;}
private:
    enum class TextureRole { Color, Normal, Data };
    struct TextureEntry
    {
        std::weak_ptr<const rhi::TextureHandle> handle;
        std::uint32_t mipLevels=1;
        std::uint64_t residencyId=0;
        std::uint32_t appliedMip=0;  // 当前 SRV 的最细 mip
        std::uint32_t uploadedMip=0; // 物理已上传的最细 mip（缺 mip 时大于 applied 的下限）
        BakedTextureFormat format=BakedTextureFormat::Bc1Unorm;
        std::uint32_t width=0,height=0;
    };
    static std::uint64_t TextureResidencyId(std::string_view key);
    bool UploadTexture(const ImageData& image,std::string_view name,rhi::Device& device,
        rhi::Texture& texture,std::vector<AssetDiagnostic>& diagnostics,std::string_view preparedKey={},
        TextureRole role=TextureRole::Color);
    std::string TextureCacheKey(std::string_view contentKey,TextureRole role) const;
    std::optional<BakedTextureFormat> BakedFormatFor(TextureRole role) const;
    struct MaterialRevision
    {
        std::shared_ptr<const MaterialDocument> source;
        std::array<std::shared_ptr<const ImageData>,3> images;
        std::shared_ptr<const PbrRenderMaterial> gpu;
        std::string previewKey;
        std::string shaderKey; // 用户产物内容 SHA-256；空表示默认 PBR。
    };
    bool pbr_=false;
    bool bc1Textures_=false;
    bool bc1DataTextures_=false;
    bool bc5Normals_=false;
    std::uint64_t compressedUploads_=0;
    TextureBakeCache bakeCache_;
    TextureResidency residency_{512ull*1024*1024};
    std::uint64_t residencyFrame_=0;
    std::size_t pendingDegradations_=0;
    std::unordered_map<std::string,MaterialRevision> materials_;
    std::unordered_map<std::string,TextureEntry> textures_;
    std::unordered_map<std::string,std::shared_ptr<const StaticRenderModel>> models_;
    std::unordered_map<std::string,std::shared_ptr<const StaticModel>> attempted_;
};
void AppendStaticModelDraws(const StaticRenderModel& model,const Mat4& entityWorld,
    std::vector<rhi::DrawIndexed>& output,rhi::PipelineHandle pipeline,const Mat4& viewProjection,bool unlit=false,
    std::uint32_t lodLevel=0);
void AppendPbrModelDraws(const StaticRenderModel& model,const Mat4& entityWorld,
    std::span<const std::shared_ptr<const PbrRenderMaterial>> overrides,std::vector<rhi::DrawIndexed>& output,
    rhi::PipelineHandle pipeline,const Mat4& viewProjection,Vec3 camera,Vec3 directionToLight,Vec3 radiance,
    std::uint32_t lodLevel=0,float lodDither=0,const Mat4* previousWorld=nullptr);
void AppendShadowModelDraws(const StaticRenderModel& model,const Mat4& entityWorld,std::vector<rhi::DrawIndexed>& output,
    std::uint32_t lodLevel=0);
}
