#pragma once
#include "AssetTasks.h"
#include "CoreLayer/Math/Projection.h"

namespace se
{
struct StaticModelPart
{
    std::uint32_t mesh=0;
    Mat4 modelFromMesh;
    Vec4 baseColor{1,1,1,1};
};
struct StaticModelLayout
{
    std::vector<StaticModelPart> parts;
    Vec3 minimum{},maximum{};
};
// 同步和后台共用，节点父子不要求数组顺序；成功后整体替换，不修改原顶点。
bool BuildStaticModelLayout(const StaticModel& model,StaticModelLayout& output,std::vector<AssetDiagnostic>& diagnostics);
enum class DefaultTextureKind { White, FlatNormal, Fallback };
const ImageData& DefaultTextureImage(DefaultTextureKind kind);
const std::string& DefaultTextureKey(DefaultTextureKind kind);
std::string ImageContentKey(const ImageData& image);
struct ModelMaterialImages
{
    const MaterialData* material=nullptr;
    std::array<const ImageData*,3> images{};
    std::array<std::string,3> keys;
    std::array<bool,3> fallback{};
    bool normalMapping=false;
};
bool SelectModelMaterialImages(const StaticModel& model,const PreparedModelImages& images,
    std::size_t mesh,ModelMaterialImages& output);
}
