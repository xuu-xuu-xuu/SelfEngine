#include "Environment.h"
#include <stdexcept>
namespace se
{
Environment::Environment():events_(world_),scenes_(world_,events_)
{
    scenes_.SetEnvironmentValidator([this](const SceneEnvironmentSettings& settings,std::shared_ptr<const TODProfile>& output,std::string& error){
        output.reset();if(settings.todProfile.empty())return true;
        if(!todResolver_){error="TOD.ResolveFailed";return false;}
        auto profile=todResolver_(settings.todProfile,error);if(!profile)return false;
        if(!tod_.ValidateProfile(*profile,clock_.Step(),error))return false;output=std::make_shared<const TODProfile>(*profile);return true;
    });
}
bool Environment::SynchronizeTOD()
{
    if(closing_){error_="Environment.Closed";return false;}
    const auto scene=scenes_.Inspect(scenes_.ActiveScene());
    if(scene.handle==todScene_&&scene.revision==todRevision_)return true;
    std::shared_ptr<const TODProfile> profile;if(!scenes_.ResolveActiveEnvironment(profile)){error_=scenes_.LastError();return false;}
    if(profile){if(!tod_.SelectProfile(*profile,scene.environment.preserveTime)){error_=tod_.LastError();return false;}}
    else tod_.Deactivate();
    todScene_=scene.handle;todRevision_=scene.revision;return true;
}
Environment::~Environment()
{
    // 生命周期回调可能读取服务；必须在服务成员析构之前释放组件。
    BeginShutdown();world_.Clear();while(world_.HasPendingCommands())world_.FlushCommands();
}
bool Environment::BeginShutdown()
{
    if(runningFrame_){error_="Tick.MutationDuringFrame";return false;}
    if(closing_)return true;closing_=true;world_.shuttingDown_=true;
    ticks_.Close();events_.Close();tod_.Close();scenes_.Close();return true;
}
bool Environment::Flush(){return world_.FlushCommands();}
void Environment::Clear()
{
    if(!BeginShutdown())return;
    world_.Clear();while(world_.HasPendingCommands())world_.FlushCommands();
    ticks_.Clear();world_.shuttingDown_=false;events_.ResetWorld(world_);scenes_.ResetWorld(world_);
    spatial_.Clear();tod_.Clear();todScene_={};todRevision_=0;clock_.Reset();
    frame_=fixedStep_=completedFixedSteps_=0;simulationSeconds_=0;paused_=framePaused_=closing_=forceFixedStep_=false;error_.clear();
}
bool Environment::ConfigureClock(double step,std::size_t maxSteps,bool automatic)
{
    if(closing_){error_="Environment.Closed";return false;}
    if(runningFrame_){error_="Tick.MutationDuringFrame";return false;}
    if(!scenes_.ValidateEnvironmentProfiles([&](const TODProfile& p,std::string& error){return TODSystem::ValidateProfile(p,step,error);})){error_=scenes_.LastError();return false;}
    try{FixedStepClock candidate(step,maxSteps);if(!tod_.ConfigureStep(step)){error_=tod_.LastError();return false;}clock_=candidate;}catch(const std::invalid_argument&){error_="Clock.InvalidSettings";return false;}
    automatic_=automatic;simulationSeconds_=0;tod_.ResetSimulationClock();fixedStep_=completedFixedSteps_=0;error_.clear();return true;
}
void Environment::SetPaused(bool paused)noexcept
{if(!closing_&&paused_!=paused){paused_=paused;clock_.ClearRemainder();}}
void Environment::SetFramePaused(bool paused)noexcept
{if(!closing_&&framePaused_!=paused){framePaused_=paused;clock_.ClearRemainder();}}
bool Environment::Phase(World& world,EnvironmentPhase phase,TickContext context,const EnvironmentHooks& hooks)
{
    context.paused=SchedulingPaused();
    bool result=true;
    if(hooks.phase)world.InvokeComponentCallback([&]{result=hooks.phase(phase,context);});
    if(!result){error_="Tick.ServiceFailed";return false;}
    if(!world.FlushPhaseCommands()){error_=world.LastError();return false;}ticks_.SweepDead(world);events_.Sweep();return true;
}
bool Environment::Stage(World& world,TickStage stage,double frameSeconds,double simulationDelta,const EnvironmentHooks& hooks)
{
    TickContext context{stage,frame_,fixedStep_,frameSeconds,frameSeconds,simulationSeconds_,SchedulingPaused()};
    const auto phase=stage==TickStage::FrameBegin?EnvironmentPhase::FrameBegin:stage==TickStage::PreTick?EnvironmentPhase::PreTick:
        stage==TickStage::FixedPreTick?EnvironmentPhase::FixedPreTick:stage==TickStage::FixedPostTick?EnvironmentPhase::FixedPostTick:
        stage==TickStage::Tick?EnvironmentPhase::Tick:EnvironmentPhase::PostTick;
    if(!Phase(world,phase,context,hooks))return false;
    context.paused=SchedulingPaused();
    if(!ticks_.RunStage(world,context,simulationDelta)){error_=ticks_.LastError();return false;}
    if(!world.FlushPhaseCommands()){error_=world.LastError();return false;}return true;
}
bool Environment::FixedStep(World& world,double seconds,double frameSeconds,const EnvironmentHooks& hooks)
{
    if(SchedulingPaused())return true;
    if(fixedStep_==UINT64_MAX){error_="Tick.TokenExhausted";return false;}++fixedStep_;
    events_.SetFixedStep(fixedStep_);
    if(!tod_.Advance(seconds)){error_=tod_.LastError();return false;}
    if(!Stage(world,TickStage::FixedPreTick,frameSeconds,seconds,hooks))return false;
    TickContext context{TickStage::FixedPreTick,frame_,fixedStep_,seconds,frameSeconds,simulationSeconds_,SchedulingPaused()};
    if(!Phase(world,EnvironmentPhase::BeforePhysics,context,hooks))return false;
    // 本子步已开始，完成一次物理；暂停清空余量，阻止随后子步重放。
    if(hooks.physics&&!hooks.physics(seconds)){error_="Tick.ServiceFailed";return false;}
    ++completedFixedSteps_;simulationSeconds_+=seconds;context.simulationSeconds=simulationSeconds_;
    if(hooks.synchronize&&!hooks.synchronize()){error_="Tick.ServiceFailed";return false;}
    if(!spatial_.Synchronize()){error_=spatial_.LastError();return false;}
    if(!Phase(world,EnvironmentPhase::AfterPhysics,context,hooks))return false;
    if(!events_.Drain(EventPhase::AfterPhysics)){error_=events_.LastError();return false;}
    return Stage(world,TickStage::FixedPostTick,frameSeconds,seconds,hooks)&&Phase(world,EnvironmentPhase::FixedEnd,context,hooks);
}
bool Environment::StepFrame(const EnvironmentHooks& hooks)
{
    if(closing_){error_="Environment.Closed";return false;}
    if(!paused_&&!framePaused_){error_="Tick.StepRequiresPause";return false;}
    if(runningFrame_||forceFixedStep_){error_="Tick.InvalidFrame";return false;}
    struct StepGuard
    {
        Environment& environment;bool paused,framePaused;
        ~StepGuard(){environment.forceFixedStep_=false;environment.paused_=paused;environment.framePaused_=framePaused;}
    } guard{*this,paused_,framePaused_};
    forceFixedStep_=true;
    return RunFrame(clock_.Step(),hooks);
}
bool Environment::RunFrame(double seconds,const EnvironmentHooks& hooks)
{
    if(closing_){error_="Environment.Closed";return false;}
    if(runningFrame_||!std::isfinite(seconds)||seconds<0||frame_==UINT64_MAX){error_="Tick.InvalidFrame";return false;}
    if(framePaused_&&!forceFixedStep_){error_.clear();return true;}
    auto& world=world_;
    if(!events_.BindWorld(world)){error_=events_.LastError();return false;}
    if(!scenes_.BindWorld(world)){error_=scenes_.LastError();return false;}if(!spatial_.BindWorld(world)){error_=spatial_.LastError();return false;}events_.BeginFrame(frame_+1);
    if(!world.BeginFrame()){events_.EndFrame();error_=world.LastError();return false;}
    runningFrame_=true;++frame_;
    struct Guard{Environment& environment;World& world;~Guard(){environment.ticks_.FinishFrame();world.FinishFrame();environment.events_.EndFrame();environment.runningFrame_=false;}} guard{*this,world};
    error_.clear();
    try
    {
        // Prepare 仅供绑定准备；当前 FrameBegin 激活的组件可参与本帧。
        TickContext context{TickStage::FrameBegin,frame_,fixedStep_,seconds,seconds,simulationSeconds_,SchedulingPaused()};
        world.preparingFrame_=true;
        if(!scenes_.FrameBegin()){world.preparingFrame_=false;error_=scenes_.LastError();return false;}
        if(!SynchronizeTOD()){world.preparingFrame_=false;return false;}
        const bool prepared=Phase(world,EnvironmentPhase::Prepare,context,hooks);world.preparingFrame_=false;
        if(!prepared||!Phase(world,EnvironmentPhase::PrepareFinished,context,hooks)||!ticks_.BeginFrame(world,frame_))
        {if(error_.empty())error_=ticks_.LastError();return false;}
        if(!Stage(world,TickStage::FrameBegin,seconds,0,hooks))return false;
        if(!events_.Drain(EventPhase::NextFrame)){error_=events_.LastError();return false;}
        if(!Stage(world,TickStage::PreTick,seconds,0,hooks))return false;
        const auto before=simulationSeconds_;
        if(forceFixedStep_)
        {
            if(!FixedStep(world,clock_.Step(),seconds,hooks))return false;
        }
        else if(automatic_&&!SchedulingPaused())
        {
            clock_.Advance(seconds,[&](double dt){if(!FixedStep(world,dt,seconds,hooks))throw std::runtime_error("Tick.ServiceFailed");});
        }
        if(!Stage(world,TickStage::Tick,seconds,simulationSeconds_-before,hooks))return false;
        context={TickStage::Tick,frame_,fixedStep_,seconds,seconds,simulationSeconds_,SchedulingPaused()};
        if(!Phase(world,EnvironmentPhase::AfterLogic,context,hooks))return false;
        if(!events_.Drain(EventPhase::AfterLogic)){error_=events_.LastError();return false;}
        if(hooks.synchronize&&!hooks.synchronize()){error_="Tick.ServiceFailed";return false;}
        if(!spatial_.Synchronize()){error_=spatial_.LastError();return false;}
        if(!Stage(world,TickStage::PostTick,seconds,simulationSeconds_-before,hooks))return false;
        if(hooks.synchronize&&!hooks.synchronize()){error_="Tick.ServiceFailed";return false;}
        if(!spatial_.Synchronize()){error_=spatial_.LastError();return false;}
        return Phase(world,EnvironmentPhase::FrameEnd,context,hooks);
    }
    catch(...){if(error_.empty())error_="Tick.ServiceFailed";return false;}
}
bool Environment::RunFixed(double seconds,const EnvironmentHooks& hooks)
{
    if(closing_){error_="Environment.Closed";return false;}
    if(automatic_||runningFrame_||!std::isfinite(seconds)||seconds<=0||frame_==UINT64_MAX){error_="Tick.ManualFixedRejected";return false;}
    if(framePaused_&&!forceFixedStep_){error_.clear();return true;}
    auto& world=world_;
    if(!events_.BindWorld(world)){error_=events_.LastError();return false;}
    if(!scenes_.BindWorld(world)){error_=scenes_.LastError();return false;}if(!spatial_.BindWorld(world)){error_=spatial_.LastError();return false;}events_.BeginFrame(frame_+1);
    if(!world.BeginFrame()){events_.EndFrame();error_=world.LastError();return false;}
    runningFrame_=true;++frame_;
    struct Guard{Environment& environment;World& world;~Guard(){environment.ticks_.FinishFrame();world.FinishFrame();environment.events_.EndFrame();environment.runningFrame_=false;}} guard{*this,world};
    error_.clear();
    try{world.preparingFrame_=true;const auto ready=scenes_.FrameBegin();world.preparingFrame_=false;if(!ready){error_=scenes_.LastError();return false;}if(!SynchronizeTOD())return false;if(!ticks_.BeginFrame(world,frame_)){error_=ticks_.LastError();return false;}return FixedStep(world,seconds,seconds,hooks);}
    catch(...){if(error_.empty())error_="Tick.ServiceFailed";return false;}
}
}
