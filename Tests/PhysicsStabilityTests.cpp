#include "FunctionLayer/Physics/PhysicsWorld.h"
#include <array>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <stdexcept>

using namespace se;
namespace
{
void Check(bool value,const char* message){if(!value)throw std::runtime_error(message);}
void Tick(PhysicsWorld& world){if(!world.Step())throw std::runtime_error("physics step: "+world.LastError());}
BodyHandle Floor(PhysicsWorld& world)
{BodyDesc d;d.mass=0;d.shape.center={0,-.5f,0};d.shape.halfExtents={20,.5f,20};const auto h=world.CreateBody(d);Check(bool(h),"floor creation");return h;}
float Friction(float friction)
{
    PhysicsWorld world;Floor(world);BodyDesc d;d.shape.center={0,.5f,0};d.friction=friction;d.linearDamping=0;const auto h=world.CreateBody(d);
    Check(bool(h)&&world.SetVelocity(h,{2,0,0}),"sliding body");for(unsigned i=0;i<180;++i)Tick(world);return world.GetBody(h)->description.shape.center.x;
}
void SupportAndReuse()
{
    PhysicsWorld world;const auto floor=Floor(world);BodyDesc d;d.shape.center={0,.5f,0};const auto h=world.CreateBody(d);for(unsigned i=0;i<120;++i)Tick(world);
    Check(world.GetBody(h)->sleeping,"supported body actually sleeps");Check(world.Teleport(floor,{0,-10,0},{})&&!world.GetBody(h)->sleeping,"moving support wakes body");
    for(unsigned i=0;i<20;++i)Tick(world);Check(world.GetBody(h)->description.shape.center.y<.1f,"unsupported sleeping body falls");
    Check(world.DestroyBody(floor),"remove support");const auto replacement=Floor(world);Check(replacement.index==floor.index&&replacement.generation!=floor.generation&&!world.IsAlive(floor),"reused support has new generation");
    Check(world.Teleport(h,{0,.5f,0},{}),"reset body to replacement support");Tick(world);
    Check(world.Statistics().cachedFeatureMatches==0&&world.Statistics().warmStartedContacts==0,"replacement pair has no stale cached feature or impulse");
    Check(world.ApplyImpulse(h,{1000,0,0},world.GetBody(h)->description.shape.center)&&!world.Step()&&world.LastError()=="Physics.SpeedLimit","unsupported speed emits bounded-scope diagnostic");
}
void Range(Vec3 p,Vec3& lo,Vec3& hi)
{lo={std::min(lo.x,p.x),std::min(lo.y,p.y),std::min(lo.z,p.z)};hi={std::max(hi.x,p.x),std::max(hi.y,p.y),std::max(hi.z,p.z)};}
void Stack(const std::filesystem::path& root,float rough,float smooth)
{
    PhysicsWorld world;Floor(world);std::array<BodyHandle,20> handles;std::array<Vec3,20> initial,lo,hi,wakeLo,wakeHi;
    for(unsigned row=0;row<5;++row)for(unsigned column=0;column<4;++column)
    {
        const auto i=row*4+column;initial[i]={column&1?1.25f:-1.25f,.5f+static_cast<float>(row)*1.005f,column&2?1.25f:-1.25f};
        BodyDesc d;d.shape.center=initial[i];handles[i]=world.CreateBody(d);Check(bool(handles[i]),"stack body creation");
        lo[i]=wakeLo[i]={1e6f,1e6f,1e6f};hi[i]=wakeHi[i]={-1e6f,-1e6f,-1e6f};
    }
    std::ofstream csv(root/"steps.csv");csv<<std::setprecision(10)<<"step,time_s,phase,body,x_m,y_m,z_m,linear_speed_m_s,angular_speed_rad_s,kinetic_j,sleeping,penetration_m,feature_matches,warm_started\n";
    float penetration=0,steady=0,drift=0,maxSpeed=0,wakeMotion=0,wakeJitter=0;std::uint64_t matches=0,warm=0;std::size_t sleeping=0;Vec3 before;
    for(unsigned frame=0;frame<2400;++frame)
    {
        if(frame==1800)
        {
            sleeping=world.Statistics().sleeping;before=world.GetBody(handles[16])->description.shape.center;
            Check(sleeping>=16&&world.GetBody(handles[16])->sleeping,"stable stack sleeps before push");
            Check(world.ApplyImpulse(handles[16],{2,0,0},before)&&!world.GetBody(handles[16])->sleeping,"impulse explicitly wakes top body");
        }
        Tick(world);const auto stats=world.Statistics();
        if(frame<1800){penetration=std::max(penetration,stats.maximumPenetration);if(frame>=300)steady=std::max(steady,stats.maximumPenetration);matches+=stats.cachedFeatureMatches;warm+=stats.warmStartedContacts;}
        for(size_t i=0;i<handles.size();++i)
        {
            const auto& state=*world.GetBody(handles[i]);const auto p=state.description.shape.center;const float speed=Length(state.linearVelocity);maxSpeed=std::max(maxSpeed,speed);
            Check(IsFinite(p)&&p.y>=.45f,"stack remains above support");
            if(frame<1800){const auto delta=p-initial[i];drift=std::max(drift,Length(Vec3{delta.x,0,delta.z}));if(frame>=1500)Range(p,lo[i],hi[i]);}
            if(frame>=2280)Range(p,wakeLo[i],wakeHi[i]);
            // 单位箱主轴惯性 1/6 kg·m²，旋转各向同性。
            const double kinetic=.5*LengthSquared(state.linearVelocity)+LengthSquared(state.angularVelocity)/12.0;
            csv<<frame+1<<','<<double(frame+1)/60<<','<<(frame<1800?"baseline":"pushed")<<','<<i<<','<<p.x<<','<<p.y<<','<<p.z<<','<<speed<<','<<Length(state.angularVelocity)<<','<<kinetic<<','<<state.sleeping<<','<<stats.maximumPenetration<<','<<stats.cachedFeatureMatches<<','<<stats.warmStartedContacts<<'\n';
        }
        if(frame==1809)wakeMotion=Length(world.GetBody(handles[16])->description.shape.center-before);
    }
    float jitter=0;for(size_t i=0;i<handles.size();++i){jitter=std::max(jitter,Length(hi[i]-lo[i]));wakeJitter=std::max(wakeJitter,Length(wakeHi[i]-wakeLo[i]));}
    std::cout<<"Stack penetration="<<penetration<<" steady="<<steady<<" drift="<<drift<<" jitter="<<jitter<<" sleep="<<sleeping<<" feature="<<matches<<" warm="<<warm<<" wakeMotion="<<wakeMotion<<" wakeJitter="<<wakeJitter<<" finalSleep="<<world.Statistics().sleeping<<" speed="<<maxSpeed<<'\n';
    Check(penetration<=.15f&&steady<=.05f&&drift<=.15f&&jitter<=.05f,"declared baseline stability thresholds");
    Check(matches>0&&warm>0,"stable feature matches and actual nonzero warm starts observed");
    Check(wakeMotion>.05f&&wakeJitter<=.05f&&world.Statistics().sleeping>=16&&maxSpeed<10,"pushed stack moves then settles within declared scope");
    Check(bool(csv),"write stack trace");std::ofstream json(root/"verification.json");json<<std::setprecision(10)
        <<"{\"schema\":1,\"success\":true,\"bodies\":20,\"layers\":5,\"hz\":60,\"iterations\":16,\"baseline_seconds\":30,\"wake_seconds\":10,\"trace_rows\":48000,\"maximum_penetration_m\":"<<penetration<<",\"steady_penetration_m\":"<<steady<<",\"horizontal_drift_m\":"<<drift<<",\"last5s_range_m\":"<<jitter<<",\"sleeping_before\":"<<sleeping<<",\"feature_matches\":"<<matches<<",\"warm_starts\":"<<warm<<",\"wake_10step_motion_m\":"<<wakeMotion<<",\"wake_last2s_range_m\":"<<wakeJitter<<",\"sleeping_after\":"<<world.Statistics().sleeping<<",\"maximum_speed_m_s\":"<<maxSpeed<<",\"rough_slide_m\":"<<rough<<",\"smooth_slide_m\":"<<smooth<<"}\n";
    Check(bool(json),"write stack proof");
}
}
int main(int argc,char** argv)
{try{const auto root=std::filesystem::path(argc>1?argv[1]:"out/validation/physics-stability");std::filesystem::create_directories(root);SupportAndReuse();const auto rough=Friction(.8f),smooth=Friction(0);Check(rough<1&&smooth>5,"friction control comparison");Stack(root,rough,smooth);return 0;}catch(const std::exception& error){std::cerr<<"PhysicsStabilityTests failed: "<<error.what()<<'\n';return 1;}}
