#pragma once

#include "CoreLayer/Math/Bounds.h"
#include "CoreLayer/FixedStepClock.h"
#include <cstdint>
#include <optional>
#include <string>
#include <unordered_map>
#include <vector>

namespace se
{
struct BodyHandle
{
    std::uint32_t index=UINT32_MAX,generation=0;
    explicit operator bool() const {return index!=UINT32_MAX && generation!=0;}
    std::uint64_t Pack() const {return (std::uint64_t{generation}<<32)|index;}
    bool operator==(const BodyHandle&) const = default;
};
struct BoxShape {Vec3 center{};Vec3 halfExtents{0.5f,0.5f,0.5f};Quat rotation{};};
struct ContactPoint {Vec3 position{};float penetration=0;std::uint64_t feature=0;};
enum class ContactKind {Face,Edge};
struct ContactManifold
{
    Vec3 normal{};std::vector<ContactPoint> points;
    // 沿 normal 移动 B 至所选轴刚好分离的距离；逐点 penetration 用于求解。
    float separationDistance=0;
    ContactKind kind=ContactKind::Face;
};
Aabb ComputeAabb(const BoxShape& box);
std::optional<ContactManifold> CollideBoxes(const BoxShape& a,const BoxShape& b,float margin=0.005f);

struct BodyDesc
{
    BoxShape shape;
    float mass=1; // 0 表示静态。
    float friction=0.6f,restitution=0.0f;
    float linearDamping=0.02f,angularDamping=0.05f;
    std::uint32_t layer=1,mask=UINT32_MAX;
    std::uint64_t entityStableId=0; // 交给 Runtime/脚本解析，物理不拥有实体。
};
struct BodyState
{
    BodyDesc description;
    Vec3 linearVelocity{},angularVelocity{};
    bool sleeping=false;
};
struct PhysicsContact {BodyHandle a,b;ContactManifold manifold;};
struct BodyPair {BodyHandle a,b;bool operator==(const BodyPair&)const=default;};
struct ContactEvent {BodyHandle a,b;std::uint64_t entityA=0,entityB=0;bool began=false;bool ended=false;};
struct DebugLine {Vec3 from,to,color;};
struct RayHit {BodyHandle body;std::uint64_t entityStableId=0;float distance=0;Vec3 point{},normal{};};
struct PhysicsStatistics {std::size_t candidates=0,contacts=0,sleeping=0;float maximumPenetration=0;std::size_t aabbUpdates=0,aabbTests=0,cachedFeatureMatches=0,warmStartedContacts=0;};

class PhysicsWorld
{
public:
    PhysicsWorld()=default;
    PhysicsWorld(const PhysicsWorld&)=delete;
    PhysicsWorld& operator=(const PhysicsWorld&)=delete;
    PhysicsWorld(PhysicsWorld&&)=default;
    PhysicsWorld& operator=(PhysicsWorld&&)=default;
    static constexpr float MaximumLinearSpeed=100.0f;
    static constexpr float MaximumAngularSpeed=100.0f;
    static constexpr float RestitutionVelocityThreshold=1.0f; // m/s，低速支撑不反弹。
    static constexpr float PenetrationSlop=0.003f; // 米。
    static constexpr float PositionCorrectionRate=0.15f;
    static constexpr float MaximumCorrectionSpeed=2.0f; // m/s，限制偏差修正注入的速度。
    static constexpr float SleepLinearSpeed=0.04f,SleepAngularSpeed=0.05f,SleepDelaySeconds=0.7f;
    Vec3 gravity{0,-9.81f,0};
    unsigned velocityIterations=16;
    BodyHandle CreateBody(const BodyDesc& description);
    bool DestroyBody(BodyHandle body);
    bool IsAlive(BodyHandle body) const;
    const BodyState* GetBody(BodyHandle body) const;
    std::vector<BodyHandle> Bodies() const;
    bool SetVelocity(BodyHandle body,Vec3 linear,Vec3 angular={});
    bool ApplyForce(BodyHandle body,Vec3 force,Vec3 worldPoint);
    bool ApplyImpulse(BodyHandle body,Vec3 impulse,Vec3 worldPoint);
    bool Teleport(BodyHandle body,Vec3 position,Quat rotation,bool clearVelocity=true);
    bool Step(float seconds=1.0f/60.0f);
    void SetEnvironmentDriven(bool enabled)noexcept{environmentDriven_=enabled;clock_.ClearRemainder();}
    bool StepFromEnvironment(float seconds);
    std::size_t Advance(double frameSeconds);
    double DroppedSeconds() const {return clock_.DroppedSeconds();}
    std::optional<RayHit> Raycast(Vec3 origin,Vec3 direction,float maxDistance,std::uint32_t mask=UINT32_MAX) const;
    const std::vector<PhysicsContact>& Contacts() const {return contacts_;}
    const std::vector<BodyPair>& Candidates() const {return candidates_;} // 借用至下一 Step/Destroy/Teleport/World 移动。
    const std::vector<ContactEvent>& Events() const {return events_;}
    std::vector<DebugLine> DebugLines() const;
    const PhysicsStatistics& Statistics() const {return statistics_;}
    const std::string& LastError() const {return error_;}

private:
    struct Body {BodyState state;Vec3 force{},torque{},inverseInertia{};float inverseMass=0,sleepTime=0;std::uint32_t generation=0;bool alive=false;};
    struct CachedPoint {Vec3 localA{},localB{},normal{},tangentImpulse{};float normalImpulse=0;std::uint64_t feature=0;};
    struct PairKey {std::uint64_t a=0,b=0;bool operator==(const PairKey&) const=default;};
    struct PairHash {std::size_t operator()(const PairKey& p) const {return std::hash<std::uint64_t>{}(p.a)^(std::hash<std::uint64_t>{}(p.b)<<1);}};
    bool Fail(const char* key) const;
    Vec3 InverseInertia(const Body& body,Vec3 value) const;
    void AddImpulse(Body& body,Vec3 impulse,Vec3 point);
    void Wake(Body& body);
    std::vector<Body> bodies_;
    std::vector<std::uint32_t> free_;
    std::unordered_map<PairKey,std::vector<CachedPoint>,PairHash> cache_;
    std::vector<PhysicsContact> contacts_;
    std::vector<BodyPair> candidates_;
    std::vector<ContactEvent> events_;
    PhysicsStatistics statistics_;
    FixedStepClock clock_;
    bool environmentDriven_=false,environmentStep_=false;
    mutable std::string error_;
};

// 直立胶囊：position 为中心，segmentHalfHeight 为中间线段的半长。
struct CharacterController
{
    Vec3 position{0,1,0};
    float radius=0.3f,segmentHalfHeight=0.6f;
    float verticalVelocity=0,walkSpeed=4,jumpSpeed=5,maximumPushImpulse=0.5f;
    bool grounded=false;
    bool Move(PhysicsWorld& world,Vec3 horizontalDirection,bool jumpPressed,float seconds);
    std::vector<DebugLine> DebugLines() const;
};
}
