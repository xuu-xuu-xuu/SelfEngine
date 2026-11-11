#pragma once

#include "CoreLayer/Math/Math.h"
#include <cstdint>
#include <string>

namespace se
{
class World;
// 默认场景的稳定实体 ID。创建成功时必须全部存在。
enum class DefaultSceneEntity:std::uint64_t { Camera=1,Light=2,Ground=3,Cube=4 };
// 方向光本地 +Z 指向太阳；输入是归一化前的方向，输出单位四元数。
Quat DirectionToRotation(Vec3 directionToLight);
// R05 实现；失败返回稳定错误键且不写任何用户文件。
bool CreateDefaultScene(World& world,std::string& errorKey);
}
