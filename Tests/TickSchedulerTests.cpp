#include "FunctionLayer/Runtime/Environment.h"
#include "FunctionLayer/Physics/PhysicsWorld.h"
#include <nlohmann/json.hpp>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <stdexcept>

using namespace se;
namespace
{
void Check(bool condition,const char* message){if(!condition)throw std::runtime_error(message);}
ComponentHandle Component(World& world,std::uint64_t id)
{return world.AttachScriptComponent(world.Create("tick component"),id);}
TickToken Add(Environment& env,ComponentHandle component,std::string key,TickStage stage,std::function<void(const TickContext&)> callback,
    std::vector<TickPrerequisite> dependencies={},bool paused=false,TickTimeDomain time=TickTimeDomain::Frame)
{
    const auto token=env.Ticks().Register(env.GetWorld(),{std::move(key),component,stage,time,paused,std::move(dependencies),std::move(callback)});
    Check(token!=0,"tick registration");return token;
}
}
int main(int argc,char** argv)
{
    try
    {
        Environment env;auto& world=env.GetWorld();const auto a=Component(world,1),b=Component(world,2);
        std::vector<std::string> trace;
        auto callback=[&](const char* name){return [&,name](const TickContext&){trace.push_back(name);};};
        const auto ap=Add(env,a,"a.pre",TickStage::PreTick,callback("a.pre"));
        const auto bp=Add(env,b,"b.pre",TickStage::PreTick,callback("b.pre"),{{ap,TickDependencyMode::Fresh}});
        Check(!env.Ticks().SetPrerequisites(ap,{{bp,TickDependencyMode::OrderOnly}})&&env.Ticks().LastError()=="Tick.DependencyCycle","cycle atomic rejection");
        const auto af=Add(env,a,"a.fixed",TickStage::FixedPreTick,callback("a.fixed"));
        Add(env,b,"b.fixed",TickStage::FixedPreTick,callback("b.fixed"),{{af,TickDependencyMode::Fresh}});
        const auto ax=Add(env,a,"a.result",TickStage::FixedPostTick,callback("a.result"),{{af,TickDependencyMode::Fresh}});
        Add(env,b,"b.result",TickStage::FixedPostTick,callback("b.result"),{{ax,TickDependencyMode::Fresh}});
        const auto at=Add(env,a,"a.tick",TickStage::Tick,[&](const TickContext&){trace.push_back("a.tick");auto pose=*world.GetTransform(world.GetComponent(a)->Owner());pose.position.x=9;Check(world.SetTransform(world.GetComponent(a)->Owner(),pose),"write phase property");});
        Add(env,b,"b.tick",TickStage::Tick,[&](const TickContext&){Check(world.GetTransform(world.GetComponent(a)->Owner())->position.x==9,"same phase prerequisite sees committed candidate");trace.push_back("b.tick");},{{at,TickDependencyMode::Fresh}});
        Add(env,a,"a.post",TickStage::PostTick,callback("a.post"));Add(env,b,"b.post",TickStage::PostTick,callback("b.post"));
        Check(!env.Ticks().SetPrerequisites(ap,{{at,TickDependencyMode::OrderOnly}})&&env.Ticks().LastError()=="Tick.FutureDependency","future stage atomic rejection");
        int physics=0;EnvironmentHooks hooks;
        hooks.physics=[&](double dt){Check(std::abs(dt-1.0/60)<1e-12,"fixed step units");++physics;trace.push_back("physics");return true;};
        hooks.synchronize=[&]{trace.push_back("sync");return true;};
        hooks.phase=[&](EnvironmentPhase phase,const TickContext&){
            if(phase==EnvironmentPhase::FrameBegin)trace.push_back("control");
            if(phase==EnvironmentPhase::BeforePhysics)trace.push_back("before");
            if(phase==EnvironmentPhase::AfterPhysics)trace.push_back("after");
            if(phase==EnvironmentPhase::AfterLogic)trace.push_back("logic");
            if(phase==EnvironmentPhase::FrameEnd)trace.push_back("end");return true;};
        Check(env.RunFrame(1.0/30,hooks)&&physics==2,"two fixed steps");
        const std::vector<std::string> expected{"control","a.pre","b.pre","a.fixed","b.fixed","before","physics","sync","after","a.result","b.result","a.fixed","b.fixed","before","physics","sync","after","a.result","b.result","a.tick","b.tick","logic","sync","a.post","b.post","sync","end"};
        Check(trace==expected,"global stage sequence");
        const auto savedTrace=trace;
        int oldReads=0,freshReads=0;
        Add(env,a,"old.result",TickStage::PostTick,[&](const TickContext&){++oldReads;},{{ax,TickDependencyMode::OrderOnly}});
        Add(env,b,"fresh.result",TickStage::PostTick,[&](const TickContext&){++freshReads;},{{ax,TickDependencyMode::Fresh}});
        Check(env.RunFrame(.001,hooks)&&oldReads==1&&freshReads==0&&physics==2,"zero fixed steps retain old data but reject fresh data");
        Check(!env.Ticks().Diagnostics().empty()&&env.Ticks().Diagnostics().back().key=="Tick.FreshDataUnavailable","fresh skip observable");
        Check(world.SetComponentEnabled(a,false)&&env.RunFrame(1.0/60,hooks)&&oldReads==1&&freshReads==0,"disabled producer cannot fabricate fresh data");
        Check(world.SetComponentEnabled(a,true),"enable producer");
        Environment budget;int count=0;EnvironmentHooks budgetHooks;budgetHooks.physics=[&](double){++count;return true;};
        Check(budget.RunFrame(1,budgetHooks)&&count==8&&budget.Clock().TotalSteps()==8&&std::abs(budget.Clock().DroppedSeconds()-52.0/60)<1e-9,"single native clock drops 52 steps");
        Check(budget.RunFrame(0,budgetHooks)&&count==8&&budget.RunFrame(1.0/60,budgetHooks)&&count==9,"no replay after budget drop");
        Check(budget.RunFrame(.008,budgetHooks),"fraction before pause");budget.SetPaused(true);
        Check(budget.RunFrame(1,budgetHooks)&&count==9,"pause stops physics");budget.SetPaused(false);
        Check(budget.RunFrame(.009,budgetHooks)&&count==9&&budget.Clock().TotalSteps()==9,"pause discards prior fraction but preserves statistics");
        const auto ui=Component(budget.GetWorld(),99);int controls=0;double uiDelta=0;
        Add(budget,ui,"ui.control",TickStage::FrameBegin,[&](const TickContext& context){++controls;uiDelta+=context.deltaSeconds;}, {},true);
        budget.SetPaused(true);Check(budget.RunFrame(.1,budgetHooks)&&controls==1&&std::abs(uiDelta-.1)<1e-12&&count==9,"paused frame time domain remains available");
        Environment mutation;auto& changing=mutation.GetWorld();const auto m=Component(changing,100),victim=Component(changing,101);EntityHandle born;ComponentHandle bornComponent;int victimCalls=0,bornCalls=0,lateCalls=0;bool requested=false;
        Add(mutation,m,"mutate",TickStage::PreTick,[&](const TickContext&){
            if(requested)return;requested=true;born=changing.Create("next frame");Check(!changing.IsAlive(born),"create reserved for next frame");
            Check(changing.SetTransform(born,{{3,4,5},{},{1,1,1}}),"reserved property order");
            Check(changing.Destroy(changing.GetComponent(victim)->Owner())&&!changing.GetComponent(victim),"destroy immediately invalidates target");
            Add(mutation,m,"late",TickStage::Tick,[&](const TickContext&){++lateCalls;});
            Check(!changing.FlushCommands()&&!mutation.ConfigureClock(.01,4),"recursive commit/clock rejected");});
        Add(mutation,victim,"victim",TickStage::Tick,[&](const TickContext&){++victimCalls;});
        Check(mutation.RunFrame(1.0/60)&&!changing.IsAlive(born)&&victimCalls==0&&lateCalls==0,"new objects/tasks wait, destroyed tasks skip");
        EnvironmentHooks activate;activate.phase=[&](EnvironmentPhase phase,const TickContext&){
            if(phase==EnvironmentPhase::Prepare&&!bornComponent){Check(changing.IsAlive(born)&&changing.GetTransform(born)->position.y==4,"next frame commits queued properties");bornComponent=changing.AttachScriptComponent(born,102);}
            return true;};
        Check(mutation.RunFrame(1.0/60,activate)&&lateCalls==1,"existing queued task enters next frame");
        Check(mutation.RunFrame(1.0/60)&&changing.GetComponent(bornComponent),"new component attaches next frame");
        Add(mutation,bornComponent,"born",TickStage::Tick,[&](const TickContext&){++bornCalls;});
        Check(mutation.RunFrame(0)&&bornCalls==1,"new component safely participates");
        Environment manual;int manualSteps=0;EnvironmentHooks manualHooks;manualHooks.physics=[&](double){++manualSteps;return true;};
        Check(!manual.RunFixed(1.0/60,manualHooks)&&manual.ConfigureClock(1.0/60,8,false)&&manual.RunFixed(1.0/60,manualHooks)&&manualSteps==1,"manual and automatic clock modes exclusive");
        Check(manual.RunFrame(1,manualHooks)&&manualSteps==1,"manual mode never automatically advances");
        PhysicsWorld driven;driven.SetEnvironmentDriven(true);
        Check(!driven.Step()&&driven.LastError()=="Physics.ClockOwned"&&driven.Advance(1.0/60)==0&&driven.LastError()=="Physics.ClockOwned","lower physics API rejects a second driver");
        Check(driven.StepFromEnvironment(1.0f/60),"owned physics single step");driven.SetEnvironmentDriven(false);Check(driven.Step(),"driver release restores direct stepping");
        Environment faults;const auto bad=Component(faults.GetWorld(),300),healthy=Component(faults.GetWorld(),301);int healthyCalls=0;
        Add(faults,bad,"fault",TickStage::Tick,[](const TickContext&){throw std::runtime_error("intentional");});
        Add(faults,healthy,"healthy",TickStage::Tick,[&](const TickContext&){++healthyCalls;});
        Check(faults.RunFrame(0)&&healthyCalls==1&&!faults.GetWorld().GetComponent(bad)->Enabled()&&faults.Ticks().Diagnostics().back().key=="Tick.CallbackFailed","callback fault isolated");
        const std::filesystem::path proof=argc>1?argv[1]:"out/tick-scheduler/verification.json";std::filesystem::create_directories(proof.parent_path());
        std::ofstream(proof)<<nlohmann::json{{"success",true},{"globalOrder",savedTrace},{"cycleRejected",true},{"futureRejected",true},{"zeroFixed",true},{"oldDataAllowed",true},{"freshDataSkipped",true},{"budgetSteps",8},{"droppedSteps",52},{"pauseNoReplay",true},{"frameTimeUi",true},{"deferredCreate",true},{"immediateDestroy",true},{"deferredTask",true},{"manualExclusive",true},{"callbackFault",true}}.dump(2);
        std::cout<<"TickScheduler passed (phases, dependencies, clock, pause and structural boundaries)\n";return 0;
    }
    catch(const std::exception& exception){std::cerr<<"[FAIL] "<<exception.what()<<'\n';return 1;}
}
