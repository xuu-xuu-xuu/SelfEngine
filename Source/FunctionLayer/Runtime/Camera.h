#pragma once

#include "FunctionLayer/Runtime/World.h"
#include "CoreLayer/Math/Projection.h"
#include <span>
#include <string_view>

namespace se
{
struct CameraInstance
{
    StableEntityId entity = 0;
    Mat4 world;
    CameraComponent camera;
    bool rigidHierarchy = true; // 提取时检查每个祖先，不能仅检查最终乘积。
};

struct CameraViewport { float x = 0, y = 0, width = 0, height = 0; };

struct CameraSnapshot
{
    StableEntityId entity = 0;
    std::uint64_t frameIndex = 0;
    Mat4 view, projection, viewProjection;
    Mat4 inverseView, inverseProjection, inverseViewProjection;
    Vec3 position;
    CameraComponent lens;
    CameraViewport viewport;
};

enum class CameraError { None, InvalidViewport, InvalidTransform, InvalidProjection, InvalidIdentity, NoPrimary, BindingMissing };
std::string_view CameraErrorKey(CameraError error);

bool BuildCameraSnapshot(const CameraInstance& camera, CameraViewport viewport, std::uint64_t frameIndex,
    CameraSnapshot& output, CameraError& error);

struct CameraSelection
{
    std::optional<CameraSnapshot> snapshot;
    CameraError error = CameraError::None;
    bool multiplePrimary = false;
    std::size_t rejectedPrimaryCount = 0;
};
CameraSelection SelectCamera(std::span<const CameraInstance> cameras, CameraViewport viewport,
    std::uint64_t frameIndex, std::optional<StableEntityId> binding = std::nullopt);

// 返回像素 x/y 和深度；投影视口外的点仍可成功，w<=0 或非有限值失败。
std::optional<Vec3> ProjectWorldPoint(const CameraSnapshot& camera, Vec3 worldPoint);
struct CameraRay { Vec3 origin, direction; };
// 拾取默认拒绝视口外坐标；已捕获的手柄拖动可显式允许越出面板。
std::optional<CameraRay> ViewportPointToRay(const CameraSnapshot& camera,Vec2 pixel,bool allowOutsideViewport=false);

struct FreeCameraState { Vec3 position; float yaw = 0, pitch = 0; };
struct FreeCameraInput { Vec3 move; Vec2 lookPixels; bool fast = false; };
struct FreeCameraSettings { float speed = 3, fastMultiplier = 4, radiansPerPixel = 0.003f; };
inline constexpr float FreeCameraPitchLimit = Pi / 2 - 0.01f;
Transform FreeCameraTransform(const FreeCameraState& state);
// 输入移动分量限制到 [-1,1]；失败不修改 state，鼠标位移不乘 dt。
bool UpdateFreeCamera(FreeCameraState& state, const FreeCameraInput& input, double deltaSeconds,
    const FreeCameraSettings& settings = {});
}
