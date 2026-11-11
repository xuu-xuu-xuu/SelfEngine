#pragma once

#include "CoreLayer/Math/Math.h"
#include <algorithm>

namespace se
{
inline constexpr float CameraRigidTolerance = 1e-4f;

inline bool IsFinite(Vec4 value)
{
    return IsFinite(value.x) && IsFinite(value.y) && IsFinite(value.z) && IsFinite(value.w);
}

inline bool IsFinite(const Mat4& matrix)
{
    return std::all_of(matrix.values.begin(), matrix.values.end(), [](float value) { return IsFinite(value); });
}

inline bool HasUnitScale(Vec3 scale)
{
    return IsFinite(scale) && std::abs(scale.x - 1) <= CameraRigidTolerance
        && std::abs(scale.y - 1) <= CameraRigidTolerance && std::abs(scale.z - 1) <= CameraRigidTolerance;
}

// 右手、列向量，相机看 -Z；普通 D3D 深度 near=0、far=1。
inline std::optional<Mat4> PerspectiveRH(float verticalFov, float aspect, float nearPlane, float farPlane)
{
    if (!IsFinite(verticalFov) || !IsFinite(aspect) || !IsFinite(nearPlane) || !IsFinite(farPlane)
        || verticalFov <= 0 || verticalFov >= Pi || aspect <= 0 || nearPlane <= 0 || farPlane <= nearPlane)
        return std::nullopt;
    const double y = 1.0 / std::tan(static_cast<double>(verticalFov) / 2);
    const double denominator = static_cast<double>(nearPlane) - farPlane;
    Mat4 result;
    result.values.fill(0);
    result(0,0) = static_cast<float>(y / aspect);
    result(1,1) = static_cast<float>(y);
    result(2,2) = static_cast<float>(farPlane / denominator);
    result(2,3) = static_cast<float>(static_cast<double>(nearPlane) * farPlane / denominator);
    result(3,2) = -1;
    if (!IsFinite(result) || result(0,0) <= 0 || result(1,1) <= 0 || result(2,3) == 0)
        return std::nullopt;
    return result;
}

inline std::optional<Mat4> OrthographicRH(float width, float height, float nearPlane, float farPlane)
{
    if (!IsFinite(width) || !IsFinite(height) || !IsFinite(nearPlane) || !IsFinite(farPlane)
        || width <= 0 || height <= 0 || nearPlane <= 0 || farPlane <= nearPlane) return std::nullopt;
    const double denominator = static_cast<double>(nearPlane) - farPlane;
    Mat4 result;
    result(0,0) = static_cast<float>(2.0 / width);
    result(1,1) = static_cast<float>(2.0 / height);
    result(2,2) = static_cast<float>(1.0 / denominator);
    result(2,3) = static_cast<float>(nearPlane / denominator);
    if (!IsFinite(result) || result(0,0) <= 0 || result(1,1) <= 0 || result(2,2) == 0)
        return std::nullopt;
    return result;
}

// 验证后使用刚性逆，不对缩放/剪切做静默正交化。
inline std::optional<Mat4> InverseRigid(const Mat4& world)
{
    if (!IsFinite(world) || std::abs(world(3,0)) > CameraRigidTolerance
        || std::abs(world(3,1)) > CameraRigidTolerance || std::abs(world(3,2)) > CameraRigidTolerance
        || std::abs(world(3,3) - 1) > CameraRigidTolerance) return std::nullopt;
    const Vec3 x{ world(0,0), world(1,0), world(2,0) };
    const Vec3 y{ world(0,1), world(1,1), world(2,1) };
    const Vec3 z{ world(0,2), world(1,2), world(2,2) };
    if (std::abs(Dot(x,x) - 1) > CameraRigidTolerance || std::abs(Dot(y,y) - 1) > CameraRigidTolerance
        || std::abs(Dot(z,z) - 1) > CameraRigidTolerance || std::abs(Dot(x,y)) > CameraRigidTolerance
        || std::abs(Dot(x,z)) > CameraRigidTolerance || std::abs(Dot(y,z)) > CameraRigidTolerance
        || std::abs(Dot(Cross(x,y),z) - 1) > CameraRigidTolerance) return std::nullopt;
    Mat4 result;
    for (std::size_t row = 0; row < 3; ++row)
        for (std::size_t column = 0; column < 3; ++column) result(row,column) = world(column,row);
    const Vec3 position{ world(0,3), world(1,3), world(2,3) };
    result(0,3) = -Dot(x, position);
    result(1,3) = -Dot(y, position);
    result(2,3) = -Dot(z, position);
    return IsFinite(result) ? std::optional<Mat4>{result} : std::nullopt;
}
}
