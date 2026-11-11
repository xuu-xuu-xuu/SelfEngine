#pragma once
#include "PlatformLayer/Graphics/Rhi.h"
#include <array>
#include <cstdint>
#include <span>
#include <vector>

namespace se
{
// G09 原型：Forward+ 固定 16×9×24 集群。指数 Z 切分与 XY 视口划分在 CPU/GPU 共用同一公式；
// 先计数、再前缀和、最后填索引，容量不足时整体回退到全局灯列表（overflow）。
inline constexpr std::uint32_t LightClusterX=16,LightClusterY=9,LightClusterZ=24;
inline constexpr std::uint32_t LightClusterCount=LightClusterX*LightClusterY*LightClusterZ;
inline constexpr std::uint32_t LightClusterCapacity=65536;
struct LightClusterRange
{
    std::uint32_t offset=0,count=0;
};
struct LightClusterData
{
    std::vector<LightClusterRange> clusters; // 16*9*24，行优先 x + y*16 + z*16*9。
    std::vector<std::uint32_t> indices;
    bool overflow=false;
    std::uint32_t maxPerCluster=0;
};
struct LightClusterProjection
{
    Mat4 view,clipFromView; // world→view 与 view→clip。
};
// 失败不替换 output；capacity==0 使用默认 65536。
bool BuildLightClusters(std::span<const rhi::PointLight> lights,const LightClusterProjection& matrices,
    float nearPlane,float farPlane,LightClusterData& output,std::uint32_t capacity=LightClusterCapacity);
// 视空间深度 → Z 切片；非法输入返回 false。
bool LightClusterZSlice(float viewDepth,float nearPlane,float farPlane,std::uint32_t& slice);
// NDC → 集群索引；越界返回 false。
bool LightClusterIndex(float ndcX,float ndcY,float viewDepth,float nearPlane,float farPlane,std::uint32_t& index);
// 容量溢出的回退：把所有灯放入每个集群（全局灯列表）。
void FillLightClustersGlobal(std::uint32_t lightCount,LightClusterData& output);
}
