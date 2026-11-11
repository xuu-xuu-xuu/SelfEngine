#include "FunctionLayer/Physics/PhysicsWorld.h"
#include <algorithm>
#include <array>
#include <cmath>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <random>
#include <set>
#include <stdexcept>

using namespace se;
namespace
{
void Check(bool value,const char* message){if(!value)throw std::runtime_error(message);}
struct D3
{
    double x=0,y=0,z=0;
    D3 operator+(D3 b)const{return {x+b.x,y+b.y,z+b.z};}
    D3 operator-(D3 b)const{return {x-b.x,y-b.y,z-b.z};}
    D3 operator*(double s)const{return {x*s,y*s,z*s};}
};
double DotD(D3 a,D3 b){return a.x*b.x+a.y*b.y+a.z*b.z;}
D3 CrossD(D3 a,D3 b){return {a.y*b.z-a.z*b.y,a.z*b.x-a.x*b.z,a.x*b.y-a.y*b.x};}
// 独立 double 四元数矩阵与八角点投影，不使用生产 Rotate/ComputeAabb/投影半径公式。
std::array<D3,8> Corners(const BoxShape& box)
{
    double x=box.rotation.x,y=box.rotation.y,z=box.rotation.z,w=box.rotation.w;
    const auto inverse=1/std::sqrt(x*x+y*y+z*z+w*w);x*=inverse;y*=inverse;z*=inverse;w*=inverse;
    const D3 colX{1-2*(y*y+z*z),2*(x*y+w*z),2*(x*z-w*y)};
    const D3 colY{2*(x*y-w*z),1-2*(x*x+z*z),2*(y*z+w*x)};
    const D3 colZ{2*(x*z+w*y),2*(y*z-w*x),1-2*(x*x+y*y)};
    std::array<D3,8> result;
    for(unsigned i=0;i<8;++i)result[i]=D3{box.center.x,box.center.y,box.center.z}
        +colX*((i&1)?box.halfExtents.x:-box.halfExtents.x)
        +colY*((i&2)?box.halfExtents.y:-box.halfExtents.y)
        +colZ*((i&4)?box.halfExtents.z:-box.halfExtents.z);
    return result;
}
double Separation(const BoxShape& a,const BoxShape& b)
{
    const auto ca=Corners(a),cb=Corners(b);
    const std::array<D3,3> ax{ca[1]-ca[0],ca[2]-ca[0],ca[4]-ca[0]},bx{cb[1]-cb[0],cb[2]-cb[0],cb[4]-cb[0]};
    double maximum=-1e30;
    for(unsigned i=0;i<15;++i)
    {
        D3 n=i<3?ax[i]:i<6?bx[i-3]:CrossD(ax[(i-6)/3],bx[(i-6)%3]);
        const double length=std::sqrt(DotD(n,n));if(length<1e-10)continue;n=n*(1/length);
        double alo=1e30,ahi=-1e30,blo=1e30,bhi=-1e30;
        for(unsigned j=0;j<8;++j){const double pa=DotD(n,ca[j]),pb=DotD(n,cb[j]);alo=std::min(alo,pa);ahi=std::max(ahi,pa);blo=std::min(blo,pb);bhi=std::max(bhi,pb);}
        maximum=std::max(maximum,std::max(blo-ahi,alo-bhi));
    }
    return maximum;
}
std::set<std::uint64_t> Features(const ContactManifold& manifold)
{std::set<std::uint64_t> result;for(const auto& p:manifold.points)result.insert(p.feature);return result;}
void Validate(const BoxShape& a,const BoxShape& b,const ContactManifold& hit)
{
    Check(IsFinite(hit.normal)&&std::abs(Length(hit.normal)-1)<.00002f,"unit finite normal");
    Check(Dot(hit.normal,b.center-a.center)>=-.00002f,"normal points A to B");
    Check(IsFinite(hit.separationDistance)&&hit.separationDistance>=0,"finite nonnegative separation distance");
    Check(!hit.points.empty()&&hit.points.size()<=4,"bounded nonempty contact set");
    Check(Features(hit).size()==hit.points.size(),"unique geometric feature per contact");
    for(const auto& p:hit.points)Check(IsFinite(p.position)&&IsFinite(p.penetration)&&p.penetration>=0,"finite contact depth and point");
    auto separated=b;separated.center+=hit.normal*(hit.separationDistance+.001f);
    Check(!CollideBoxes(a,separated,0),"translation along normal beyond separation distance eliminates contact");
    Check(Separation(a,separated)>.0007,"independent projection confirms separation translation");
    const auto again=CollideBoxes(a,b,0);Check(again&&Features(*again)==Features(hit),"repeat has identical features");
}
void FixedCases()
{
    BoxShape a,b;b.center={0,.9f,0};auto hit=CollideBoxes(a,b,0);
    Check(hit&&hit->kind==ContactKind::Face&&hit->points.size()==4,"flat support has four points");
    Check(hit->normal.y>.999f&&std::abs(hit->separationDistance-.1f)<.00001f,"support normal agrees with separation depth");
    for(const auto& p:hit->points)Check(std::abs(p.penetration-.1f)<.00001f,"flat support per point depth");
    Validate(a,b,*hit);
    const auto features=Features(*hit);b.center.x=.0001f;hit=CollideBoxes(a,b,0);
    // 轻移改变裁剪拓扑时不强制全点同编号；同拓扑内再移动应保持编号。
    const auto clipped=Features(*hit);b.center.x=.0002f;hit=CollideBoxes(a,b,0);
    Check(Features(*hit)==clipped&&features.size()==4,"tangential motion within clipping topology preserves features");
    b.center={0,1.003f,0};hit=CollideBoxes(a,b,.005f);
    Check(hit&&hit->separationDistance==0&&!CollideBoxes(a,b,0),"speculative margin distinguishes gap from penetration");
    b.center={0,0,0};b.halfExtents={.1f,.2f,.3f};hit=CollideBoxes(a,b,0);Check(bool(hit),"fully contained box contact");Validate(a,b,*hit);
    b.halfExtents={.5f,.5f,.5f};b.center={0,.9999f,0};b.rotation=AxisAngle({0,1,0},.000001f);
    hit=CollideBoxes(a,b,0);Check(bool(hit),"nearly parallel shallow support");Validate(a,b,*hit);
    a.halfExtents={2,.02f,.08f};b.halfExtents={.03f,1,.07f};b.center={.02f,.01f,.01f};
    a.rotation=AxisAngle(Normalize(Vec3{1,2,3}),.8f);b.rotation=AxisAngle(Normalize(Vec3{3,1,2}),1.1f);
    hit=CollideBoxes(a,b,0);Check(bool(hit),"different sized slender shapes");Validate(a,b,*hit);
}
void RandomCases(const std::filesystem::path& output)
{
    std::mt19937 random(5112026);std::uniform_real_distribution<float> center(-2.5f,2.5f),extent(.03f,2.0f),component(-1,1),angle(-Pi,Pi);
    unsigned overlaps=0,separate=0,edge=0,boundary=0;
    for(unsigned sample=0;sample<8192;++sample)
    {
        const auto shape=[&](){BoxShape value;value.center={center(random),center(random),center(random)};value.halfExtents={extent(random),extent(random),extent(random)};
            Vec3 axis{component(random),component(random),component(random)};if(LengthSquared(axis)<.001f)axis={1,0,0};value.rotation=AxisAngle(Normalize(axis),angle(random));return value;};
        const auto a=shape(),b=shape();const auto hit=CollideBoxes(a,b,0);const double separation=Separation(a,b);
        if(std::abs(separation)<.0002){++boundary;continue;}
        if(bool(hit)!=(separation<0)){std::cerr<<"seed=5112026 sample="<<sample<<" separation="<<separation<<'\n';Check(false,"independent double SAT agrees");}
        if(!hit){++separate;continue;}++overlaps;if(hit->kind==ContactKind::Edge)++edge;Validate(a,b,*hit);
    }
    Check(overlaps>1000&&separate>1000&&edge>100,"random coverage includes face/edge/separation");
    std::filesystem::create_directories(output.parent_path());std::ofstream file(output);
    file<<"{\"schema\":1,\"success\":true,\"seed\":5112026,\"samples\":8192,\"overlaps\":"<<overlaps<<",\"separate\":"<<separate<<",\"edge\":"<<edge<<",\"boundary_skipped\":"<<boundary<<",\"separation_slack_m\":0.001,\"oracle_boundary_m\":0.0002}\n";
    Check(bool(file),"write narrowphase proof");
    std::cout<<"Narrowphase overlaps="<<overlaps<<" separate="<<separate<<" edge="<<edge<<" boundary="<<boundary<<'\n';
}
}
int main(int argc,char** argv){try{FixedCases();RandomCases(argc>1?argv[1]:"out/validation/physics-narrowphase.json");return 0;}catch(const std::exception& error){std::cerr<<"PhysicsNarrowphaseTests failed: "<<error.what()<<'\n';return 1;}}
