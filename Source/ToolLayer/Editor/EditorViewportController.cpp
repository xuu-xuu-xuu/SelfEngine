#include "EditorViewportController.h"
#include <algorithm>
#include <cmath>
#include <limits>

namespace se
{
namespace
{
using ui::UiColor;
constexpr UiColor colors[]={{.95f,.25f,.25f,1},{.25f,.9f,.35f,1},{.3f,.6f,1,1}};
Vec2 Screen(const CameraSnapshot& c,Vec4 p)
{return {c.viewport.x+(p.x/p.w*.5f+.5f)*c.viewport.width,c.viewport.y+(.5f-p.y/p.w*.5f)*c.viewport.height};}
// 在齐次空间裁剪近面；视口 scissor 负责左右上下边界，避免跨越相机的网格爆炸。
bool Segment(const CameraSnapshot& c,Vec3 a,Vec3 b,Vec2& x,Vec2& y)
{
    auto p=c.viewProjection*Vec4{a.x,a.y,a.z,1},q=c.viewProjection*Vec4{b.x,b.y,b.z,1};
    for(int plane=0;plane<2;++plane)
    {
        const float u=plane?p.z:p.w-.0001f,v=plane?q.z:q.w-.0001f;
        if(u<0&&v<0)return false;
        if((u<0)!=(v<0))
        {
            const float t=u/(u-v);
            const Vec4 k{p.x+(q.x-p.x)*t,p.y+(q.y-p.y)*t,p.z+(q.z-p.z)*t,p.w+(q.w-p.w)*t};
            if(u<0)p=k;else q=k;
        }
    }
    x=Screen(c,p);y=Screen(c,q);
    return IsFinite(x.x)&&IsFinite(x.y)&&IsFinite(y.x)&&IsFinite(y.y);
}
float Distance(Vec2 p,Vec2 a,Vec2 b)
{
    const Vec2 d{b.x-a.x,b.y-a.y};const float l=d.x*d.x+d.y*d.y;
    const float t=l>0?std::clamp(((p.x-a.x)*d.x+(p.y-a.y)*d.y)/l,0.f,1.f):0;
    return std::hypot(p.x-a.x-t*d.x,p.y-a.y-t*d.y);
}
Vec3 Forward(const EditorViewportCamera& c){return Rotate(FreeCameraTransform(c.state).rotation,{0,0,-1});}
}

Vec3 EditorViewportController::Pivot(const World& w,AssetStore& assets,EntityHandle e)const
{
    if(center_)if(auto b=EditorEntityBounds(w,assets,e))return (b->minimum+b->maximum)*.5f;
    return TransformPoint(w.GetWorldMatrix(e),{});
}
std::optional<Aabb> EditorViewportController::SceneBounds(const World& w,AssetStore& assets)const
{
    std::optional<Aabb> result;
    for(auto e:w.Entities())if(w.GetMesh(e))if(auto b=EditorEntityBounds(w,assets,e))
    {
        if(!result)result=b;
        else
        {
            result->minimum={std::min(result->minimum.x,b->minimum.x),std::min(result->minimum.y,b->minimum.y),std::min(result->minimum.z,b->minimum.z)};
            result->maximum={std::max(result->maximum.x,b->maximum.x),std::max(result->maximum.y,b->maximum.y),std::max(result->maximum.z,b->maximum.z)};
        }
    }
    return result;
}
bool EditorViewportController::FrameSelection(EditorSession& s,AssetStore& assets,bool follow)
{
    if(s.IsPlaying()||Captured())return false;
    const auto e=s.EditWorld().FindByStableId(s.Selected());
    if(!e)return false;
    const auto b=EditorEntityBounds(s.EditWorld(),assets,e);if(!b)return false;
    pivot_=(b->minimum+b->maximum)*.5f;
    const float aspect=viewport_.height>0?viewport_.width/viewport_.height:1;
    // 默认镜头垂直 FOV 60 度，窄视口也必须容纳整个包围球。
    const float halfFov=std::atan(std::tan(Pi/6)*std::min(1.f,aspect));
    distance_=std::max(.6f,Length(b->maximum-b->minimum)*.55f/std::sin(halfFov));
    camera_.state.position=pivot_-Forward(camera_)*distance_;
    follow_=follow?s.Selected():0;followPosition_=pivot_;
    return true;
}
void EditorViewportController::Dolly(float amount)
{
    const float next=std::clamp(distance_*std::exp(amount),.05f,100000.f);
    camera_.state.position+=Forward(camera_)*(distance_-next);distance_=next;
    pivot_=camera_.state.position+Forward(camera_)*distance_;
}
void EditorViewportController::Cancel(EditorSession& s)
{
    if(gizmo_.Active())gizmo_.End(s,false);
    gesture_=Gesture::None;dragAxis_=-1;keys_.fill(false);
}
void EditorViewportController::Blur(EditorSession& s){Cancel(s);focused_=false;}
bool EditorViewportController::Action(std::string_view action,EditorSession& s,AssetStore& assets)
{
    if(s.IsPlaying())return false;
    if(Captured())Cancel(s);
    if(action=="Q")viewTool_=true;
    else if(action=="W"||action=="E"||action=="R")
    {viewTool_=false;mode_=action=="W"?GizmoMode::Translate:action=="E"?GizmoMode::Rotate:GizmoMode::Scale;}
    else if(action=="X")space_=Local()?GizmoSpace::World:GizmoSpace::Local;
    else if(action=="Z")center_=!center_;
    else if(action=="Sky")sky_=!sky_;
    else if(action=="Grid")grid_=!grid_;
    else if(action=="Gizmos")gizmos_=!gizmos_;
    else if(action=="Frame")return FrameSelection(s,assets);
    else if(action=="FrameAll")
    {
        auto b=SceneBounds(s.EditWorld(),assets);if(!b)return false;
        pivot_=(b->minimum+b->maximum)*.5f;distance_=std::max(1.f,Length(b->maximum-b->minimum)*1.25f);
        camera_.state.position=pivot_-Forward(camera_)*distance_;follow_=0;
    }
    else return false;
    return true;
}
void EditorViewportController::Update(double seconds,EditorSession& s,AssetStore& assets)
{
    if(s.IsPlaying()){Blur(s);return;}
    if(follow_)
    {
        const auto e=s.EditWorld().FindByStableId(follow_);
        if(!e||s.Selected()!=follow_)follow_=0;
        else
        {
            const auto b=EditorEntityBounds(s.EditWorld(),assets,e);
            if(b){const auto p=(b->minimum+b->maximum)*.5f;camera_.state.position+=p-followPosition_;pivot_+=p-followPosition_;followPosition_=p;}
        }
    }
    if(Flying())
    {
        const Vec3 move{float(keys_['D'])-float(keys_['A']),float(keys_['E'])-float(keys_['Q']),float(keys_['W'])-float(keys_['S'])};
        camera_.Update({move,{},keys_[16]},seconds,{speed_,4,.003f});
        pivot_=camera_.state.position+Forward(camera_)*distance_;
    }
}
bool EditorViewportController::Handle(ui::UiInputEvent& ev,EditorSession& s,AssetStore& assets,bool canStart)
{
    using enum ui::UiEventType;
    if(ev.type==FocusLost||ev.type==PointerCaptureLost){Blur(s);return false;}
    if(s.IsPlaying())return false;
    if(ev.type==KeyUp)
    {if(ev.key<keys_.size())keys_[ev.key]=false;return Captured();}
    if(ev.type==KeyDown)
    {
        const bool toolKey=ev.key=='Q'||ev.key=='W'||ev.key=='E'||ev.key=='R'||ev.key=='X'||ev.key=='Z';
        // 工具与聚焦选择也可从 Hierarchy 使用；文本/菜单屏蔽由宿主先处理。
        if(!focused_&&!toolKey&&ev.key!='F')return false;
        if(ev.key==27){Cancel(s);return true;}
        if(ev.key>=keys_.size())return false;
        const bool repeated=keys_[ev.key];keys_[ev.key]=true;
        if(Flying())return true;
        if(Captured()||ev.control||ev.alt||repeated)return false;
        if(ev.key=='F')return FrameSelection(s,assets,ev.shift);
        if(ev.key=='Q'||ev.key=='W'||ev.key=='E'||ev.key=='R'||ev.key=='X'||ev.key=='Z')
            return Action(std::string(1,static_cast<char>(ev.key)),s,assets);
        return false;
    }
    const Vec2 mouse{ev.x,ev.y};
    if(ev.type==PointerWheel&&(canStart||Flying()))
    {
        if(Flying())speed_=std::clamp(speed_*std::exp(ev.wheel*.2f),.05f,1000.f);
        else {follow_=0;Dolly(-ev.wheel*.12f);}
        return true;
    }
    if(ev.type==PointerDown&&canStart&&!Captured()&&snapshot_)
    {
        focused_=true;lastMouse_=mouse;dragButton_=ev.button;
        if(ev.alt&&ev.button==0)gesture_=Gesture::Orbit;
        else if(ev.alt&&ev.button==1)gesture_=Gesture::Zoom;
        else if(ev.button==2||(ev.button==0&&viewTool_))gesture_=Gesture::Pan;
        else if(ev.button==1)gesture_=Gesture::Fly;
        else if(ev.button==0)
        {
            float nearest=9;const HandleSegment* hit=nullptr;
            for(const auto& h:handles_)if(const auto d=Distance(mouse,h.a,h.b);d<nearest){nearest=d;hit=&h;}
            if(hit)
            {
                const auto e=s.EditWorld().FindByStableId(s.Selected());const auto origin=Pivot(s.EditWorld(),assets,e);
                const Vec2 direction{hit->b.x-hit->a.x,hit->b.y-hit->a.y};
                if(gizmo_.Begin(s,mode_,static_cast<GizmoAxis>(hit->axis),mouse,direction,80,space_,&*snapshot_,&origin))
                {gesture_=Gesture::Gizmo;dragAxis_=hit->axis;return true;}
            }
            auto picked=PickEditorBounds(s.EditWorld(),assets,*snapshot_,(ev.x-viewport_.x)/viewport_.width,(ev.y-viewport_.y)/viewport_.height);
            if(gizmos_)for(auto e:s.EditWorld().Entities())if(s.EditWorld().IsEnabled(e)&&(s.EditWorld().GetCamera(e)||s.EditWorld().GetLight(e)))
                if(auto p=ProjectWorldPoint(*snapshot_,TransformPoint(s.EditWorld().GetWorldMatrix(e),{}));p&&p->z>=0&&p->z<=1&&std::hypot(ev.x-p->x,ev.y-p->y)<10)
                    picked=s.EditWorld().Get(e)->stableId;
            s.Select(picked);return true;
        }
        follow_=0;keys_[16]=ev.shift;return true;
    }
    if(ev.type==PointerMove)
    {
        hoverAxis_=-1;
        if(canStart)for(const auto& h:handles_)if(Distance(mouse,h.a,h.b)<8)hoverAxis_=h.axis;
        if(!Captured())return false;
        const Vec2 delta{mouse.x-lastMouse_.x,mouse.y-lastMouse_.y};lastMouse_=mouse;
        if(gesture_==Gesture::Gizmo){gizmo_.Update(s,mouse);return true;}
        if(gesture_==Gesture::Fly||gesture_==Gesture::Orbit)
        {
            camera_.Update({{},delta,ev.shift},0);
            if(gesture_==Gesture::Orbit)camera_.state.position=pivot_-Forward(camera_)*distance_;
            else pivot_=camera_.state.position+Forward(camera_)*distance_;
        }
        else if(gesture_==Gesture::Pan)
        {
            const float units=2*distance_*std::tan(Pi/6)/std::max(1.f,viewport_.height);
            const auto d=Rotate(FreeCameraTransform(camera_.state).rotation,Vec3{-delta.x*units,delta.y*units,0});
            camera_.state.position+=d;pivot_+=d;
        }
        else if(gesture_==Gesture::Zoom)Dolly((delta.x-delta.y)*.01f);
        return true;
    }
    if(ev.type==PointerUp&&Captured()&&ev.button==dragButton_)
    {
        if(gizmo_.Active())gizmo_.End(s,true);
        gesture_=Gesture::None;dragAxis_=-1;keys_.fill(false);return true;
    }
    return false;
}
void EditorViewportController::Prepare(EditorSession& s,AssetStore& assets,ui::UiRect rect,std::uint64_t frame)
{
    if(!initialized_){pivot_=camera_.state.position+Forward(camera_)*distance_;initialized_=true;}
    viewport_=rect;overlay_.Clear();sceneLines_.clear();handles_.clear();
    snapshot_=s.IsPlaying()?std::nullopt:camera_.Snapshot({rect.x,rect.y,rect.width,rect.height},frame);
    if(gizmo_.Active()&&!gizmo_.ProjectionMatches(snapshot_))Cancel(s);
    if(snapshot_)Draw(s,assets);
}
void EditorViewportController::Draw(EditorSession& s,AssetStore& assets)
{
    const auto& c=*snapshot_;overlay_.PushClip(viewport_);
    const auto line=[&](Vec3 a,Vec3 b,UiColor color,float width=1.f,int axis=-1)
    {
        Vec2 x,y;if(!Segment(c,a,b,x,y))return;
        sceneLines_.push_back({a,b,{color.r,color.g,color.b,color.a},width,axis>=0?.22f:0.f});
        if(axis>=0)handles_.push_back({x,y,axis});
    };
    if(grid_)
    {
        // XZ 网格由渲染器解析无限平面，单位固定；这里只保留原点的 Y 轴标记。
        line({0,0,0},{0,1,0},colors[1],2);
    }
    if(gizmos_)for(auto e:s.EditWorld().Entities())if(s.EditWorld().IsEnabled(e)&&(s.EditWorld().GetCamera(e)||s.EditWorld().GetLight(e)))
    {
        const auto p=ProjectWorldPoint(c,TransformPoint(s.EditWorld().GetWorldMatrix(e),{}));
        if(p&&p->z>=0&&p->z<=1){overlay_.CanvasCircle(p->x,p->y,7,{1,.8f,.3f,1});overlay_.Text({p->x-4,p->y-7,16,16},s.EditWorld().GetCamera(e)?"C":"L",12,{.1f,.1f,.1f,1});}
    }
    const auto e=s.EditWorld().FindByStableId(s.Selected());
    if(e)
    {
        // 实体的可见轮廓由渲染器按实际网格和场景深度生成，不再叠加包围盒背面边线。
        const auto origin=Pivot(s.EditWorld(),assets,e);const auto p=ProjectWorldPoint(c,origin);
        if(gizmos_&&!viewTool_&&p&&p->z>0&&p->z<1)
        {
            const auto view=c.view*Vec4{origin.x,origin.y,origin.z,1};
            const float radius=std::max(.001f,-view.z)*2*std::tan(Pi/6)*80/std::max(1.f,viewport_.height);
            for(int axis=0;axis<3;++axis)
            {
                const auto direction=EditorGizmoAxis(s.EditWorld(),e,static_cast<GizmoAxis>(axis),space_);
                const UiColor color=(axis==dragAxis_||axis==hoverAxis_)?UiColor{1,1,.3f,1}:colors[axis];
                if(mode_==GizmoMode::Rotate)
                {
                    const auto u=Normalize(Cross(direction,std::abs(direction.y)<.9f?Vec3{0,1,0}:Vec3{1,0,0})),v=Cross(direction,u);
                    for(int i=0;i<64;++i){const float a=i*2*Pi/64,b=(i+1)*2*Pi/64;line(origin+(u*std::cos(a)+v*std::sin(a))*radius,origin+(u*std::cos(b)+v*std::sin(b))*radius,color,2.5f,axis);}
                }
                else
                {
                    const auto end=origin+direction*radius;line(origin+direction*radius*.15f,end,color,3,axis);
                    if(auto tip=ProjectWorldPoint(c,end);tip&&tip->z>0&&tip->z<1)
                    {
                        if(mode_==GizmoMode::Scale)
                        {
                            const float half=radius*.045f;Vec3 corners[8];
                            for(int i=0;i<8;++i)corners[i]=end+Vec3{i&1?half:-half,i&2?half:-half,i&4?half:-half};
                            for(int i=0;i<8;++i)for(int bit=1;bit<=4;bit*=2)if(!(i&bit))line(corners[i],corners[i|bit],color,2,axis);
                        }
                        else
                        {
                            auto side=Cross(direction,Forward(camera_));
                            if(Length(side)<.001f)side=Cross(direction,{0,1,0});
                            side=Normalize(side)*radius*.065f;
                            line(end,end-direction*radius*.15f+side,color,3,axis);
                            line(end,end-direction*radius*.15f-side,color,3,axis);
                        }
                    }
                }
            }
        }
    }
    overlay_.PopClip();
}
}
