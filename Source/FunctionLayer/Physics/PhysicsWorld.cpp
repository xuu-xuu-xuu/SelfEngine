#include "FunctionLayer/Physics/PhysicsWorld.h"

#include <algorithm>
#include <array>
#include <atomic>
#include <cmath>
#include <limits>
#include <unordered_set>

namespace se
{
namespace
{
std::atomic<std::uint64_t> nextBodyGeneration{1};
float Component(Vec3 value,int axis) {return axis==0?value.x:axis==1?value.y:value.z;}
std::array<Vec3,3> Axes(const BoxShape& box)
{return {Rotate(box.rotation,{1,0,0}),Rotate(box.rotation,{0,1,0}),Rotate(box.rotation,{0,0,1})};}
Vec3 LocalPoint(const BoxShape& box,Vec3 point) {return Rotate(Conjugate(box.rotation),point-box.center);}
Vec3 Support(const BoxShape& box,Vec3 direction)
{
    const auto axes=Axes(box);Vec3 result=box.center;
    for(int i=0;i<3;++i)result+=axes[i]*(Dot(axes[i],direction)>=0?Component(box.halfExtents,i):-Component(box.halfExtents,i));
    return result;
}
bool Overlap(const Aabb& a,const Aabb& b,float margin)
{
    return a.minimum.x<=b.maximum.x+margin && a.maximum.x+margin>=b.minimum.x
        && a.minimum.y<=b.maximum.y+margin && a.maximum.y+margin>=b.minimum.y
        && a.minimum.z<=b.maximum.z+margin && a.maximum.z+margin>=b.minimum.z;
}
std::uint64_t Feature(std::uint64_t a,std::uint64_t b,std::uint64_t plane)
{
    if(a>b)std::swap(a,b);
    std::uint64_t hash=14695981039346656037ull;
    for(const auto value:{a,b,plane})for(unsigned shift=0;shift<64;shift+=8)
    {hash^=(value>>shift)&255;hash*=1099511628211ull;}
    return hash;
}
struct ClipVertex {Vec3 position;std::uint64_t feature;};
std::vector<ClipVertex> Clip(const std::vector<ClipVertex>& polygon,Vec3 center,Vec3 normal,float distance,std::uint64_t plane)
{
    std::vector<ClipVertex> result;if(polygon.empty())return result;
    auto previous=polygon.back();float previousDistance=Dot(previous.position-center,normal)-distance;
    for(const auto current:polygon)
    {
        const float currentDistance=Dot(current.position-center,normal)-distance;
        if((currentDistance<=0)!=(previousDistance<=0))
            result.push_back({previous.position+(current.position-previous.position)*(previousDistance/(previousDistance-currentDistance)),Feature(previous.feature,current.feature,plane)});
        if(currentDistance<=0)result.push_back(current);
        previous=current;previousDistance=currentDistance;
    }
    return result;
}
std::pair<Vec3,Vec3> ClosestSegments(Vec3 p1,Vec3 q1,Vec3 p2,Vec3 q2)
{
    const Vec3 d1=q1-p1,d2=q2-p2,r=p1-p2;const float a=Dot(d1,d1),e=Dot(d2,d2),f=Dot(d2,r);
    float s=0,t=0;
    if(a<=1e-10f && e<=1e-10f)return {p1,p2};
    if(a<=1e-10f)t=std::clamp(f/e,0.0f,1.0f);
    else
    {
        const float c=Dot(d1,r);
        if(e<=1e-10f)s=std::clamp(-c/a,0.0f,1.0f);
        else
        {
            const float b=Dot(d1,d2),denominator=a*e-b*b;
            if(denominator>1e-10f)s=std::clamp((b*f-c*e)/denominator,0.0f,1.0f);
            t=(b*s+f)/e;
            if(t<0){t=0;s=std::clamp(-c/a,0.0f,1.0f);}
            else if(t>1){t=1;s=std::clamp((b-c)/a,0.0f,1.0f);}
        }
    }
    return {p1+d1*s,p2+d2*t};
}
bool ValidShape(const BoxShape& shape)
{
    return IsFinite(shape.center)&&IsFinite(shape.halfExtents)&&IsFinite(shape.rotation)
        && IsFinite(LengthSquared(shape.rotation))&&LengthSquared(shape.rotation)>1e-10f && shape.halfExtents.x>0.001f
        && shape.halfExtents.y>0.001f && shape.halfExtents.z>0.001f;
}
}

Aabb ComputeAabb(const BoxShape& box)
{
    const auto axes=Axes(box);Vec3 radius;
    for(int i=0;i<3;++i)
    {const float h=Component(box.halfExtents,i);radius+={std::abs(axes[i].x)*h,std::abs(axes[i].y)*h,std::abs(axes[i].z)*h};}
    return {box.center-radius,box.center+radius};
}

std::optional<ContactManifold> CollideBoxes(const BoxShape& a,const BoxShape& b,float margin)
{
    if(!ValidShape(a)||!ValidShape(b)||!IsFinite(margin)||margin<0)return std::nullopt;
    const auto ax=Axes(a),bx=Axes(b);const Vec3 delta=b.center-a.center;
    float minimum=std::numeric_limits<float>::max();int selected=-1;Vec3 normal;
    for(int axisIndex=0;axisIndex<15;++axisIndex)
    {
        Vec3 axis=axisIndex<3?ax[axisIndex]:axisIndex<6?bx[axisIndex-3]:Cross(ax[(axisIndex-6)/3],bx[(axisIndex-6)%3]);
        const float length=Length(axis);if(length<1e-5f)continue;axis=axis/length;
        float ra=0,rb=0;for(int i=0;i<3;++i){ra+=Component(a.halfExtents,i)*std::abs(Dot(ax[i],axis));rb+=Component(b.halfExtents,i)*std::abs(Dot(bx[i],axis));}
        const float penetration=ra+rb-std::abs(Dot(delta,axis));if(penetration < -margin)return std::nullopt;
        // 近似相等时保留面轴，提高连续接触的稳定性。
        if(selected<0 || penetration < minimum-(axisIndex>=6?0.001f:0.00001f))
        {minimum=penetration;selected=axisIndex;normal=Dot(delta,axis)>=0?axis:-axis;}
    }
    ContactManifold result;result.normal=normal;result.separationDistance=std::max(minimum,0.0f);
    if(selected<6)
    {
        const bool referenceA=selected<3;
        const auto& reference=referenceA?a:b;const auto& incident=referenceA?b:a;
        const auto referenceAxes=referenceA?ax:bx,incidentAxes=referenceA?bx:ax;
        const int referenceAxis=selected%3;const Vec3 referenceNormal=referenceA?normal:-normal;
        const Vec3 center=reference.center+referenceNormal*Component(reference.halfExtents,referenceAxis);
        int incidentAxis=0;for(int i=1;i<3;++i)if(std::abs(Dot(incidentAxes[i],referenceNormal))>std::abs(Dot(incidentAxes[incidentAxis],referenceNormal)))incidentAxis=i;
        const float sign=Dot(incidentAxes[incidentAxis],referenceNormal)>0?-1.0f:1.0f;
        const Vec3 incidentCenter=incident.center+incidentAxes[incidentAxis]*(sign*Component(incident.halfExtents,incidentAxis));
        const int u=(incidentAxis+1)%3,v=(incidentAxis+2)%3;
        const Vec3 sideU=incidentAxes[u]*Component(incident.halfExtents,u),sideV=incidentAxes[v]*Component(incident.halfExtents,v);
        const auto faceFeature=Feature(static_cast<std::uint64_t>(selected*2+(Dot(referenceNormal,referenceAxes[referenceAxis])>0)),
            static_cast<std::uint64_t>(16+incidentAxis*2+(sign>0)),32);
        std::vector<ClipVertex> polygon{{incidentCenter-sideU-sideV,Feature(faceFeature,0,64)},
            {incidentCenter+sideU-sideV,Feature(faceFeature,1,64)},{incidentCenter+sideU+sideV,Feature(faceFeature,2,64)},
            {incidentCenter-sideU+sideV,Feature(faceFeature,3,64)}};
        for(int i=0;i<3;++i)if(i!=referenceAxis)
        {polygon=Clip(polygon,center,referenceAxes[i],Component(reference.halfExtents,i),static_cast<std::uint64_t>(i*2));
            polygon=Clip(polygon,center,-referenceAxes[i],Component(reference.halfExtents,i),static_cast<std::uint64_t>(i*2+1));}
        std::vector<ContactPoint> points;
        for(std::size_t i=0;i<polygon.size();++i)
        {
            const float depth=Dot(center-polygon[i].position,referenceNormal);
            if(depth>=-margin)points.push_back({polygon[i].position+referenceNormal*(depth*0.5f),std::max(depth,0.0f),polygon[i].feature});
        }
        if(points.size()<=4)result.points=std::move(points);
        else for(std::size_t i=0;i<4;++i)result.points.push_back(points[i*points.size()/4]);
    }
    else
    {
        result.kind=ContactKind::Edge;
        const int ai=(selected-6)/3,bi=(selected-6)%3;
        Vec3 ac=a.center,bc=b.center;std::uint64_t sideBits=0;
        for(int i=0;i<3;++i)
        {
            if(i!=ai&&Dot(ax[i],normal)>=0)sideBits|=std::uint64_t{1}<<i;
            if(i!=bi&&Dot(bx[i],normal)>=0)sideBits|=std::uint64_t{1}<<(i+3);
            if(i!=ai)ac+=ax[i]*(Dot(ax[i],normal)>=0?Component(a.halfExtents,i):-Component(a.halfExtents,i));
            if(i!=bi)bc+=bx[i]*(Dot(bx[i],normal)>=0?-Component(b.halfExtents,i):Component(b.halfExtents,i));
        }
        const auto points=ClosestSegments(ac-ax[ai]*Component(a.halfExtents,ai),ac+ax[ai]*Component(a.halfExtents,ai),
            bc-bx[bi]*Component(b.halfExtents,bi),bc+bx[bi]*Component(b.halfExtents,bi));
        result.points.push_back({(points.first+points.second)*0.5f,std::max(minimum,0.0f),Feature(static_cast<std::uint64_t>(selected),sideBits,128)});
    }
    if(result.points.empty())result.points.push_back({(Support(a,normal)+Support(b,-normal))*0.5f,std::max(minimum,0.0f),0});
    return result;
}

bool PhysicsWorld::Fail(const char* key) const {error_=key;return false;}
bool PhysicsWorld::IsAlive(BodyHandle body) const {return body && body.index<bodies_.size()&&bodies_[body.index].alive&&bodies_[body.index].generation==body.generation;}
const BodyState* PhysicsWorld::GetBody(BodyHandle body) const {return IsAlive(body)?&bodies_[body.index].state:nullptr;}
std::vector<BodyHandle> PhysicsWorld::Bodies() const
{std::vector<BodyHandle> result;for(std::size_t i=0;i<bodies_.size();++i)if(bodies_[i].alive)result.push_back({static_cast<std::uint32_t>(i),bodies_[i].generation});return result;}
BodyHandle PhysicsWorld::CreateBody(const BodyDesc& description)
{
    if(!ValidShape(description.shape)||!IsFinite(description.mass)||description.mass<0
        ||!IsFinite(description.friction)||description.friction<0||!IsFinite(description.restitution)||description.restitution<0||description.restitution>1
        ||!IsFinite(description.linearDamping)||description.linearDamping<0||!IsFinite(description.angularDamping)||description.angularDamping<0)
    {Fail("Physics.InvalidBody");return {};}
    Vec3 inverseInertia{};float inverseMass=0;
    if(description.mass>0)
    {
        inverseMass=1/description.mass;const auto h=description.shape.halfExtents;
        inverseInertia={3/(description.mass*(h.y*h.y+h.z*h.z)),3/(description.mass*(h.x*h.x+h.z*h.z)),3/(description.mass*(h.x*h.x+h.y*h.y))};
        if(!IsFinite(inverseMass)||!IsFinite(inverseInertia)){Fail("Physics.InvalidBody");return {};}
    }
    const auto generation=nextBodyGeneration.fetch_add(1);if(generation>UINT32_MAX){Fail("Physics.HandleExhausted");return {};}
    std::uint32_t index;
    if(free_.empty()){if(bodies_.size()>=UINT32_MAX){Fail("Physics.HandleExhausted");return {};}index=static_cast<std::uint32_t>(bodies_.size());bodies_.emplace_back();}
    else {index=free_.back();free_.pop_back();}
    auto& body=bodies_[index];body={};body.alive=true;body.generation=static_cast<std::uint32_t>(generation);
    body.state.description=description;body.state.description.shape.rotation=Normalize(description.shape.rotation);
    body.inverseMass=inverseMass;body.inverseInertia=inverseInertia;
    error_.clear();return {index,body.generation};
}
bool PhysicsWorld::DestroyBody(BodyHandle body)
{
    if(!IsAlive(body))return Fail("Physics.InvalidBodyHandle");bodies_[body.index].alive=false;free_.push_back(body.index);
    // 删除支撑物后周围物体必须重新参与模拟；小场景基线先唤醒所有动态体。
    for(auto& candidate:bodies_)if(candidate.alive&&candidate.inverseMass>0)Wake(candidate);
    std::erase_if(cache_,[&](const auto& entry){return entry.first.a==body.Pack()||entry.first.b==body.Pack();});
    std::erase_if(contacts_,[&](const auto& contact){return contact.a==body||contact.b==body;});
    std::erase_if(candidates_,[&](const auto& pair){return pair.a==body||pair.b==body;});statistics_.candidates=candidates_.size();
    std::erase_if(events_,[&](const auto& event){return event.a==body||event.b==body;});
    return true;
}
void PhysicsWorld::Wake(Body& body) {body.state.sleeping=false;body.sleepTime=0;}
Vec3 PhysicsWorld::InverseInertia(const Body& body,Vec3 value) const
{
    if(body.state.sleeping)return {};
    const auto rotation=body.state.description.shape.rotation;
    return Rotate(rotation,Hadamard(body.inverseInertia,Rotate(Conjugate(rotation),value)));
}
void PhysicsWorld::AddImpulse(Body& body,Vec3 impulse,Vec3 point)
{
    if(body.inverseMass==0 || body.state.sleeping)return;
    body.state.linearVelocity+=impulse*body.inverseMass;
    body.state.angularVelocity+=InverseInertia(body,Cross(point-body.state.description.shape.center,impulse));
}
bool PhysicsWorld::SetVelocity(BodyHandle body,Vec3 linear,Vec3 angular)
{
    if(!IsAlive(body))return Fail("Physics.InvalidBodyHandle");if(!IsFinite(linear)||!IsFinite(angular))return Fail("Physics.InvalidValue");
    if(LengthSquared(linear)>MaximumLinearSpeed*MaximumLinearSpeed||LengthSquared(angular)>MaximumAngularSpeed*MaximumAngularSpeed)return Fail("Physics.SpeedLimit");
    auto& value=bodies_[body.index];if(value.inverseMass==0)return Fail("Physics.StaticBody");
    Wake(value);value.state.linearVelocity=linear;value.state.angularVelocity=angular;return true;
}
bool PhysicsWorld::ApplyForce(BodyHandle body,Vec3 force,Vec3 point)
{
    if(!IsAlive(body))return Fail("Physics.InvalidBodyHandle");if(!IsFinite(force)||!IsFinite(point))return Fail("Physics.InvalidValue");
    auto& value=bodies_[body.index];if(value.inverseMass==0)return Fail("Physics.StaticBody");
    Wake(value);value.force+=force;value.torque+=Cross(point-value.state.description.shape.center,force);return true;
}
bool PhysicsWorld::ApplyImpulse(BodyHandle body,Vec3 impulse,Vec3 point)
{
    if(!IsAlive(body))return Fail("Physics.InvalidBodyHandle");if(!IsFinite(impulse)||!IsFinite(point))return Fail("Physics.InvalidValue");
    auto& value=bodies_[body.index];if(value.inverseMass==0)return Fail("Physics.StaticBody");Wake(value);AddImpulse(value,impulse,point);return true;
}
bool PhysicsWorld::Teleport(BodyHandle body,Vec3 position,Quat rotation,bool clearVelocity)
{
    if(!IsAlive(body))return Fail("Physics.InvalidBodyHandle");if(!IsFinite(position)||!IsFinite(rotation)||!IsFinite(LengthSquared(rotation))||LengthSquared(rotation)<1e-10f)return Fail("Physics.InvalidValue");
    auto& value=bodies_[body.index];value.state.description.shape.center=position;value.state.description.shape.rotation=Normalize(rotation);
    if(clearVelocity){value.state.linearVelocity={};value.state.angularVelocity={};}
    // 支撑物传送同样可能使休眠堆叠失去支撑。
    for(auto& candidate:bodies_)if(candidate.alive&&candidate.inverseMass>0)Wake(candidate);
    for(auto& [key,points]:cache_)if(key.a==body.Pack()||key.b==body.Pack())
        for(auto& point:points){point.normalImpulse=0;point.tangentImpulse={};}
    std::erase_if(contacts_,[&](const auto& contact){return contact.a==body||contact.b==body;});
    std::erase_if(candidates_,[&](const auto& pair){return pair.a==body||pair.b==body;});statistics_.candidates=candidates_.size();
    return true;
}

bool PhysicsWorld::Step(float seconds)
{
    if(environmentDriven_&&!environmentStep_)return Fail("Physics.ClockOwned");
    if(!IsFinite(seconds)||seconds<=0||seconds>0.1f||!IsFinite(gravity)||velocityIterations==0||velocityIterations>64)return Fail("Physics.InvalidStep");
    struct Constraint {BodyHandle a,b;PairKey key;Vec3 point,normal,t1,t2,localA,localB;float massN=0,massT1=0,massT2=0,bias=0,friction=0,normalImpulse=0,tangent1=0,tangent2=0;std::uint64_t feature=0;};
    std::vector<Constraint> constraints;contacts_.clear();candidates_.clear();events_.clear();statistics_={};
    for(auto& body:bodies_)if(body.alive&&body.inverseMass>0)
    {
        if(!body.state.sleeping)
        {
            body.state.linearVelocity+=(gravity+body.force*body.inverseMass)*seconds;
            body.state.angularVelocity+=InverseInertia(body,body.torque)*seconds;
            body.state.linearVelocity*=std::exp(-body.state.description.linearDamping*seconds);
            body.state.angularVelocity*=std::exp(-body.state.description.angularDamping*seconds);
        }
        body.force={};body.torque={};
    }
    const auto handles=Bodies();std::vector<bool> touching(bodies_.size(),false);
    std::unordered_set<PairKey,PairHash> currentPairs;
    std::vector<Aabb> bounds;bounds.reserve(handles.size());for(const auto handle:handles)bounds.push_back(ComputeAabb(bodies_[handle.index].state.description.shape));
    statistics_.aabbUpdates=bounds.size();
    for(std::size_t i=0;i<handles.size();++i)for(std::size_t j=i+1;j<handles.size();++j)
    {
        auto& a=bodies_[handles[i].index];auto& b=bodies_[handles[j].index];
        const auto& ad=a.state.description;const auto& bd=b.state.description;
        if((a.inverseMass==0&&b.inverseMass==0)||!(ad.mask&bd.layer)||!(bd.mask&ad.layer))continue;
        ++statistics_.aabbTests;if(Overlap(bounds[i],bounds[j],0.005f))candidates_.push_back({handles[i],handles[j]});
    }
    statistics_.candidates=candidates_.size();
    for(const auto pair:candidates_)
    {
        auto& a=bodies_[pair.a.index];auto& b=bodies_[pair.b.index];const auto& ad=a.state.description;const auto& bd=b.state.description;
        auto collision=CollideBoxes(ad.shape,bd.shape);if(!collision)continue;
        const PairKey key{pair.a.Pack(),pair.b.Pack()};currentPairs.insert(key);
        touching[pair.a.index]=touching[pair.b.index]=true;
        if(!cache_.contains(key))events_.push_back({pair.a,pair.b,ad.entityStableId,bd.entityStableId,true,false});
        contacts_.push_back({pair.a,pair.b,*collision});statistics_.contacts+=collision->points.size();
        const auto moving=[](const Body& value){return value.inverseMass>0&&!value.state.sleeping
            &&(LengthSquared(value.state.linearVelocity)>0.04f||LengthSquared(value.state.angularVelocity)>0.04f);};
        if((a.state.sleeping&&moving(b))||(b.state.sleeping&&moving(a)))
        {if(a.inverseMass>0)Wake(a);if(b.inverseMass>0)Wake(b);}
        for(const auto& point:collision->points)
        {
            statistics_.maximumPenetration=std::max(statistics_.maximumPenetration,point.penetration);
            Constraint c;c.a=pair.a;c.b=pair.b;c.key=key;c.point=point.position;c.normal=collision->normal;
            c.feature=point.feature;
            c.localA=LocalPoint(ad.shape,c.point);c.localB=LocalPoint(bd.shape,c.point);
            c.t1=Normalize(Cross(std::abs(c.normal.x)<0.7f?Vec3{1,0,0}:Vec3{0,1,0},c.normal));c.t2=Cross(c.normal,c.t1);
            const Vec3 ra=c.point-ad.shape.center,rb=c.point-bd.shape.center;
            const auto effective=[&](Vec3 direction)
            {
                const float value=(a.state.sleeping?0:a.inverseMass)+(b.state.sleeping?0:b.inverseMass)
                    +Dot(direction,Cross(InverseInertia(a,Cross(ra,direction)),ra)+Cross(InverseInertia(b,Cross(rb,direction)),rb));
                return value>1e-9f?1/value:0.0f;
            };
            c.massN=effective(c.normal);c.massT1=effective(c.t1);c.massT2=effective(c.t2);
            const Vec3 relative=b.state.linearVelocity+Cross(b.state.angularVelocity,rb)-a.state.linearVelocity-Cross(a.state.angularVelocity,ra);
            const float closing=Dot(relative,c.normal);
            c.bias=std::min(MaximumCorrectionSpeed,PositionCorrectionRate*std::max(point.penetration-PenetrationSlop,0.0f)/seconds);
            if(closing< -RestitutionVelocityThreshold)c.bias=std::max(c.bias,-std::max(ad.restitution,bd.restitution)*closing);
            c.friction=std::sqrt(ad.friction*bd.friction);
            if(const auto found=cache_.find(key);found!=cache_.end())
            {
                float nearest=0.005f;const CachedPoint* match=nullptr;
                for(const auto& previous:found->second)
                {
                    const float distance=LengthSquared(previous.localA-c.localA)+LengthSquared(previous.localB-c.localB);
                    if(distance>=0.005f||Dot(previous.normal,c.normal)<=0.95f)continue;
                    if(previous.feature==c.feature){match=&previous;++statistics_.cachedFeatureMatches;break;}
                    if(distance<nearest){nearest=distance;match=&previous;}
                }
                if(match){c.normalImpulse=match->normalImpulse;c.tangent1=Dot(match->tangentImpulse,c.t1);c.tangent2=Dot(match->tangentImpulse,c.t2);}
            }
            if(c.massN==0){c.normalImpulse=0;c.tangent1=0;c.tangent2=0;}
            if(c.normalImpulse>0)++statistics_.warmStartedContacts;
            const Vec3 impulse=c.normal*c.normalImpulse+c.t1*c.tangent1+c.t2*c.tangent2;
            AddImpulse(a,-impulse,c.point);AddImpulse(b,impulse,c.point);constraints.push_back(c);
        }
    }
    for(unsigned iteration=0;iteration<velocityIterations;++iteration)for(auto& c:constraints)
    {
        if(c.massN==0)continue;
        auto& a=bodies_[c.a.index];auto& b=bodies_[c.b.index];
        const auto relative=[&](){return b.state.linearVelocity+Cross(b.state.angularVelocity,c.point-b.state.description.shape.center)
            -a.state.linearVelocity-Cross(a.state.angularVelocity,c.point-a.state.description.shape.center);};
        const float old=c.normalImpulse;c.normalImpulse=std::max(0.0f,old+c.massN*(c.bias-Dot(relative(),c.normal)));
        Vec3 impulse=c.normal*(c.normalImpulse-old);AddImpulse(a,-impulse,c.point);AddImpulse(b,impulse,c.point);
        const Vec3 velocity=relative();const float old1=c.tangent1,old2=c.tangent2;
        c.tangent1-=c.massT1*Dot(velocity,c.t1);c.tangent2-=c.massT2*Dot(velocity,c.t2);
        const float limit=c.friction*c.normalImpulse,length=std::sqrt(c.tangent1*c.tangent1+c.tangent2*c.tangent2);
        if(length>limit&&length>0){c.tangent1*=limit/length;c.tangent2*=limit/length;}
        impulse=c.t1*(c.tangent1-old1)+c.t2*(c.tangent2-old2);AddImpulse(a,-impulse,c.point);AddImpulse(b,impulse,c.point);
    }
    for(const auto& [key,points]:cache_)if(!currentPairs.contains(key))
    {
        (void)points;const BodyHandle a{static_cast<std::uint32_t>(key.a),static_cast<std::uint32_t>(key.a>>32)},b{static_cast<std::uint32_t>(key.b),static_cast<std::uint32_t>(key.b>>32)};
        events_.push_back({a,b,IsAlive(a)?bodies_[a.index].state.description.entityStableId:0,IsAlive(b)?bodies_[b.index].state.description.entityStableId:0,false,true});
    }
    cache_.clear();for(const auto& c:constraints)cache_[c.key].push_back({c.localA,c.localB,c.normal,c.t1*c.tangent1+c.t2*c.tangent2,c.normalImpulse,c.feature});
    for(std::size_t i=0;i<bodies_.size();++i)
    {
        auto& body=bodies_[i];if(!body.alive||body.inverseMass==0)continue;
        if(!body.state.sleeping)
        {
            auto& state=body.state;
            if(!IsFinite(state.linearVelocity)||!IsFinite(state.angularVelocity))return Fail("Physics.NonFiniteState");
            if(LengthSquared(state.linearVelocity)>MaximumLinearSpeed*MaximumLinearSpeed
                ||LengthSquared(state.angularVelocity)>MaximumAngularSpeed*MaximumAngularSpeed)return Fail("Physics.SpeedLimit");
            state.description.shape.center+=state.linearVelocity*seconds;
            const Quat omega{state.angularVelocity.x,state.angularVelocity.y,state.angularVelocity.z,0};
            const Quat derivative=omega*state.description.shape.rotation;auto& q=state.description.shape.rotation;
            q=Normalize(Quat{q.x+derivative.x*seconds*0.5f,q.y+derivative.y*seconds*0.5f,q.z+derivative.z*seconds*0.5f,q.w+derivative.w*seconds*0.5f});
            if(!IsFinite(state.description.shape.center)||!IsFinite(state.linearVelocity)||!IsFinite(state.angularVelocity))return Fail("Physics.NonFiniteState");
            if(touching[i]&&LengthSquared(state.linearVelocity)<SleepLinearSpeed*SleepLinearSpeed&&LengthSquared(state.angularVelocity)<SleepAngularSpeed*SleepAngularSpeed)body.sleepTime+=seconds;else body.sleepTime=0;
            if(body.sleepTime>SleepDelaySeconds){state.sleeping=true;state.linearVelocity={};state.angularVelocity={};}
        }
        if(body.state.sleeping)++statistics_.sleeping;
    }
    error_.clear();return true;
}

bool PhysicsWorld::StepFromEnvironment(float seconds)
{
    if(!environmentDriven_||environmentStep_)return Fail("Physics.ClockOwned");
    struct Guard{bool& value;~Guard(){value=false;}} guard{environmentStep_};environmentStep_=true;return Step(seconds);
}
std::size_t PhysicsWorld::Advance(double frameSeconds)
{
    if(environmentDriven_){Fail("Physics.ClockOwned");return 0;}
    return clock_.Advance(frameSeconds,[this](double step){if(!Step(static_cast<float>(step)))throw std::runtime_error(error_);});
}

std::optional<RayHit> PhysicsWorld::Raycast(Vec3 origin,Vec3 direction,float maxDistance,std::uint32_t mask) const
{
    if(!IsFinite(origin)||!IsFinite(direction)||!IsFinite(maxDistance)||maxDistance<0
        ||!IsFinite(LengthSquared(direction))||LengthSquared(direction)<1e-10f)return std::nullopt;
    direction=Normalize(direction);std::optional<RayHit> closest;
    for(const auto handle:Bodies())
    {
        const auto& description=GetBody(handle)->description;if(!(mask&description.layer))continue;
        const auto& box=description.shape;const Vec3 local=LocalPoint(box,origin),ray=Rotate(Conjugate(box.rotation),direction);
        float enter=-std::numeric_limits<float>::infinity(),exit=std::numeric_limits<float>::infinity();Vec3 enterNormal{},exitNormal{};bool hit=true;
        for(int axis=0;axis<3;++axis)
        {
            const float coordinate=Component(local,axis),velocity=Component(ray,axis),extent=Component(box.halfExtents,axis);
            if(std::abs(velocity)<1e-8f){if(std::abs(coordinate)>extent){hit=false;break;}continue;}
            float low=(-extent-coordinate)/velocity,high=(extent-coordinate)/velocity;float sign=-1;
            if(low>high){std::swap(low,high);sign=1;}
            const Vec3 normal=axis==0?Vec3{sign,0,0}:axis==1?Vec3{0,sign,0}:Vec3{0,0,sign};
            if(low>enter){enter=low;enterNormal=normal;}
            if(high<exit){exit=high;exitNormal=-normal;}
            if(enter>exit||exit<0){hit=false;break;}
        }
        const float distance=enter>=0?enter:exit;const Vec3 point=origin+direction*distance;
        if(hit&&IsFinite(distance)&&distance>=0&&distance<=maxDistance&&IsFinite(point)&&(!closest||distance<closest->distance))
            closest=RayHit{handle,description.entityStableId,distance,point,Rotate(box.rotation,enter>=0?enterNormal:exitNormal)};
    }
    return closest;
}

std::vector<DebugLine> PhysicsWorld::DebugLines() const
{
    std::vector<DebugLine> lines;
    for(const auto handle:Bodies())
    {
        const auto& body=*GetBody(handle);const auto& box=body.description.shape;std::array<Vec3,8> vertices;
        for(unsigned i=0;i<8;++i)vertices[i]=box.center+Rotate(box.rotation,{(i&1)?box.halfExtents.x:-box.halfExtents.x,(i&2)?box.halfExtents.y:-box.halfExtents.y,(i&4)?box.halfExtents.z:-box.halfExtents.z});
        const Vec3 color=body.description.mass==0?Vec3{0.5f,0.5f,0.5f}:body.sleeping?Vec3{0.2f,0.4f,1}:Vec3{0.2f,1,0.2f};
        for(unsigned i=0;i<8;++i)for(unsigned bit=1;bit<=4;bit*=2)if(!(i&bit))lines.push_back({vertices[i],vertices[i|bit],color});
    }
    for(const auto& contact:contacts_)for(const auto& point:contact.manifold.points)
    {
        lines.push_back({point.position,point.position+contact.manifold.normal*0.2f,{1,0,0}});
        for(const auto axis:{Vec3{.04f,0,0},Vec3{0,.04f,0},Vec3{0,0,.04f}})
            lines.push_back({point.position-axis,point.position+axis,{0,1,1}});
    }
    return lines;
}
}
