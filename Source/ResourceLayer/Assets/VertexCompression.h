#pragma once
#include "CoreLayer/Math/Bounds.h"
#include "CoreLayer/Math/Projection.h"
#include "StaticModel.h"
#include <array>
#include <cstdint>
#include <span>
#include <string>
#include <vector>

namespace se
{
// G03：16 字节压缩顶点。位置按网格包围盒量化，法线八面体 8 位，
// UV 半精度，切线 4×8 位。仅 CPU 烘焙格式；GPU 布局由渲染层转换。
struct PackedVertex
{
    std::uint16_t position[3]{};
    std::uint8_t normal[2]{};
    std::uint16_t uv[2]{};
    std::uint32_t tangent=0;
};
static_assert(sizeof(PackedVertex)==16,"G03 约定压缩顶点为 16 字节");

struct MeshQuantization
{
    std::array<float,3> minimum{};
    std::array<float,3> extent{1,1,1};
};

// 位置量化以调用方给出的网格包围盒为基准；失败时不修改输出。
bool CompressMeshVertices(std::span<const MeshVertex> vertices,const Aabb& bounds,
    std::vector<PackedVertex>& output,MeshQuantization& quantization,std::string& error);

Vec3 DecodePackedNormal(const PackedVertex& vertex);
Vec3 DecodePackedPosition(const PackedVertex& vertex,const MeshQuantization& quantization);
void DecodePackedUv(const PackedVertex& vertex,float& u,float& v);
}
