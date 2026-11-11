#pragma once
#include "CoreLayer/Math/Math.h"
#include <string>
#include <vector>

namespace se
{
// 兼容的属性/快照值描述；实际运行时存储由对应的 ComponentBase 包装拥有。
struct CameraComponent {float verticalFovRadians=Pi/3;float nearPlane=.1f;float farPlane=1000;bool primary=false;};
struct MeshComponent {std::string meshAsset;std::vector<std::string> materials;bool castsShadow=true,receivesShadow=true;};
enum class LightType {Directional,Point};
struct LightComponent {Vec3 color{1,1,1};float intensity=1;LightType type=LightType::Directional;float range=10;};
}
