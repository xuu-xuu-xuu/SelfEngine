#include "FunctionLayer/Runtime/Camera.h"
#include <algorithm>
#include <limits>

namespace se
{
namespace
{
bool ValidViewport(CameraViewport viewport)
{
    return IsFinite(viewport.x) && IsFinite(viewport.y) && IsFinite(viewport.width) && IsFinite(viewport.height)
        && viewport.width > 0 && viewport.height > 0 && IsFinite(viewport.x + viewport.width)
        && IsFinite(viewport.y + viewport.height);
}
std::optional<Vec3> Divide(Vec4 value)
{
    if (!IsFinite(value) || value.w == 0) return std::nullopt;
    const Vec3 result{ value.x/value.w, value.y/value.w, value.z/value.w };
    return IsFinite(result) ? std::optional<Vec3>{result} : std::nullopt;
}
}

std::string_view CameraErrorKey(CameraError error)
{
    switch (error)
    {
    case CameraError::None: return {};
    case CameraError::InvalidViewport: return "Camera.InvalidViewport";
    case CameraError::InvalidTransform: return "Camera.InvalidTransform";
    case CameraError::InvalidProjection: return "Camera.InvalidProjection";
    case CameraError::InvalidIdentity: return "Camera.InvalidIdentity";
    case CameraError::NoPrimary: return "Camera.NoPrimary";
    case CameraError::BindingMissing: return "Camera.BindingMissing";
    }
    return "Camera.InvalidProjection";
}

bool BuildCameraSnapshot(const CameraInstance& camera, CameraViewport viewport, std::uint64_t frameIndex,
    CameraSnapshot& output, CameraError& error)
{
    error = CameraError::None;
    if (!ValidViewport(viewport)) { error = CameraError::InvalidViewport; return false; }
    if (!camera.entity) { error = CameraError::InvalidIdentity; return false; }
    const auto view = InverseRigid(camera.world);
    if (!camera.rigidHierarchy || !view) { error = CameraError::InvalidTransform; return false; }
    const auto projection = PerspectiveRH(camera.camera.verticalFovRadians, viewport.width / viewport.height,
        camera.camera.nearPlane, camera.camera.farPlane);
    const auto inverse = projection ? Inverse(*projection) : std::nullopt;
    if (!inverse) { error = CameraError::InvalidProjection; return false; }
    CameraSnapshot next;
    next.entity = camera.entity;
    next.frameIndex = frameIndex;
    next.view = *view;
    next.projection = *projection;
    next.viewProjection = *projection * *view;
    next.inverseView = camera.world;
    next.inverseProjection = *inverse;
    next.inverseViewProjection = camera.world * *inverse;
    if (!IsFinite(next.viewProjection) || !IsFinite(next.inverseViewProjection))
    { error = CameraError::InvalidProjection; return false; }
    next.position = { camera.world(0,3), camera.world(1,3), camera.world(2,3) };
    next.lens = camera.camera;
    next.viewport = viewport;
    output = next;
    return true;
}

CameraSelection SelectCamera(std::span<const CameraInstance> cameras, CameraViewport viewport,
    std::uint64_t frameIndex, std::optional<StableEntityId> binding)
{
    CameraSelection result;
    if (!ValidViewport(viewport)) { result.error = CameraError::InvalidViewport; return result; }
    std::vector<const CameraInstance*> ordered;
    ordered.reserve(cameras.size());
    for (const auto& camera : cameras)
    {
        if (!camera.entity) { result.error = CameraError::InvalidIdentity; return result; }
        ordered.push_back(&camera);
    }
    std::sort(ordered.begin(), ordered.end(), [](const auto* a, const auto* b) { return a->entity < b->entity; });
    for (std::size_t index = 1; index < ordered.size(); ++index)
        if (ordered[index-1]->entity == ordered[index]->entity)
        { result.error = CameraError::InvalidIdentity; return result; }
    if (binding)
    {
        const auto found = std::find_if(ordered.begin(), ordered.end(), [&](const auto* camera) { return camera->entity == *binding; });
        if (found == ordered.end()) { result.error = CameraError::BindingMissing; return result; }
        CameraSnapshot snapshot;
        if (BuildCameraSnapshot(**found, viewport, frameIndex, snapshot, result.error)) result.snapshot = snapshot;
        return result;
    }
    CameraError firstError = CameraError::NoPrimary;
    std::size_t validCount = 0;
    for (const auto* camera : ordered)
    {
        if (!camera->camera.primary) continue;
        CameraSnapshot snapshot;
        CameraError error;
        if (!BuildCameraSnapshot(*camera, viewport, frameIndex, snapshot, error))
        {
            if (result.rejectedPrimaryCount++ == 0) firstError = error;
            continue;
        }
        if (++validCount == 1) result.snapshot = snapshot;
    }
    result.multiplePrimary = validCount > 1;
    if (!result.snapshot) result.error = firstError;
    return result;
}

std::optional<Vec3> ProjectWorldPoint(const CameraSnapshot& camera, Vec3 worldPoint)
{
    if (!ValidViewport(camera.viewport) || !IsFinite(worldPoint) || !IsFinite(camera.viewProjection)) return std::nullopt;
    const Vec4 clip = camera.viewProjection * Vec4{ worldPoint.x, worldPoint.y, worldPoint.z, 1 };
    if (clip.w <= 0) return std::nullopt;
    const auto ndc = Divide(clip);
    if (!ndc) return std::nullopt;
    const Vec3 pixel{ camera.viewport.x + (ndc->x + 1) * camera.viewport.width / 2,
        camera.viewport.y + (1 - ndc->y) * camera.viewport.height / 2, ndc->z };
    return IsFinite(pixel) ? std::optional<Vec3>{pixel} : std::nullopt;
}

std::optional<CameraRay> ViewportPointToRay(const CameraSnapshot& camera,Vec2 pixel,bool allowOutsideViewport)
{
    const auto viewport = camera.viewport;
    if (!ValidViewport(viewport) || !IsFinite(pixel.x) || !IsFinite(pixel.y) || !IsFinite(camera.inverseViewProjection)
        || (!allowOutsideViewport&&(pixel.x < viewport.x || pixel.x > viewport.x + viewport.width
        || pixel.y < viewport.y || pixel.y > viewport.y + viewport.height))) return std::nullopt;
    const float x = 2 * ((pixel.x - viewport.x) / viewport.width) - 1;
    const float y = 1 - 2 * ((pixel.y - viewport.y) / viewport.height);
    const auto origin = Divide(camera.inverseViewProjection * Vec4{ x, y, 0, 1 });
    // 用内部深度点求方向，避免很远的 far 在 float 投影中量化成无穷远。
    const auto inside = Divide(camera.inverseViewProjection * Vec4{ x, y, 0.5f, 1 });
    if (!origin || !inside) return std::nullopt;
    const Vec3 difference = *inside - *origin;
    const float length = Length(difference);
    if (!IsFinite(length) || length <= 0) return std::nullopt;
    return CameraRay{ *origin, difference / length };
}

Transform FreeCameraTransform(const FreeCameraState& state)
{
    return { state.position, AxisAngle({0,1,0},state.yaw) * AxisAngle({1,0,0},state.pitch), {1,1,1} };
}

bool UpdateFreeCamera(FreeCameraState& state, const FreeCameraInput& input, double deltaSeconds,
    const FreeCameraSettings& settings)
{
    if (!IsFinite(state.position) || !IsFinite(state.yaw) || !IsFinite(state.pitch)
        || std::abs(state.pitch) > FreeCameraPitchLimit || !IsFinite(input.move)
        || !IsFinite(input.lookPixels.x) || !IsFinite(input.lookPixels.y)
        || !std::isfinite(deltaSeconds) || deltaSeconds < 0 || !IsFinite(settings.speed) || settings.speed <= 0
        || !IsFinite(settings.fastMultiplier) || settings.fastMultiplier < 1
        || !IsFinite(settings.radiansPerPixel) || settings.radiansPerPixel <= 0) return false;
    FreeCameraState next = state;
    next.yaw = static_cast<float>(std::remainder(static_cast<double>(state.yaw)
        - static_cast<double>(input.lookPixels.x) * settings.radiansPerPixel, 2.0 * Pi));
    next.pitch = static_cast<float>(std::clamp(static_cast<double>(state.pitch)
        - static_cast<double>(input.lookPixels.y) * settings.radiansPerPixel,
        -static_cast<double>(FreeCameraPitchLimit), static_cast<double>(FreeCameraPitchLimit)));
    const Vec3 move{ std::clamp(input.move.x,-1.0f,1.0f), std::clamp(input.move.y,-1.0f,1.0f),
        std::clamp(input.move.z,-1.0f,1.0f) };
    Vec3 direction = Rotate(FreeCameraTransform(next).rotation, { move.x, 0, -move.z }) + Vec3{0,move.y,0};
    const float length = Length(direction);
    if (length > 1) direction = direction / length;
    const double distance = static_cast<double>(settings.speed) * (input.fast ? settings.fastMultiplier : 1) * deltaSeconds;
    if (!std::isfinite(distance) || distance > std::numeric_limits<float>::max()) return false;
    next.position += direction * static_cast<float>(distance);
    if (!IsFinite(next.position)) return false;
    state = next;
    return true;
}
}
