#pragma once
#include "ResourceLayer/Assets/StaticModel.h"
#include <string_view>

namespace se
{
inline constexpr char BuiltinCubeAssetId[]="00000000000000000000000000000001";
inline constexpr char BuiltinSphereAssetId[]="00000000000000000000000000000002";
inline bool IsBuiltinMesh(std::string_view id){return id==BuiltinCubeAssetId||id==BuiltinSphereAssetId;}
// 边长 1 米，面独立顶点/UV；返回规范 CPU 数据，不持有 GPU 资源。
MeshData MakeUnitCube();
// 直径 1 米；UV 的 +V 朝南极，T 沿 +U，tangent.w=+1；无退化极点三角形。
MeshData MakeUnitSphere(std::uint32_t segments=48,std::uint32_t rings=24);
}
