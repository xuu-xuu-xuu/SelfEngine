#pragma once
#include "StaticModel.h"
#include "CoreLayer/Math/Projection.h"
#include <array>
#include <cstdint>
#include <filesystem>
#include <string>
#include <vector>
#include <stop_token>

namespace se
{
// G05.09：HLOD 64 m 单元。实例按世界包围盒中心落入 floor(x/cell)、floor(z/cell) 的唯一单元，
// 单元内再按材质分组，烘焙成世界空间的合并网格（每个实例只进一个单元、一个材质组）。
inline constexpr float HlodCellSize=64.0f;

struct HlodInstance
{
    const MeshData* mesh=nullptr; // 借用顶点缓冲；烘焙期间只读。
    const std::vector<std::uint32_t>* indices=nullptr; // 空表示 mesh->indices；否则为所选 LOD 级索引。
    Mat4 world;
    float worldError=0;       // 该实例相对原网格的误差，写入单元误差上限。
    std::string material;     // 材质分组键（可为空）。
};
struct HlodCellCoordinate
{
    std::int32_t x=0,z=0;
    bool operator==(const HlodCellCoordinate&)const=default;
};
struct HlodPart
{
    std::string material;
    MeshData mesh; // 世界空间顶点/索引；不再套实体变换。
};
struct HlodCell
{
    HlodCellCoordinate coordinate;
    Vec3 center{};     // 合并后包围盒中心。
    float radius=0;    // 合并后包围球半径（选择公式使用）。
    float worldError=0;
    std::vector<HlodPart> parts;
};
struct HlodScene
{
    float cellSize=HlodCellSize;
    std::uint32_t instanceCount=0;
    std::vector<HlodCell> cells;
};

// 世界包围盒中心所在单元；负数按 floor 向下取整，单元边界只归属一侧。
HlodCellCoordinate HlodCellFor(const Vec3& worldCenter,float cellSize);
// 把网格顶点/法线/切线变换到世界空间；失败（退化矩阵）时使用单位旋转。
void TransformMeshToWorld(const MeshData& source,const Mat4& world,MeshData& output);
// 合并实例并按 64 m（或指定边长）单元分组。assignments 非空时按输入顺序回填每个实例的单元。
bool BakeHlod(const std::vector<HlodInstance>& instances,float cellSize,HlodScene& output,
    std::vector<AssetDiagnostic>& diagnostics,std::vector<HlodCellCoordinate>* assignments=nullptr);
// .sehlod：magic/版本 + 单元坐标/包围球/误差 + 材质与世界空间顶点/索引；读取时完整校验。
bool WriteHlodScene(const std::filesystem::path& path,const HlodScene& scene,std::string& error);
bool ReadHlodScene(const std::filesystem::path& path,HlodScene& output,std::string& error,std::stop_token stop={});
}
