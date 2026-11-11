#include "FunctionLayer/Physics/PhysicsWorld.h"
#include <algorithm>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <stdexcept>
#include <string>
#include <vector>

using namespace se;
namespace
{
void Check(bool value,const char* message){if(!value)throw std::runtime_error(message);}
double Kinetic(const BodyState& state)
{
    const auto& d=state.description;const auto h=d.shape.halfExtents;
    const auto w=Rotate(Conjugate(d.shape.rotation),state.angularVelocity);
    // 独立长方体主轴惯性 I=m/12*(边长²+边长²)。
    const double ix=d.mass/3.0*(h.y*h.y+h.z*h.z),iy=d.mass/3.0*(h.x*h.x+h.z*h.z),iz=d.mass/3.0*(h.x*h.x+h.y*h.y);
    return .5*d.mass*LengthSquared(state.linearVelocity)+.5*(ix*w.x*w.x+iy*w.y*w.y+iz*w.z*w.z);
}
BodyHandle Add(PhysicsWorld& world,Vec3 center,float mass,float restitution,float friction=0)
{
    BodyDesc d;d.shape.center=center;d.mass=mass;d.restitution=restitution;d.friction=friction;d.linearDamping=d.angularDamping=0;
    if(mass==0&&center.y<0)d.shape.halfExtents={20,.5f,20};const auto handle=world.CreateBody(d);Check(bool(handle),"create solver body");return handle;
}
struct Summary
{
    std::string name;unsigned hz=0,iterations=0,steps=0;double maximumEnergy=0,maximumPenetration=0,maximumRestKinetic=0,finalHeight=0,spin=0,momentumError=0,bounceHeight=0;
};
Summary Run(const std::string& name,unsigned hz,unsigned iterations,std::ofstream& csv)
{
    PhysicsWorld world;world.velocityIterations=iterations;const float dt=1.0f/static_cast<float>(hz);std::vector<BodyHandle> dynamic;
    double initialEnergy=0;bool contact=false,bounced=false;Summary s;s.name=name;s.hz=hz;s.iterations=iterations;
    if(name=="drop"||name=="rest")
    {Add(world,{0,-.5f,0},0,0,.6f);dynamic.push_back(Add(world,{0,name=="drop"?3.0f:.5f,0},1,name=="drop"?.6f:0,.6f));initialEnergy=name=="drop"?9.81*2.5:0;}
    else
    {
        world.gravity={};
        if(name=="head_on")
        {dynamic.push_back(Add(world,{-1.5f,0,0},1,.5f));dynamic.push_back(Add(world,{1.5f,0,0},1,.5f));Check(world.SetVelocity(dynamic[0],{2,0,0})&&world.SetVelocity(dynamic[1],{-2,0,0}),"head-on velocity");initialEnergy=4;}
        else {Add(world,{},0,.2f);dynamic.push_back(Add(world,{-2,.7f,0},1,.2f));Check(world.SetVelocity(dynamic[0],{4,0,0}),"eccentric velocity");initialEnergy=8;}
    }
    s.maximumEnergy=initialEnergy;
    for(unsigned step=1;step<=hz*6;++step)
    {
        Check(world.Step(dt),"solver simulation step");++s.steps;contact|=!world.Contacts().empty();
        double energy=0,kinetic=0;Vec3 momentum{};
        for(const auto handle:dynamic)
        {
            const auto& state=*world.GetBody(handle);const auto& p=state.description.shape.center;const double k=Kinetic(state);
            const double potential=(name=="drop"||name=="rest")?state.description.mass*9.81*(p.y-.5):0;
            kinetic+=k;energy+=k+potential;momentum+=state.linearVelocity*state.description.mass;
            s.spin=std::max(s.spin,static_cast<double>(-state.angularVelocity.z));
            if(name=="drop"&&contact&&state.linearVelocity.y>1)bounced=true;
            if(name=="drop"&&bounced)s.bounceHeight=std::max(s.bounceHeight,static_cast<double>(p.y));
            csv<<name<<','<<hz<<','<<iterations<<','<<step<<','<<double(step)/hz<<','<<handle.index<<','<<p.x<<','<<p.y<<','<<p.z<<','
                <<state.linearVelocity.x<<','<<state.linearVelocity.y<<','<<state.linearVelocity.z<<','<<state.angularVelocity.x<<','<<state.angularVelocity.y<<','<<state.angularVelocity.z<<','
                <<k<<','<<potential<<','<<k+potential<<','<<world.Statistics().maximumPenetration<<','<<state.sleeping<<','<<world.Statistics().contacts<<'\n';
        }
        Check(std::isfinite(energy)&&std::isfinite(kinetic),"finite energy");s.maximumEnergy=std::max(s.maximumEnergy,energy);
        s.maximumPenetration=std::max(s.maximumPenetration,static_cast<double>(world.Statistics().maximumPenetration));
        if(step>hz*3)s.maximumRestKinetic=std::max(s.maximumRestKinetic,kinetic);
        if(name=="head_on")s.momentumError=std::max(s.momentumError,static_cast<double>(Length(momentum)));
    }
    s.finalHeight=world.GetBody(dynamic[0])->description.shape.center.y;
    Check(contact,"scene actually exercises collision");
    if(name=="drop")
    {
        Check(bounced&&s.bounceHeight>1&&s.bounceHeight<2,"bounded restitution bounce");
        Check(s.maximumPenetration<9*dt+.01,"drop penetration bounded by discrete timestep contract");
        Check(std::abs(s.finalHeight-.5)<.015,"drop settles on support");
    }
    if(name=="head_on")
    {
        Check(s.momentumError<.001,"equal and opposite impulses preserve momentum");
        Check(std::abs(world.GetBody(dynamic[0])->linearVelocity.x+1)<.03f&&std::abs(world.GetBody(dynamic[1])->linearVelocity.x-1)<.03f,"two-body restitution speeds");
    }
    if(name=="eccentric")Check(s.spin>.2,"off-center impact gives expected angular direction");
    if(name=="rest")Check(s.maximumRestKinetic<.005&&std::abs(s.finalHeight-.5)<.015,"stationary support does not keep gaining energy");
    else Check(s.maximumEnergy<=initialEnergy*1.1,"impact energy bounded by declared correction tolerance");
    return s;
}
void Thresholds()
{
    for(const float speed:{.5f,2.0f})
    {
        PhysicsWorld world;world.gravity={};Add(world,{},0,1);const auto moving=Add(world,{1,0,0},1,1);Check(world.SetVelocity(moving,{-speed,0,0})&&world.Step(),"threshold test step");
        const auto vx=world.GetBody(moving)->linearVelocity.x;
        if(speed<PhysicsWorld::RestitutionVelocityThreshold)Check(std::abs(vx)<.005,"below threshold contact does not rebound");
        else Check(std::abs(vx-speed)<.005,"above threshold elastic rebound");
    }
    PhysicsWorld world;world.gravity={};Add(world,{},0,0);const auto moving=Add(world,{0,.05f,0},1,0);Check(world.Step(),"deep correction step");
    Check(Length(world.GetBody(moving)->linearVelocity)<=PhysicsWorld::MaximumCorrectionSpeed+.01f,"deep penetration correction speed capped");
}
}
int main(int argc,char** argv)
{
    try
    {
        const auto root=std::filesystem::path(argc>1?argv[1]:"out/validation/physics-solver");std::filesystem::create_directories(root);
        std::ofstream csv(root/"steps.csv");csv<<std::setprecision(10)<<"scene,hz,iterations,step,time_s,body,x_m,y_m,z_m,vx_m_s,vy_m_s,vz_m_s,wx_rad_s,wy_rad_s,wz_rad_s,kinetic_j,potential_j,total_j,penetration_m,sleeping,contact_points\n";
        Thresholds();std::vector<Summary> summaries;
        for(const unsigned hz:{30u,60u,120u})for(const unsigned iterations:{8u,16u,32u})for(const auto name:{"drop","head_on","eccentric","rest"})
        {std::cout<<"Solver "<<name<<" hz="<<hz<<" iterations="<<iterations<<std::endl;summaries.push_back(Run(name,hz,iterations,csv));}
        Check(bool(csv),"write solver trace");std::ofstream json(root/"verification.json");json<<std::setprecision(10)<<"{\"schema\":1,\"success\":true,\"records\":[";
        bool first=true;for(const auto& s:summaries)
        {if(!first)json<<',';first=false;json<<"{\"scene\":\""<<s.name<<"\",\"hz\":"<<s.hz<<",\"iterations\":"<<s.iterations<<",\"steps\":"<<s.steps<<",\"maximum_energy_j\":"<<s.maximumEnergy<<",\"maximum_penetration_m\":"<<s.maximumPenetration<<",\"rest_kinetic_j\":"<<s.maximumRestKinetic<<",\"final_height_m\":"<<s.finalHeight<<",\"negative_z_spin_rad_s\":"<<s.spin<<",\"momentum_error\":"<<s.momentumError<<",\"bounce_height_m\":"<<s.bounceHeight<<'}';}
        json<<"]}\n";Check(bool(json),"write solver proof");std::cout<<"PhysicsSolverTests passed (36 traces, thresholds and correction cap).\n";return 0;
    }
    catch(const std::exception& error){std::cerr<<"PhysicsSolverTests failed: "<<error.what()<<'\n';return 1;}
}
