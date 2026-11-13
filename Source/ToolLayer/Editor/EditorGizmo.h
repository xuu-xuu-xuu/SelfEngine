#pragma once
#include "EditorSession.h"
#include "FunctionLayer/Runtime/Camera.h"

namespace se
{
enum class GizmoMode { Translate, Rotate, Scale };
enum class GizmoAxis { X, Y, Z };
enum class GizmoSpace { World,Local };
Vec3 EditorGizmoAxis(const World& world,EntityHandle entity,GizmoAxis axis,GizmoSpace space);

// 可选空间；有相机时平移用射线与轴最近点，捕获后允许出面板，投影变化取消。
// 无相机的 CPU 调用保留投影单位标定；旋转/缩放使用像素到弧度/比例的灵敏度。
class EditorGizmo
{
public:
    bool Begin(EditorSession& editor, GizmoMode mode, GizmoAxis axis, Vec2 mouse,
        Vec2 screenDirection,float pixelsPerUnit=80,GizmoSpace space=GizmoSpace::World,const CameraSnapshot* camera=nullptr,
        const Vec3* pivot=nullptr);
    bool Update(EditorSession& editor, Vec2 mouse);
    bool End(EditorSession& editor, bool commit = true);
    bool Active() const noexcept { return active_; }
    bool ProjectionMatches(const std::optional<CameraSnapshot>& camera) const;
    const std::string& LastError() const noexcept{return error_;}
private:
    bool Fail(const char* key){error_=key;return false;}
    bool active_ = false;
    StableEntityId entity_ = 0;
    GizmoMode mode_{}; GizmoAxis axis_{};
    GizmoSpace space_=GizmoSpace::World;
    Transform initial_;
    Mat4 parentInverse_,initialWorld_;
    Vec3 axisDirection_;
    Vec3 pivot_;
    std::optional<CameraSnapshot> camera_;
    double initialAxisParameter_=0;
    std::string error_;
    Vec2 mouse_{}, direction_{};
    float pixelsPerUnit_ = 80;
};

}
