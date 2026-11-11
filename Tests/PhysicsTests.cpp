#include "FunctionLayer/Physics/PhysicsWorld.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <iostream>
#include <limits>
#include <stdexcept>

namespace
{
using namespace se;
constexpr float Step=1.0f/60.0f;
void Require(bool condition,const char* message) {if(!condition)throw std::runtime_error(message);}
bool Near(float a,float b,float epsilon=0.001f) {return std::abs(a-b)<=epsilon;}
BodyHandle Ground(PhysicsWorld& world)
{
    BodyDesc floor;floor.mass=0;floor.shape.center={0,-0.5f,0};floor.shape.halfExtents={20,0.5f,20};
    const auto body=world.CreateBody(floor);Require(static_cast<bool>(body),"ground creation");return body;
}
BodyHandle Box(PhysicsWorld& world,Vec3 center,float restitution=0)
{
    BodyDesc description;description.shape.center=center;description.restitution=restitution;
    const auto body=world.CreateBody(description);Require(static_cast<bool>(body),"box creation");return body;
}
void Tick(PhysicsWorld& world,int count) {for(int i=0;i<count;++i)Require(world.Step(),"physics step");}

void Geometry()
{
    BoxShape a,b;b.center={0,0.9f,0};
    auto hit=CollideBoxes(a,b,0);Require(hit&&hit->points.size()==4,"face manifold must support four corners");
    Require(hit->normal.y>0.99f,"normal A to B");
    for(const auto& point:hit->points)Require(Near(point.penetration,0.1f),"face penetration");
    b.center={0,1.02f,0};Require(!CollideBoxes(a,b,0.005f),"separated boxes");
    b.center={0,1,0};Require(CollideBoxes(a,b,0).has_value(),"touching boxes");
    b.center={0,0.2f,0};Require(CollideBoxes(a,b,0)->points.front().penetration>0.7f,"deep overlap");
    b.rotation=AxisAngle({0,1,0},0.0001f);b.center={0,0.99f,0};Require(CollideBoxes(a,b).has_value(),"nearly parallel");
    a.halfExtents={1,2,0.25f};a.rotation=AxisAngle(Normalize(Vec3{1,1,1}),0.7f);
    const auto bounds=ComputeAabb(a);
    for(unsigned bits=0;bits<8;++bits)
    {
        const Vec3 vertex=a.center+Rotate(a.rotation,{(bits&1)?1.0f:-1.0f,(bits&2)?2.0f:-2.0f,(bits&4)?0.25f:-0.25f});
        Require(vertex.x>=bounds.minimum.x-0.0001f&&vertex.x<=bounds.maximum.x+0.0001f
            &&vertex.y>=bounds.minimum.y-0.0001f&&vertex.y<=bounds.maximum.y+0.0001f
            &&vertex.z>=bounds.minimum.z-0.0001f&&vertex.z<=bounds.maximum.z+0.0001f,"rotated AABB encloses all corners");
    }
    // 两根斜交细杆接近角落时必须得到边接触；分离方向仍符合几何。
    bool edgeFound=false;
    a.halfExtents={1.2f,0.15f,0.15f};a.rotation=AxisAngle(Normalize(Vec3{1,2,0}),0.7f);
    b.halfExtents={1.2f,0.15f,0.15f};b.rotation=AxisAngle(Normalize(Vec3{0,1,2}),1.2f);
    for(int x=-4;x<=4&&!edgeFound;++x)for(int y=-4;y<=4&&!edgeFound;++y)
    {
        b.center={static_cast<float>(x)*0.2f,static_cast<float>(y)*0.2f,0.2f};
        hit=CollideBoxes(a,b,0);
        if(!hit||hit->kind!=ContactKind::Edge)continue;
        const Vec3 normal=hit->normal;const float depth=hit->points[0].penetration;
        Require(IsFinite(normal)&&Near(Length(normal),1),"finite edge normal");
        b.center+=normal*(depth+0.02f);Require(!CollideBoxes(a,b,0),"edge minimum separation");edgeFound=true;
    }
    Require(edgeFound,"edge contact scenario covered");
}

void LifecycleAndIntegration()
{
    PhysicsWorld world;BodyDesc description;description.shape.center={0,20,0};description.linearDamping=0;description.angularDamping=0;
    const auto original=world.CreateBody(description);Require(world.DestroyBody(original),"destroy body");
    const auto body=world.CreateBody(description);Require(!world.IsAlive(original)&&body.index==original.index,"reused slot rejects old generation");
    Tick(world,60);const auto& state=*world.GetBody(body);
    Require(Near(state.linearVelocity.y,-9.81f,0.0001f),"gravity velocity");
    Require(Near(state.description.shape.center.y,20-9.81f*Step*Step*60*61/2,0.001f),"semi implicit position");
    Require(world.SetVelocity(body,{},{0,2,0}),"angular velocity");world.gravity={};Tick(world,600);
    Require(Near(LengthSquared(world.GetBody(body)->description.shape.rotation),1,0.00001f),"normalized long rotation");
    Require(world.ApplyImpulse(body,{2,0,0},world.GetBody(body)->description.shape.center+Vec3{0,1,0}),"off center impulse");
    Require(world.GetBody(body)->angularVelocity.z< -1,"off center angular response");
    Require(world.Teleport(body,{3,4,5},{})&&LengthSquared(world.GetBody(body)->linearVelocity)==0,"teleport resets velocities");
    world=PhysicsWorld{};const auto afterReset=world.CreateBody(description);Require(!world.IsAlive(body)&&afterReset!=body,"reset never revives handle");
    Require(!world.Step(0)&&!world.Step(0.2f),"invalid timestep rejected");
    Require(!world.SetVelocity(afterReset,{101,0,0})&&world.LastError()=="Physics.SpeedLimit","unsupported speed diagnosed");
    PhysicsWorld a,b;a.CreateBody(description);b.CreateBody(description);
    for(int i=0;i<60;++i)a.Advance(1.0/60);for(int i=0;i<30;++i)b.Advance(1.0/30);
    Require(Near(a.GetBody(a.Bodies()[0])->description.shape.center.y,b.GetBody(b.Bodies()[0])->description.shape.center.y),"display rate independent fixed steps");
    Require(a.Advance(1.0)==8&&a.DroppedSeconds()>0.8,"bounded catchup reports dropped time");
}

void ImpactFrictionAndEvents()
{
    PhysicsWorld world;Ground(world);const auto body=Box(world,{0,3,0},0.6f);
    bool collided=false,bounced=false,began=false,ended=false;float peakAfterBounce=0;
    for(int i=0;i<600;++i)
    {
        Require(world.Step(),"falling body step");const auto* state=world.GetBody(body);
        if(!world.Contacts().empty())collided=true;
        if(collided&&state->linearVelocity.y>1)bounced=true;
        if(bounced)peakAfterBounce=std::max(peakAfterBounce,state->description.shape.center.y);
        for(const auto& event:world.Events()){began|=event.began;ended|=event.ended;}
    }
    Require(collided&&bounced&&peakAfterBounce>1.0f,"real impact restitution");
    Require(began&&ended,"contact begin and separation events");
    Require(std::abs(world.GetBody(body)->description.shape.center.y-0.5f)<0.04f,"box rests on ground");
    Require(world.GetBody(body)->sleeping,"resting body sleeps");
    Require(world.ApplyImpulse(body,{2,0,0},world.GetBody(body)->description.shape.center),"wake with impulse");
    const float x=world.GetBody(body)->description.shape.center.x;Tick(world,10);
    Require(world.GetBody(body)->description.shape.center.x>x+0.05f&&!world.GetBody(body)->sleeping,"sleep is not frozen dynamics");

    PhysicsWorld offset;offset.gravity={};BodyDesc obstacle;obstacle.mass=0;obstacle.shape.center={0,0,0};offset.CreateBody(obstacle);
    const auto moving=Box(offset,{-2,0.7f,0});Require(offset.SetVelocity(moving,{4,0,0}),"impact initial velocity");
    float spin=0;for(int i=0;i<60;++i){Require(offset.Step(),"offset impact step");spin=std::max(spin,Length(offset.GetBody(moving)->angularVelocity));}
    Require(spin>0.2f,"eccentric collision causes spin");

    const auto slide=[](float friction)
    {
        PhysicsWorld scene;Ground(scene);BodyDesc box;box.shape.center={0,0.5f,0};box.friction=friction;box.linearDamping=0;
        const auto handle=scene.CreateBody(box);scene.SetVelocity(handle,{2,0,0});Tick(scene,180);
        return scene.GetBody(handle)->description.shape.center.x;
    };
    const float rough=slide(0.8f),smooth=slide(0);
    Require(rough<1.0f&&smooth>5.0f,"friction removes sliding motion");
    std::cout<<"impact bounce height="<<peakAfterBounce<<" eccentric spin="<<spin<<" friction distance="<<rough<<" smooth="<<smooth<<'\n';
}

void SupportAndFiltering()
{
    PhysicsWorld world;const auto floor=Ground(world),box=Box(world,{0,0.5f,0});Tick(world,120);
    Require(world.GetBody(box)->sleeping,"support sleep setup");
    const auto unrelated=Box(world,{5,5,5});world.DestroyBody(unrelated);world.Step();
    Require(world.Events().empty(),"unrelated deletion does not restart contact lifetime");
    Require(world.Teleport(floor,{0,-10,0},{})&&world.Step(),"move static support");
    bool ended=false;for(const auto& event:world.Events())ended|=event.ended;
    Require(ended,"teleport separation dispatches end on next step");Tick(world,19);
    Require(world.GetBody(box)->description.shape.center.y<0.1f,"moving support wakes resting body");

    PhysicsWorld filtered;filtered.gravity={};BodyDesc a,b;a.mass=0;b.mass=1;a.layer=1;a.mask=1;b.layer=2;b.mask=2;
    filtered.CreateBody(a);const auto dynamic=filtered.CreateBody(b);Require(filtered.Step()&&filtered.Contacts().empty(),"collision masks filter overlapping bodies");
    const auto ray=filtered.Raycast({0,0,-3},{0,0,1},10,2);Require(ray&&ray->body==dynamic,"ray layer mask");
    const float before=filtered.GetBody(dynamic)->description.shape.center.x;
    Require(filtered.ApplyForce(dynamic,{60,0,0},{0,1,0})&&filtered.Step(),"force and torque integration");
    Require(filtered.GetBody(dynamic)->description.shape.center.x>before&&filtered.GetBody(dynamic)->angularVelocity.z<0,"force at offset changes translation and rotation");
}

void Stack()
{
    PhysicsWorld world;Ground(world);std::array<BodyHandle,20> handles;std::array<Vec3,20> initial,minima,maxima;
    for(int column=0;column<4;++column)for(int level=0;level<5;++level)
    {
        const std::size_t i=static_cast<std::size_t>(column*5+level);
        initial[i]={column&1?1.25f:-1.25f,0.5f+static_cast<float>(level)*1.005f,column&2?1.25f:-1.25f};
        handles[i]=Box(world,initial[i]);minima[i]={1e6f,1e6f,1e6f};maxima[i]={-1e6f,-1e6f,-1e6f};
    }
    float penetration=0,drift=0;
    for(int step=0;step<1800;++step)
    {
        Require(world.Step(),"stack step");
        if(step>300)penetration=std::max(penetration,world.Statistics().maximumPenetration);
        for(std::size_t i=0;i<handles.size();++i)
        {
            const auto p=world.GetBody(handles[i])->description.shape.center;const Vec3 horizontal{p.x-initial[i].x,0,p.z-initial[i].z};
            drift=std::max(drift,Length(horizontal));
            if(step>=1500)
            {
                minima[i]={std::min(minima[i].x,p.x),std::min(minima[i].y,p.y),std::min(minima[i].z,p.z)};
                maxima[i]={std::max(maxima[i].x,p.x),std::max(maxima[i].y,p.y),std::max(maxima[i].z,p.z)};
            }
        }
    }
    float jitter=0;for(std::size_t i=0;i<handles.size();++i)jitter=std::max(jitter,Length(maxima[i]-minima[i]));
    std::cout<<"stack 20 boxes / 5 layers / 30 seconds: penetration="<<penetration<<" drift="<<drift<<" last5s range="<<jitter<<" sleeping="<<world.Statistics().sleeping<<'\n';
    Require(penetration<=0.05f,"stack penetration threshold 0.05m");Require(drift<=0.15f,"stack drift threshold 0.15m");Require(jitter<=0.05f,"stack jitter threshold 0.05m");
    const auto top=handles[4];const Vec3 before=world.GetBody(top)->description.shape.center;
    Require(world.ApplyImpulse(top,{2,0,0},before),"stack wake impulse");Tick(world,10);
    Require(Length(world.GetBody(top)->description.shape.center-before)>0.05f,"stack body responds after sleep");
}

void RayAndCharacter()
{
    PhysicsWorld world;const auto floor=Ground(world);const auto near=Box(world,{0,2,3}),far=Box(world,{0,2,6});
    (void)far;auto hit=world.Raycast({0,2,0},{0,0,3},20);Require(hit&&hit->body==near&&Near(hit->distance,2.5f),"ray chooses nearest hit");
    Require(hit->normal.z< -0.99f,"ray outward normal");Require(!world.Raycast({20,2,0},{0,0,1},10),"ray miss");
    Require(world.DestroyBody(near),"ray body deletion");hit=world.Raycast({0,2,0},{0,0,1},20);Require(hit&&hit->body==far,"ray after deletion");
    Require(world.DebugLines().size()>=24,"CPU debug box edges");
    world.DestroyBody(far);BodyDesc wall;wall.mass=0;wall.shape.center={2,2,0};wall.shape.halfExtents={0.1f,2,4};world.CreateBody(wall);
    CharacterController character;character.position={0,2,0};
    Require(!character.DebugLines().empty(),"CPU capsule debug lines");
    for(int i=0;i<120;++i)Require(character.Move(world,{},false,Step),"character fall");
    Require(character.grounded&&Near(character.position.y,0.9f,0.005f),"capsule grounded");
    for(int i=0;i<120;++i)Require(character.Move(world,{1,0,0},false,Step),"walk into thin wall");
    Require(character.position.x<=1.605f&&character.position.x>1.5f,"capsule does not cross thin wall");
    Require(character.Move(world,{},true,Step)&&!character.grounded,"ground jump");
    const float velocity=character.verticalVelocity;Require(character.Move(world,{},true,Step)&&character.verticalVelocity<velocity,"no air jump");
    float height=character.position.y;for(int i=0;i<120;++i){Require(character.Move(world,{},false,Step),"jump flight");height=std::max(height,character.position.y);}
    Require(height>1.8f&&character.grounded,"jump arcs and lands");
    character.position={0,0,0};Require(character.Move(world,{},false,Step)&&character.position.y>=0.89f,"spawn overlap resolves");
    world.DestroyBody(floor);

    PhysicsWorld push;Ground(push);const auto crate=Box(push,{1.1f,0.5f,0});CharacterController player;player.position={0,0.9f,0};
    for(int i=0;i<120;++i){Require(player.Move(push,{1,0,0},false,Step),"push character");Require(push.Step(),"push physics");}
    const auto& moved=*push.GetBody(crate);
    std::cout<<"character jump height="<<height<<" pushed box x="<<moved.description.shape.center.x<<" velocity="<<Length(moved.linearVelocity)<<" player x="<<player.position.x<<'\n';
    Require(moved.description.shape.center.x>1.3f&&Length(moved.linearVelocity)<10,"bounded character push moves crate");
}
}

int main()
{
    try {Geometry();LifecycleAndIntegration();ImpactFrictionAndEvents();SupportAndFiltering();Stack();RayAndCharacter();std::cout<<"PhysicsTests passed\n";return 0;}
    catch(const std::exception& error){std::cerr<<"PhysicsTests failed: "<<error.what()<<'\n';return 1;}
}
