#include "FunctionLayer/Physics/PhysicsWorld.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <limits>

namespace se
{
namespace
{
float Component(Vec3 value,int axis) {return axis==0?value.x:axis==1?value.y:value.z;}
Vec3 ClampToBox(Vec3 p,Vec3 h)
{return {std::clamp(p.x,-h.x,h.x),std::clamp(p.y,-h.y,h.y),std::clamp(p.z,-h.z,h.z)};}
struct CapsuleContact {Vec3 normal,point;float depth;};
std::optional<CapsuleContact> CapsuleBox(Vec3 center,float halfHeight,float radius,const BoxShape& box)
{
    const Quat inverse=Conjugate(box.rotation);
    const Vec3 a=Rotate(inverse,center-Vec3{0,halfHeight,0}-box.center);
    const Vec3 b=Rotate(inverse,center+Vec3{0,halfHeight,0}-box.center),d=b-a,h=box.halfExtents;
    // 点到 AABB 的距离在各坐标穿过面的位置处分段；每段为一元二次函数。
    std::vector<float> cuts{0,1};
    for(int axis=0;axis<3;++axis)if(std::abs(Component(d,axis))>1e-8f)
        for(float sign:{-1.0f,1.0f})
        {
            const float t=(sign*Component(h,axis)-Component(a,axis))/Component(d,axis);
            if(t>0&&t<1)cuts.push_back(t);
        }
    std::sort(cuts.begin(),cuts.end());
    float best=std::numeric_limits<float>::max();Vec3 segmentPoint{},boxPoint{};
    const auto sample=[&](float t)
    {
        const Vec3 p=a+d*t,q=ClampToBox(p,h);const float distance=LengthSquared(p-q);
        if(distance<best){best=distance;segmentPoint=p;boxPoint=q;}
    };
    for(std::size_t i=1;i<cuts.size();++i)
    {
        const float lo=cuts[i-1],hi=cuts[i],mid=(lo+hi)*0.5f;float numerator=0,denominator=0;
        sample(lo);sample(hi);
        for(int axis=0;axis<3;++axis)
        {
            const float value=Component(a+d*mid,axis),extent=Component(h,axis);
            if(value>=-extent&&value<=extent)continue;
            const float start=Component(a,axis)-(value<0?-extent:extent),slope=Component(d,axis);
            numerator+=start*slope;denominator+=slope*slope;
        }
        if(denominator>1e-10f)sample(std::clamp(-numerator/denominator,lo,hi));
    }
    const float distance=std::sqrt(best);if(distance>radius+0.001f)return std::nullopt;
    Vec3 normal{};float depth=radius-distance;
    if(distance>1e-6f)normal=(segmentPoint-boxPoint)/distance;
    else
    {
        // 出生于箱内也有确定的退出方向；按整个胶囊线段的投影计算平移。
        depth=std::numeric_limits<float>::max();
        for(int axis=0;axis<3;++axis)for(float sign:{-1.0f,1.0f})
        {
            const float low=std::min(Component(a,axis),Component(b,axis));
            const float high=std::max(Component(a,axis),Component(b,axis));
            const float candidate=Component(h,axis)+(sign>0?-low:high)+radius;
            if(candidate<depth)
            {depth=candidate;normal=axis==0?Vec3{sign,0,0}:axis==1?Vec3{0,sign,0}:Vec3{0,0,sign};}
        }
    }
    return CapsuleContact{Rotate(box.rotation,normal),box.center+Rotate(box.rotation,boxPoint),depth};
}
}

bool CharacterController::Move(PhysicsWorld& world,Vec3 horizontalDirection,bool jumpPressed,float seconds)
{
    if(!IsFinite(position)||!IsFinite(horizontalDirection)||!IsFinite(radius)||radius<=0.001f
        ||!IsFinite(segmentHalfHeight)||segmentHalfHeight<0||!IsFinite(verticalVelocity)
        ||!IsFinite(walkSpeed)||walkSpeed<0||!IsFinite(jumpSpeed)||jumpSpeed<0
        ||!IsFinite(maximumPushImpulse)||maximumPushImpulse<0||!IsFinite(seconds)||seconds<=0||seconds>0.1f
        ||!IsFinite(world.gravity))return false;
    horizontalDirection.y=0;const float inputLength=Length(horizontalDirection);
    if(!IsFinite(inputLength))return false;
    if(inputLength>1)horizontalDirection=horizontalDirection/inputLength;
    const float nextVertical=(jumpPressed&&grounded?jumpSpeed:verticalVelocity)+world.gravity.y*seconds;
    const Vec3 displacement=(horizontalDirection*walkSpeed+Vec3{0,nextVertical,0})*seconds;
    const float count=std::ceil(Length(displacement)/(radius*0.25f));
    if(!IsFinite(nextVertical)||!IsFinite(count)||count>128)return false;
    const unsigned steps=static_cast<unsigned>(std::max(1.0f,count));
    const Vec3 increment=displacement/static_cast<float>(steps);
    // 超出分段预算时不留下半次跳跃。
    verticalVelocity=nextVertical;
    float pushBudget=maximumPushImpulse;grounded=false;
    const auto bodies=world.Bodies();
    for(unsigned step=0;step<steps;++step)
    {
        position+=increment;
        for(unsigned iteration=0;iteration<8;++iteration)
        {
            bool corrected=false;
            for(const auto handle:bodies)
            {
                const auto* body=world.GetBody(handle);if(!body)continue;
                const auto contact=CapsuleBox(position,segmentHalfHeight,radius,body->description.shape);
                if(!contact)continue;
                if(contact->normal.y>0.65f&&verticalVelocity<=0){grounded=true;verticalVelocity=0;}
                if(contact->normal.y< -0.65f&&verticalVelocity>0)verticalVelocity=0;
                if(contact->depth>0)
                {
                    position+=contact->normal*(contact->depth+0.0001f);corrected=true;
                    const float into=-Dot(horizontalDirection*walkSpeed,contact->normal);
                    if(body->description.mass>0&&into>0&&pushBudget>0)
                    {
                        const Vec3 pushDirection=-contact->normal;
                        const Vec3 pointVelocity=body->linearVelocity+Cross(body->angularVelocity,
                            contact->point-body->description.shape.center);
                        // 只补足沿接触法线的目标速度，且整次移动共享有限冲量预算。
                        const float needed=std::max(0.0f,into-Dot(pointVelocity,pushDirection))*body->description.mass;
                        const float impulse=std::min(pushBudget,needed);
                        if(impulse>0){world.ApplyImpulse(handle,pushDirection*impulse,contact->point);pushBudget-=impulse;}
                    }
                }
            }
            if(!corrected)break;
        }
    }
    return IsFinite(position);
}

std::vector<DebugLine> CharacterController::DebugLines() const
{
    std::vector<DebugLine> lines;
    if(!IsFinite(position)||!IsFinite(radius)||radius<=0||!IsFinite(segmentHalfHeight)||segmentHalfHeight<0)return lines;
    constexpr unsigned segments=16;const Vec3 color{1,0.8f,0.1f};
    for(unsigned i=0;i<segments;++i)
    {
        const float a=2*Pi*static_cast<float>(i)/segments,b=2*Pi*static_cast<float>(i+1)/segments;
        for(float sign:{-1.0f,1.0f})
        {
            const Vec3 center=position+Vec3{0,sign*segmentHalfHeight,0};
            lines.push_back({center+Vec3{radius*std::cos(a),0,radius*std::sin(a)},center+Vec3{radius*std::cos(b),0,radius*std::sin(b)},color});
            const float arcA=Pi*static_cast<float>(i)/segments,arcB=Pi*static_cast<float>(i+1)/segments;
            for(unsigned axis=0;axis<2;++axis)
            {
                const Vec3 p=axis==0?Vec3{radius*std::cos(arcA),sign*radius*std::sin(arcA),0}:Vec3{0,sign*radius*std::sin(arcA),radius*std::cos(arcA)};
                const Vec3 q=axis==0?Vec3{radius*std::cos(arcB),sign*radius*std::sin(arcB),0}:Vec3{0,sign*radius*std::sin(arcB),radius*std::cos(arcB)};
                lines.push_back({center+p,center+q,color});
            }
        }
    }
    for(const Vec3 axis:{Vec3{1,0,0},Vec3{-1,0,0},Vec3{0,0,1},Vec3{0,0,-1}})
        lines.push_back({position+axis*radius-Vec3{0,segmentHalfHeight,0},position+axis*radius+Vec3{0,segmentHalfHeight,0},color});
    return lines;
}
}
