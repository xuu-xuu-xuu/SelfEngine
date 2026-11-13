#include "EditorGizmo.h"
#include "CoreLayer/Math/TransformDecomposition.h"
#include <algorithm>
#include <cmath>
#include <limits>

namespace se
{
namespace
{
Vec3 Axis(GizmoAxis axis)
{ return axis == GizmoAxis::X ? Vec3{1,0,0} : axis == GizmoAxis::Y ? Vec3{0,1,0} : Vec3{0,0,1}; }
std::optional<double> AxisParameter(const CameraSnapshot& camera,Vec2 mouse,Vec3 origin,Vec3 axis)
{
    const auto ray=ViewportPointToRay(camera,mouse,true);if(!ray)return {};
    const auto dot=[](Vec3 a,Vec3 b){return double(a.x)*b.x+double(a.y)*b.y+double(a.z)*b.z;};
    const auto offset=origin-ray->origin;const auto a=dot(axis,axis),b=dot(axis,ray->direction),c=dot(ray->direction,ray->direction);
    const auto denominator=a*c-b*b;if(!std::isfinite(denominator)||denominator<1e-5)return {};
    const auto result=(b*dot(ray->direction,offset)-c*dot(axis,offset))/denominator;
    if(!std::isfinite(result)||std::abs(result)>(std::numeric_limits<float>::max)())return {};return result;
}
}
Vec3 EditorGizmoAxis(const World& world,EntityHandle entity,GizmoAxis axis,GizmoSpace space)
{
    if(!world.IsAlive(entity)||static_cast<unsigned>(axis)>2||static_cast<unsigned>(space)>1)return {};
    if(space==GizmoSpace::World)return Axis(axis);
    Quat orientation;
    for(auto cursor=entity;cursor;cursor=world.Get(cursor)->parent)orientation=Normalize(world.GetTransform(cursor)->rotation*orientation);
    return Normalize(Rotate(orientation,Axis(axis)));
}
bool EditorGizmo::Begin(EditorSession& editor, GizmoMode mode, GizmoAxis axis,
    Vec2 mouse,Vec2 screenDirection,float pixelsPerUnit,GizmoSpace space,const CameraSnapshot* camera,const Vec3* pivot)
{
    if (active_ || editor.IsPlaying() || !std::isfinite(pixelsPerUnit) || pixelsPerUnit <= 0
        || !std::isfinite(mouse.x) || !std::isfinite(mouse.y)
        || static_cast<unsigned>(mode)>2 || static_cast<unsigned>(axis)>2||static_cast<unsigned>(space)>1)return Fail("Editor.InvalidGizmo");
    const float length = std::hypot(screenDirection.x,screenDirection.y);
    if (!std::isfinite(length) || length < 0.001f)return Fail("Editor.InvalidGizmo");
    const auto& world = editor.EditWorld();
    const auto entity = world.FindByStableId(editor.Selected());
    const auto* transform = world.GetTransform(entity);
    if (!transform)return Fail("Editor.InvalidSelection");
    const auto* record = world.Get(entity);
    const auto inverse = record->parent ? Inverse(world.GetWorldMatrix(record->parent)) : std::optional<Mat4>(Mat4{});
    if(!inverse)return Fail("Editor.TransformNotRepresentable");
    const auto direction=EditorGizmoAxis(world,entity,axis,space);const auto initialWorld=world.GetWorldMatrix(entity);
    const auto origin=pivot?*pivot:TransformPoint(initialWorld,{});
    if(!IsFinite(origin))return Fail("Editor.InvalidGizmo");
    if(Length(origin-TransformPoint(initialWorld,{}))>.00001f&&!Inverse(initialWorld))return Fail("Editor.TransformNotRepresentable");
    for(const auto value:initialWorld.values)if(!IsFinite(value))return Fail("Editor.TransformNotRepresentable");
    std::optional<double> parameter;
    if(camera&&mode==GizmoMode::Translate){parameter=AxisParameter(*camera,mouse,origin,direction);if(!parameter)return Fail("Editor.InvalidGizmo");}
    if(!editor.BeginEdit("Editor.Transform")){error_=editor.LastError();return false;}
    initial_ = *transform; parentInverse_ = *inverse; entity_ = record->stableId;
    mode_=mode; axis_=axis; mouse_=mouse;space_=space;axisDirection_=direction;initialWorld_=initialWorld;pivot_=origin;
    camera_=camera?std::optional<CameraSnapshot>(*camera):std::nullopt;initialAxisParameter_=parameter.value_or(0);
    direction_={screenDirection.x/length,screenDirection.y/length}; pixelsPerUnit_=pixelsPerUnit;
    active_=true;error_.clear();return true;
}
bool EditorGizmo::ProjectionMatches(const std::optional<CameraSnapshot>& camera) const
{
    if(!camera_)return true;if(!camera)return false;
    const auto a=camera_->viewport,b=camera->viewport;
    return a.x==b.x&&a.y==b.y&&a.width==b.width&&a.height==b.height&&camera_->viewProjection.values==camera->viewProjection.values;
}
bool EditorGizmo::Update(EditorSession& editor, Vec2 mouse)
{
    if(!active_||!std::isfinite(mouse.x)||!std::isfinite(mouse.y))return Fail("Editor.InvalidGizmo");
    float amount=((mouse.x-mouse_.x)*direction_.x+(mouse.y-mouse_.y)*direction_.y)/pixelsPerUnit_;
    if(mode_==GizmoMode::Translate&&camera_)
    {
        const auto parameter=AxisParameter(*camera_,mouse,pivot_,axisDirection_);
        if(!parameter)return Fail("Editor.InvalidGizmo");amount=static_cast<float>(*parameter-initialAxisParameter_);
    }
    if(!IsFinite(amount))return Fail("Editor.InvalidGizmo");
    Transform value=initial_;
    if(mode_==GizmoMode::Translate)value.position+=TransformVector(parentInverse_,axisDirection_*amount);
    else if(space_==GizmoSpace::Local&&mode_==GizmoMode::Rotate)
    {
        value.rotation=Normalize(initial_.rotation*AxisAngle(Axis(axis_),amount));
        const auto origin=TransformPoint(initialWorld_,{});
        value.position=TransformPoint(parentInverse_,pivot_+Rotate(AxisAngle(axisDirection_,amount),origin-pivot_));
    }
    else if(space_==GizmoSpace::Local)
    {
        const float factor=std::max(0.001f,1.0f+amount);
        if(axis_==GizmoAxis::X)value.scale.x*=factor;
        if(axis_==GizmoAxis::Y)value.scale.y*=factor;
        if(axis_==GizmoAxis::Z)value.scale.z*=factor;
        const auto inverseWorld=Inverse(initialWorld_);
        const auto localPivot=inverseWorld?TransformPoint(*inverseWorld,pivot_):Vec3{};
        value.position=initial_.position+TransformVector(initial_.Matrix(),localPivot-TransformVector(Scaling(
            axis_==GizmoAxis::X?Vec3{factor,1,1}:axis_==GizmoAxis::Y?Vec3{1,factor,1}:Vec3{1,1,factor}),localPivot));
    }
    else
    {
        Vec3 factors{1,1,1};const float factor=std::max(.001f,1+amount);
        if(axis_==GizmoAxis::X)factors.x=factor;if(axis_==GizmoAxis::Y)factors.y=factor;if(axis_==GizmoAxis::Z)factors.z=factor;
        const auto operation=mode_==GizmoMode::Rotate?Rotation(AxisAngle(Axis(axis_),amount)):Scaling(factors);
        auto desired=Translation(pivot_)*operation*Translation(-pivot_)*initialWorld_;
        if(!TryDecomposeTrs(parentInverse_*desired,value))return Fail("Editor.TransformNotRepresentable");
    }
    if(!editor.SetTransform(entity_,value)){error_=editor.LastError();return false;}error_.clear();return true;
}
bool EditorGizmo::End(EditorSession& editor, bool commit)
{
    if(!active_)return false;
    const bool result=commit?editor.CommitEdit():editor.CancelEdit();
    if(result)active_=false; return result;
}
}
