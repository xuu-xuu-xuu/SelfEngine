#include "ToolLayer/Editor/EditorSession.h"
#include "FunctionLayer/Application/FrameData.h"
#include "FunctionLayer/Physics/PhysicsWorld.h"
#include "ResourceLayer/Scenes/SceneSerializer.h"
#include <nlohmann/json.hpp>
#include <atomic>
#include <array>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <thread>
using namespace se;
namespace
{
void Check(bool value,const char* message){if(!value)throw std::runtime_error(message);}
constexpr std::uint8_t payload[]{1,0,0,0};
void Lifetime()
{
    bool detached=false;EventBus::Publisher late;
    {
        auto environment=std::make_unique<Environment>();auto* current=environment.get();auto& world=current->GetWorld();
        auto entity=world.Create("destructor probe");late=current->Events().CreatePublisher();
        auto component=world.AttachScriptComponent(entity,1,[&](const ComponentEvent& event){
            if(event.kind!=ComponentEventKind::Detach)return;
            detached=current->Closing()&&!current->GetWorld().Create("forbidden")
                &&!current->TimeOfDay().SetRate(1)&&late.Post(1,1,payload)==EventPostResult::Closed;
        });Check(bool(component),"shutdown callback installed");environment.reset();
    }
    Check(detached&&late.Post(1,1,payload)==EventPostResult::Closed,"destructor closes services before world callbacks and keeps them alive");
    Environment reusable;const auto old=reusable.GetWorld().Create("old");late=reusable.Events().GetPublisher();
    Check(reusable.BeginShutdown()&&!reusable.RunFrame(0)&&!reusable.ConfigureClock(.1,8),"closed environment refuses new execution");
    reusable.Clear();Check(!reusable.Closing()&&reusable.GetWorld().Create("new")&&!reusable.GetWorld().IsAlive(old)
        &&reusable.RunFrame(0)&&late.Post(1,1,payload)==EventPostResult::Closed,"explicit Clear opens a new domain without reviving old publishers");
}
void PausedSingleStep()
{
    for(const bool automatic:{true,false})
    for(const unsigned pauseMode:{1u,2u,3u})
    {
        const bool simulationPaused=(pauseMode&1u)!=0,framePaused=(pauseMode&2u)!=0;
        Environment environment;Check(environment.ConfigureClock(.02,8,automatic),"single-step clock");
        auto& world=environment.GetWorld();const auto entity=world.Create("single-step body",100);
        const auto component=world.FindComponent(entity,ComponentTypes::Transform);
        std::array<unsigned,6> ticks{};
        for(std::uint32_t stage=0;stage<ticks.size();++stage)
        {
            TickDescriptor descriptor;descriptor.key="single-step."+std::to_string(stage);descriptor.component=component;
            descriptor.stage=static_cast<TickStage>(stage);descriptor.time=TickTimeDomain::Simulation;
            descriptor.callback=[&,stage](const TickContext& context){
                Check(!context.paused&&(environment.Paused()||environment.FramePaused()),"forced step schedules as active while pause remains set");
                if(stage>=2)Check(std::abs(context.deltaSeconds-.02)<1e-12,"single step uses fixed delta");
                ++ticks[stage];
            };
            Check(environment.Ticks().Register(world,std::move(descriptor))!=0,"single-step task registered");
        }
        unsigned controls=0;
        TickDescriptor control;control.key="paused-control";control.component=component;
        control.stage=TickStage::FrameBegin;control.time=TickTimeDomain::Frame;control.runWhenPaused=true;
        control.callback=[&](const TickContext&){++controls;};
        Check(environment.Ticks().Register(world,std::move(control))!=0,"runtime pause control registered");
        PhysicsWorld physics;BodyDesc bodyDescription;bodyDescription.shape.center={0,10,0};bodyDescription.entityStableId=100;
        const auto body=physics.CreateBody(bodyDescription);Check(bool(body),"single-step real body");physics.SetEnvironmentDriven(true);
        unsigned physicsCalls=0,phases=0;bool attemptNested=true;
        EnvironmentHooks hooks;
        hooks.phase=[&](EnvironmentPhase phase,const TickContext& context){
            ++phases;
            if(phase==EnvironmentPhase::FrameBegin&&!context.paused&&attemptNested)
            {attemptNested=false;Check(!environment.StepFrame(hooks),"nested single step rejected without cancelling outer step");}
            return true;
        };
        hooks.physics=[&](double seconds){++physicsCalls;return physics.StepFromEnvironment(static_cast<float>(seconds));};
        environment.SetPaused(simulationPaused);environment.SetFramePaused(framePaused);
        Check(environment.RunFrame(.3,hooks)&&environment.FixedStepIndex()==0&&physicsCalls==0,"paused normal frame does not simulate");
        Check(framePaused?(controls==0&&phases==0&&environment.FrameIndex()==0):(controls==1&&phases>0&&environment.FrameIndex()==1),
            "editor frame pause suppresses all stages; runtime simulation pause keeps frame controls");
        for(const auto calls:ticks)Check(calls==0,"paused ordinary frame skips normal script ticks");
        const auto alpha=environment.Clock().Alpha(),dropped=environment.Clock().DroppedSeconds();
        const auto controlsBeforeStep=controls,phasesBeforeStep=phases;
        Check(environment.StepFrame(hooks)&&environment.Paused()==simulationPaused&&environment.FramePaused()==framePaused&&environment.FixedStepIndex()==1
            &&std::abs(environment.SimulationSeconds()-.02)<1e-12&&physicsCalls==1,"paused step executes exactly one physical step in automatic and manual modes");
        Check(controls==controlsBeforeStep+1&&phases>phasesBeforeStep,"single step also advances UI control and phase hooks once");
        for(const auto calls:ticks)Check(calls==1,"single step executes every script stage once");
        const auto bodyHeight=physics.GetBody(body)->description.shape.center.y;
        Check(bodyHeight<10&&environment.Clock().Alpha()==alpha&&environment.Clock().DroppedSeconds()==dropped,"single step moves real physics body without changing clock remainder");
        const auto phasesAfterStep=phases,controlsAfterStep=controls;
        Check(environment.RunFrame(.3,hooks)&&environment.Paused()==simulationPaused&&environment.FramePaused()==framePaused&&physicsCalls==1
            &&physics.GetBody(body)->description.shape.center.y==bodyHeight,"next normal frame remains paused");
        Check(framePaused?(phases==phasesAfterStep&&controls==controlsAfterStep):(phases>phasesAfterStep&&controls==controlsAfterStep+1),
            "step preserves independent runtime and editor pause semantics");
        for(const auto calls:ticks)Check(calls==1,"paused frame after step does not replay script tick");
        auto failed=hooks;failed.physics=[](double){return false;};
        Check(!environment.StepFrame(failed)&&environment.Paused()==simulationPaused&&environment.FramePaused()==framePaused&&!environment.IsRunningFrame(),"failed single step restores both pause states and frame guards");
        const auto before=ticks;
        Check(environment.RunFrame(.3,hooks)&&ticks==before&&physicsCalls==1,"failed step cannot leave force flag enabled");
        environment.SetFramePaused(false);
        Check(environment.Paused()==simulationPaused&&!environment.FramePaused(),"editor resume does not change runtime pause state");
        environment.Clear();Check(!environment.Paused()&&!environment.FramePaused(),"Clear resets both pause states");
    }
}
void Cycles()
{
    EditorSession editor;TODProfile profile;profile.dayLengthSeconds=1;profile.initialTime=.2;
    editor.SetTODResolver([&](const std::string&,std::string&){return std::make_shared<const TODProfile>(profile);});
    Check(editor.Apply("seed",[](World& world){return bool(world.Create("edit source",10))&&world.SetEnvironmentSettings({"profile",false});}),"edit seed");
    std::string baseline,error;Check(SerializeScene(editor.EditWorld(),baseline,error),"edit bytes");
    EntityHandle oldEntity;ComponentHandle oldComponent;SceneHandle oldScene;TickToken oldTick=0;EventBus::Publisher oldPublisher;
    unsigned stopped=0,delivered=0,detached=0,asyncCompleted=0;
    for(unsigned cycle=0;cycle<24;++cycle)
    {
        EventBus::SubscriptionToken token;TickToken tick=0;bool stopClean=false;std::atomic<bool> workerStarted=false;
        Check(editor.SetPlayCallbacks({},[&](World& world){
            auto& environment=*editor.PlayEnvironment();const auto entity=world.FindByStableId(10);
            const auto component=world.FindComponent(entity,ComponentTypes::Transform);
            TickDescriptor rejected;rejected.key="closed";rejected.component=component;rejected.callback=[](const auto&){};
            SceneHandle previous;
            stopClean=environment.Closing()&&world.IsAlive(entity)&&environment.Ticks().Unregister(tick)
                &&!environment.Ticks().Register(world,std::move(rejected))&&!environment.Scenes().Create("closed")
                &&environment.Scenes().ExchangeCreationScope(environment.Scenes().ActiveScene(),previous)
                &&!environment.GetWorld().Create("closed")&&!token.Active()&&environment.Scenes().Statistics().pendingJobs==0;
            ++stopped;
        }),"cycle callbacks");
        Check(editor.StartPlay(),"fresh Play");auto& environment=*editor.PlayEnvironment();auto& world=environment.GetWorld();
        const auto entity=world.FindByStableId(10);const auto component=world.FindComponent(entity,ComponentTypes::Transform);
        const auto scene=environment.Scenes().ActiveScene();Check(entity&&component&&scene,"new runtime context");
        Check(!world.IsAlive(oldEntity)&&!world.GetComponent(oldComponent)&&!environment.Scenes().Inspect(oldScene).handle
            &&!environment.Ticks().Unregister(oldTick)&&environment.Ticks().Size()==0&&environment.Events().SubscriptionCount()==0,"all old identities rejected");
        if(cycle)Check(oldPublisher.Post(1,1,payload)==EventPostResult::Closed,"late publisher cannot enter next Play");
        Check(environment.FrameIndex()==0&&environment.SimulationSeconds()==0&&environment.Snapshot().time==.2,"new Play resets its clock and TOD");
        auto attached=world.AttachScriptComponent(entity,1,[&](const ComponentEvent& event){if(event.kind==ComponentEventKind::Detach)++detached;});Check(bool(attached),"runtime behavior");
        Check(environment.Events().RegisterType({1,"engine.int32",1,4}),"test fact type");
        token=environment.Events().Subscribe(component,1,[&](const EventMessage&,EventSubscriptionId){++delivered;return true;});
        auto publisher=environment.Events().CreatePublisher();Check(publisher.Post(1,1,payload)==EventPostResult::Accepted,"queued runtime fact");
        TickDescriptor descriptor;descriptor.key="post";descriptor.component=component;descriptor.stage=TickStage::PostTick;
        EnvironmentSnapshot post;descriptor.callback=[&](const TickContext&){post=environment.Snapshot();};tick=environment.Ticks().Register(world,std::move(descriptor));Check(tick!=0,"runtime schedule");
        Check(environment.RunFrame(.05),"runtime frame");FrameData frame;BuildFrameData(environment,nullptr,nullptr,frame);
        Check(frame.timeOfDay&&post.time==frame.timeOfDay->time&&post.simulationSeconds==frame.timeOfDay->simulationSeconds&&delivered==cycle+1,"PostTick and final snapshot agree");
        const auto owned=*frame.timeOfDay;Check(environment.TimeOfDay().SetTime(7,.8)&&owned.time==post.time,"owned snapshot survives later state changes");
        const auto pending=environment.Scenes().Create("pending");
        Check(environment.Scenes().RequestFile(pending,"unused",SceneLoadMode::Additive,{},[&](const auto&,SceneDocument&,std::string&,const CancellationToken& cancellation){
            workerStarted=true;while(!cancellation.IsCancelled())std::this_thread::sleep_for(std::chrono::milliseconds(1));++asyncCompleted;return true;
        })!=0,"pending load");
        for(unsigned attempt=0;attempt<2000&&!workerStarted;++attempt)std::this_thread::sleep_for(std::chrono::milliseconds(1));Check(workerStarted,"worker running before Stop");
        oldEntity=entity;oldComponent=component;oldScene=scene;oldTick=tick;oldPublisher=publisher;
        if(cycle%2==0)
        {
            EnvironmentHooks hooks;hooks.phase=[&](EnvironmentPhase phase,const TickContext&){if(phase==EnvironmentPhase::PostTick)editor.StopPlay();return true;};
            Check(environment.RunFrame(0,hooks)&&editor.IsPlaying(),"Stop inside frame defers until caller returns");editor.CompletePlayFrame();
        }
        else editor.StopPlay();
        Check(!editor.IsPlaying()&&stopClean&&stopped==cycle+1&&detached==cycle+1&&asyncCompleted==cycle+1,"ordered teardown joins owned worker, runs cleanup once and releases world");
        std::string after;Check(SerializeScene(editor.EditWorld(),after,error)&&after==baseline,"Play never modifies edit bytes");
    }
    unsigned unexpectedStop=0;editor.SetPlayCallbacks({},[&](World&){++unexpectedStop;});
    editor.SetTODResolver([](const std::string&,std::string& error)->std::shared_ptr<const TODProfile>{error="TOD.ResolveFailed";return {};});
    Check(!editor.StartPlay()&&!editor.IsPlaying()&&unexpectedStop==0,"failed candidate never invokes external stop for a runtime that was not started");
}
}
int main(int argc,char** argv)
{
    try{Lifetime();PausedSingleStep();Cycles();const std::filesystem::path path=argc>1?argv[1]:"out/validation/environment-isolation.json";
        std::filesystem::create_directories(path.parent_path());std::ofstream file(path);file<<nlohmann::json{{"success",true},{"cycles",24},{"pendingWorkersJoined",24},{"deferredStops",12},{"editBytesUnchanged",true},{"oldIdentitiesRejected",true},{"postTickSnapshot",true},{"destructorServicesAlive",true},{"failedStartNoStop",true},{"pausedSingleStepAllStages",true},{"singleStepRealPhysics",true},{"manualClockSingleStep",true},{"singleStepFailureGuard",true},{"independentFrameAndSimulationPause",true}}.dump(2);
        Check(bool(file),"isolation evidence");std::cout<<"EnvironmentIsolationTests passed (24 Play/Stop cycles, pending loads, shutdown callbacks, domains and snapshots).\n";return 0;
    }catch(const std::exception& error){std::cerr<<error.what()<<'\n';return 1;}
}
