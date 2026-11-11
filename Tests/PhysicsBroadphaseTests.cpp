#include "FunctionLayer/Physics/PhysicsWorld.h"
#include "FunctionLayer/Rendering/PhysicsDebugDraw.h"
#include <algorithm>
#include <array>
#include <iostream>
#include <limits>
#include <set>
#include <stdexcept>

using namespace se;
namespace
{
void Check(bool value,const char* message){if(!value)throw std::runtime_error(message);}
using Pair=std::pair<std::uint64_t,std::uint64_t>;
Pair Key(BodyHandle a,BodyHandle b){return std::minmax(a.Pack(),b.Pack());}
std::array<Vec3,8> Corners(const BoxShape& shape)
{std::array<Vec3,8> result;for(unsigned i=0;i<8;++i)result[i]=shape.center+Rotate(shape.rotation,{(i&1)?shape.halfExtents.x:-shape.halfExtents.x,(i&2)?shape.halfExtents.y:-shape.halfExtents.y,(i&4)?shape.halfExtents.z:-shape.halfExtents.z});return result;}
Aabb CornerBounds(const BoxShape& shape)
{
    const float infinity=std::numeric_limits<float>::infinity();Aabb bounds{{infinity,infinity,infinity},{-infinity,-infinity,-infinity}};
    for(const auto p:Corners(shape)){bounds.minimum={std::min(bounds.minimum.x,p.x),std::min(bounds.minimum.y,p.y),std::min(bounds.minimum.z,p.z)};bounds.maximum={std::max(bounds.maximum.x,p.x),std::max(bounds.maximum.y,p.y),std::max(bounds.maximum.z,p.z)};}return bounds;
}
bool Overlap(const Aabb& a,const Aabb& b)
{return a.minimum.x<=b.maximum.x+.005f&&a.maximum.x+.005f>=b.minimum.x&&a.minimum.y<=b.maximum.y+.005f&&a.maximum.y+.005f>=b.minimum.y&&a.minimum.z<=b.maximum.z+.005f&&a.maximum.z+.005f>=b.minimum.z;}
std::set<Pair> Reference(const PhysicsWorld& physics,std::size_t& tests)
{
    std::set<Pair> result;const auto handles=physics.Bodies();tests=0;
    for(size_t i=0;i<handles.size();++i)for(size_t j=i+1;j<handles.size();++j)
    {const auto& a=physics.GetBody(handles[i])->description;const auto& b=physics.GetBody(handles[j])->description;if((a.mass==0&&b.mass==0)||!(a.mask&b.layer)||!(b.mask&a.layer))continue;++tests;if(Overlap(CornerBounds(a.shape),CornerBounds(b.shape)))result.insert(Key(handles[i],handles[j]));}return result;
}
void Compare(PhysicsWorld& physics)
{
    std::size_t tests=0;const auto expected=Reference(physics,tests);const auto count=physics.Bodies().size();Check(physics.Step(),"broadphase step");std::set<Pair> actual;
    for(const auto pair:physics.Candidates()){Check(physics.IsAlive(pair.a)&&physics.IsAlive(pair.b),"candidate handles are alive");actual.insert(Key(pair.a,pair.b));}
    Check(actual==expected&&actual.size()==physics.Candidates().size(),"candidate set exactly matches independent corner-AABB brute force without duplicates");
    Check(physics.Statistics().aabbUpdates==count&&physics.Statistics().aabbTests==tests&&physics.Statistics().candidates==expected.size(),"one AABB per body and exact filtered comparison cost");
}
void Test()
{
    std::uint32_t seed=424242;const auto random=[&](float low,float high){seed=seed*1664525u+1013904223u;return low+(high-low)*static_cast<float>(seed>>8)/16777216.f;};
    for(unsigned scene=0;scene<32;++scene)
    {
        PhysicsWorld physics;physics.gravity={};std::vector<BodyHandle> handles;
        for(unsigned i=0;i<32;++i)
        {
            BodyDesc body;body.mass=i%4?1.f:0.f;body.layer=1u<<(i%3);body.mask=i%5?UINT32_MAX:0;body.linearDamping=body.angularDamping=0;
            body.shape.center={random(-3,3),random(-3,3),random(-3,3)};body.shape.halfExtents={random(.2f,.8f),random(.2f,.8f),random(.2f,.8f)};
            body.shape.rotation=AxisAngle(Normalize(Vec3{random(-1,1),random(.1f,1),random(-1,1)}),random(-Pi,Pi));const auto handle=physics.CreateBody(body);Check(bool(handle),"random body");handles.push_back(handle);
            const auto& shape=physics.GetBody(handle)->description.shape;const auto bounds=ComputeAabb(shape);
            for(const auto p:Corners(shape))Check(p.x>=bounds.minimum.x-1e-5f&&p.x<=bounds.maximum.x+1e-5f&&p.y>=bounds.minimum.y-1e-5f&&p.y<=bounds.maximum.y+1e-5f&&p.z>=bounds.minimum.z-1e-5f&&p.z<=bounds.maximum.z+1e-5f,"rotated AABB encloses every independently transformed corner");
        }
        Compare(physics);const auto retired=handles[scene%handles.size()];Check(physics.DestroyBody(retired),"delete collider");
        for(const auto pair:physics.Candidates())Check(pair.a!=retired&&pair.b!=retired&&physics.IsAlive(pair.a)&&physics.IsAlive(pair.b),"delete immediately removes stale candidate handles");
        BodyDesc replacement;replacement.shape.center={0,0,0};const auto next=physics.CreateBody(replacement);Check(next.index==retired.index&&next!=retired,"replacement uses new generation");
        Check(physics.Teleport(next,{2,2,2},AxisAngle({0,1,0},.7f)),"collider transform update");Compare(physics);
    }
    for(unsigned i=0;i<4096;++i)
    {
        DebugLine line{{random(-10,10),random(-10,10),random(-10,10)},{random(-10,10),random(-10,10),random(-10,10)},{.2f,1,.2f}};Mat4 matrix;
        Check(BuildDebugSegmentTransform(line,.015f,matrix)&&Length(TransformPoint(matrix,{-.5f,0,0})-line.from)<1e-4f&&Length(TransformPoint(matrix,{.5f,0,0})-line.to)<1e-4f,"GPU segment cube endpoints map to requested world endpoints");
    }
    Mat4 unchanged;const auto before=unchanged.values;Check(!BuildDebugSegmentTransform({{1,2,3},{1,2,3},{1,1,1}},.015f,unchanged)&&unchanged.values==before,"zero segment rejected atomically");
    DebugLine invalid;invalid.to={std::numeric_limits<float>::quiet_NaN(),0,0};Check(!BuildDebugSegmentTransform(invalid,.015f,unchanged)&&unchanged.values==before,"nonfinite segment rejected atomically");
}
}
int main(){try{Test();std::cout<<"PhysicsBroadphaseTests passed (32 worlds, 64 exact candidate comparisons, corner bounds, deletion/reuse/teleport, 4096 GPU segment transforms).\n";return 0;}catch(const std::exception& error){std::cerr<<"PhysicsBroadphaseTests failed: "<<error.what()<<'\n';return 1;}}
