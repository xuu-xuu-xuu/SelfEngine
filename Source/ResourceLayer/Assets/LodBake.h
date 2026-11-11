#pragma once
#include "StaticModel.h"
#include <filesystem>
#include <string>
#include <vector>

namespace se
{
// G05：离线 LOD 链。级别 0 为原始索引；更粗级别用顶点聚类简化，
// 并锁定开放边界（只被一个三角形使用的边）的顶点，避免裂开。
struct LodLevel
{
    std::vector<std::uint32_t> indices;
    float worldError=0; // 该级相对原网格的最大位置偏差（世界单位）
};
struct LodChain
{
    std::uint32_t mesh=0;
    std::uint32_t vertexCount=0;
    float radius=0; // 网格包围球半径（选择公式使用）
    std::uint32_t lockedVertices=0;
    std::vector<LodLevel> levels;
};
// levels>=2 且 <=8；失败不修改 output。索引必须有效且构成非退化三角形。
bool BakeLodChain(const StaticModel& model,std::uint32_t mesh,std::uint32_t levels,
    LodChain& output,std::vector<AssetDiagnostic>& diagnostics);
// .selod：magic/版本 + mesh/顶点数/半径 + 每级索引与误差；读取时做完整校验。
bool WriteLodChain(const std::filesystem::path& path,const LodChain& chain,std::string& error);
bool ReadLodChain(const std::filesystem::path& path,LodChain& output,std::string& error);
}
