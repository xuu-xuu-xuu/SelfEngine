#include "FunctionLayer/Physics/PhysicsWorld.h"
#include <algorithm>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <limits>
#include <random>
#include <stdexcept>

using namespace se;
namespace
{
void Check(bool value,const char* message){if(!value)throw std::runtime_error(message);}
BodyHandle Add(PhysicsWorld& world,Vec3 center,Vec3 half,Quat rotation={},std::uint32_t layer=1,std::uint64_t stable=0)
{BodyDesc d;d.mass=0;d.shape={center,half,rotation};d.layer=layer;d.entityStableId=stable;const auto h=world.CreateBody(d);Check(bool(h),"create static query body");return h;}
void FailureAtomicity()
{
    PhysicsWorld world;CharacterController character;character.position={0,.9f,0};character.grounded=true;character.walkSpeed=1e8f;
    const auto before=character;Check(!character.Move(world,{1,0,0},true,1.0f/60),"over-budget move rejected");
    Check(Length(character.position-before.position)==0&&character.verticalVelocity==before.verticalVelocity&&character.grounded==before.grounded,"rejected over-budget move preserves jump, ground and velocity state");
}
void Rays()
{
    std::mt19937 random(5412026);std::uniform_real_distribution<float> center(-10,10),extent(.1f,2),component(-1,1),angle(-Pi,Pi);
    for(unsigned sample=0;sample<4096;++sample)
    {
        PhysicsWorld world;const Vec3 p{center(random),center(random),center(random)},h{extent(random),extent(random),extent(random)};
        Vec3 axis{component(random),component(random),component(random)};if(LengthSquared(axis)<.001f)axis={1,0,0};const auto q=AxisAngle(Normalize(axis),angle(random));
        const unsigned dimension=sample%3;const float sign=sample&1?1.0f:-1.0f;
        const Vec3 unit=dimension==0?Vec3{sign,0,0}:dimension==1?Vec3{0,sign,0}:Vec3{0,0,sign};
        const float half=dimension==0?h.x:dimension==1?h.y:h.z;const auto normal=Rotate(q,unit),direction=-normal;
        const auto near=Add(world,p,h,q,1,sample+1);const auto far=Add(world,p+direction*10,h,q,2,sample+8193);
        const auto origin=p+normal*(half+2),surface=p+normal*half;
        auto hit=world.Raycast(origin,direction*3,30);Check(hit&&hit->body==near&&hit->entityStableId==sample+1,"nearest hit and entity metadata");
        Check(std::abs(hit->distance-2)<.00005f&&Length(hit->point-surface)<.00005f&&Dot(hit->normal,normal)>.99999f,"rotated known local face distance, point and outward normal");
        hit=world.Raycast(p,normal,30);Check(hit&&hit->body==near&&std::abs(hit->distance-half)<.00005f&&Dot(hit->normal,normal)>.99999f,"inside origin returns forward exit surface with outward normal");
        Check(!world.Raycast(p,normal,half-.01f),"inside exit respects maximum distance");
        hit=world.Raycast(origin,direction,30,2);Check(hit&&hit->body==far&&std::abs(hit->distance-12)<.00005f,"mask chooses farther layer");
        Check(world.DestroyBody(near),"remove nearest body");hit=world.Raycast(origin,direction,30);Check(hit&&hit->body==far,"deleted hit cannot leak stale body handle");
    }
    PhysicsWorld world;Add(world,{}, {.5f,.5f,.5f});auto hit=world.Raycast({.5f,0,0},{-1,0,0},1);
    Check(hit&&hit->distance==0&&hit->normal.x>.999f,"surface entry at zero has valid normal");
    hit=world.Raycast({.5f,0,0},{1,0,0},1);Check(hit&&hit->distance==0&&hit->normal.x>.999f,"surface outward exit at zero");
    Check(!world.Raycast({}, {},1)&&!world.Raycast({}, {std::numeric_limits<float>::max(),0,0},1),"zero and overflow norm ray directions rejected");
    Check(!world.Raycast({std::numeric_limits<float>::quiet_NaN(),0,0},{1,0,0},1),"nonfinite ray origin rejected");
}
void Move(CharacterController& character,PhysicsWorld& world,Vec3 direction,bool jump,float dt)
{Check(character.Move(world,direction,jump,dt),"bounded character move");}
void Characters(std::ofstream& json)
{
    bool first=true;
    for(const unsigned hz:{30u,60u,120u})
    {
        const float dt=1.0f/static_cast<float>(hz);PhysicsWorld world;Add(world,{0,-.5f,0},{20,.5f,20});Add(world,{2,2,0},{.01f,2,20});
        CharacterController c;c.position={0,2,0};for(unsigned i=0;i<hz*2;++i)Move(c,world,{},false,dt);
        Check(c.grounded&&std::abs(c.position.y-.9f)<.003f,"land on floor with declared capsule height");
        for(unsigned i=0;i<hz*3;++i)Move(c,world,{1,0,.5f},false,dt);
        Check(c.position.x<=1.6902f&&c.position.x>1.68f&&c.position.z>5,"thin wall constrains normal movement and allows tangent slide");
        float lo=1e6f,hi=-1e6f;for(unsigned i=0;i<hz;++i){Move(c,world,{1,0,0},false,dt);lo=std::min(lo,c.position.x);hi=std::max(hi,c.position.x);}
        Check(hi-lo<=.005f&&c.grounded,"wall pushing remains grounded without jitter");
        c.position={0,.9f,0};c.grounded=true;Move(c,world,{},true,dt);const float firstVelocity=c.verticalVelocity;Move(c,world,{},true,dt);
        Check(c.verticalVelocity<firstVelocity,"second air press does not refresh jump velocity");float peak=c.position.y;
        for(unsigned i=2;i<hz*2;++i){Move(c,world,{},false,dt);peak=std::max(peak,c.position.y);}
        Check(peak>2&&peak<2.3f&&c.grounded,"bounded jump arc returns to ground");
        c.position={0,0,0};c.verticalVelocity=0;Move(c,world,{},false,dt);Check(c.position.y>=.899f,"spawn overlapping floor resolves upwards");
        c.position={2,.9f,0};Move(c,world,{},false,dt);Check(std::abs(c.position.x-2)>=.309f,"spawn inside thin wall resolves outside");
        PhysicsWorld ceiling;Add(ceiling,{0,-.5f,0},{20,.5f,20});Add(ceiling,{0,2.5f,0},{20,.1f,20});
        c={};c.position={0,.9f,0};c.grounded=true;Move(c,ceiling,{},true,dt);float head=0;
        for(unsigned i=0;i<hz*2;++i){Move(c,ceiling,{},false,dt);head=std::max(head,c.position.y+.9f);}
        Check(head<=2.401f&&c.grounded,"low ceiling blocks upward movement and character lands");
        if(!first)json<<',';first=false;json<<"{\"hz\":"<<hz<<",\"jump_peak_m\":"<<peak<<",\"wall_range_m\":"<<hi-lo<<",\"head_peak_m\":"<<head<<'}';
    }
    for(const float radius:{.15f,.5f})
    {
        PhysicsWorld world;Add(world,{0,-.5f,0},{20,.5f,20});const auto q=AxisAngle({0,1,0},.4f);Add(world,{0,2,0},{.01f,2,20},q);
        const auto normal=Rotate(q,{1,0,0});CharacterController c;c.radius=radius;c.segmentHalfHeight=radius*2;c.position=-normal*2;c.position.y=radius*3;c.walkSpeed=8;
        for(unsigned i=0;i<120;++i)Move(c,world,normal,false,1.0f/30);
        Check(Dot(c.position,normal)<=-.01f-radius+.001f&&c.grounded,"varied capsule size and rotated thin wall at 8 m/s");
    }
}
}
int main(int argc,char** argv)
{try{const auto root=std::filesystem::path(argc>1?argv[1]:"out/validation/character-motion");std::filesystem::create_directories(root);FailureAtomicity();Rays();std::ofstream json(root/"verification.json");json<<std::setprecision(10)<<"{\"schema\":1,\"success\":true,\"ray_seed\":5412026,\"ray_cases\":4096,\"radius_m\":0.3,\"segment_half_height_m\":0.6,\"walk_m_s\":4,\"jump_m_s\":5,\"records\":[";Characters(json);json<<"],\"varied_sizes\":2,\"failure_preserves_state\":true}\n";Check(bool(json),"write character evidence");std::cout<<"CharacterMotionTests passed (ray surfaces, wall slide, jump, overlap, ceiling, sizes and failure state).\n";return 0;}catch(const std::exception& error){std::cerr<<"CharacterMotionTests failed: "<<error.what()<<'\n';return 1;}}
