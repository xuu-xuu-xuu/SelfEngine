#pragma once
#include "CoreLayer/Math/Bounds.h"
#include "CoreLayer/Math/Projection.h"
#include "ResourceLayer/Assets/StaticModel.h"
#include <cstdint>
#include <filesystem>
#include <span>
#include <string>
#include <vector>

namespace se
{
// G07（原型）：64 顶点 / 124 三角形的 cluster 烘焙。cluster 保存局部索引与
// 局部→源顶点映射，几何覆盖与源网格一致；法线锥用于背面剔除。
inline constexpr std::uint32_t MeshletMaxVertices=64,MeshletMaxTriangles=124;
struct MeshletCluster
{
    std::vector<std::uint32_t> vertices; // 局部索引 → 源顶点索引（≤64）。
    std::vector<std::uint32_t> indices;  // 每 3 个一组，局部索引（≤372）。
    Vec3 center{};
    float radius=0;
    Vec3 coneAxis{0,0,1};
    float coneCutoff=0;                  // 圆锥半角（弧度）；coneValid=false 时不用于剔除。
    bool coneValid=false;
};
struct MeshletSet
{
    std::uint32_t sourceVertexCount=0,triangleCount=0;
    std::vector<MeshletCluster> clusters;
};
bool BakeMeshlets(const MeshData& mesh,MeshletSet& output,std::vector<AssetDiagnostic>& diagnostics);
bool WriteMeshletSet(const std::filesystem::path& path,const MeshletSet& set,std::string& error);
bool ReadMeshletSet(const std::filesystem::path& path,MeshletSet& set,std::string& error);
struct MeshletView
{
    Vec3 position{};
    Frustum frustum;
    bool coneCulling=true;
};
struct MeshletCullResult
{
    std::vector<std::uint32_t> visible; // cluster 索引，输入顺序。
    std::uint32_t culled=0,coneCulled=0;
};
// 保守：球-视锥 + 法线锥背面。相机离 cluster 太近时跳过锥测试；
// 任何数值异常/退化锥都保留可见，宁可多画。
void CullMeshlets(const MeshletSet& set,const MeshletView& view,MeshletCullResult& output);
}
