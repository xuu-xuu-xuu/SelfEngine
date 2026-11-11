#pragma once
#include "World.h"
#include "TickScheduler.h"
#include "EventBus.h"
#include "SceneManager.h"
#include "SceneSpatialIndex.h"
#include "TODSystem.h"
#include "CoreLayer/FixedStepClock.h"

namespace se
{
enum class EnvironmentPhase:std::uint32_t {Prepare,FrameBegin,PreTick,FixedPreTick,BeforePhysics,AfterPhysics,FixedPostTick,Tick,AfterLogic,PostTick,FrameEnd,FixedEnd,PrepareFinished};
struct EnvironmentHooks
{
    std::function<bool(EnvironmentPhase,const TickContext&)> phase;
    std::function<bool(double)> physics;
    std::function<bool()> synchronize;
};
// 运行上下文唯一拥有 World 和模拟时钟；服务通过阶段钩子接入。
class Environment
{
public:
    Environment();
    ~Environment();
    Environment(const Environment&)=delete;
    Environment& operator=(const Environment&)=delete;
    Environment(Environment&&)=delete;
    Environment& operator=(Environment&&)=delete;
    World& GetWorld()noexcept{return world_;}
    const World& GetWorld()const noexcept{return world_;}
    bool Flush();
    void Clear();
    bool BeginShutdown();
    bool Closing()const noexcept{return closing_;}
    TickScheduler& Ticks()noexcept{return ticks_;}
    const TickScheduler& Ticks()const noexcept{return ticks_;}
    EventBus& Events()noexcept{return events_;}
    const EventBus& Events()const noexcept{return events_;}
    SceneManager& Scenes()noexcept{return scenes_;}
    const SceneManager& Scenes()const noexcept{return scenes_;}
    SceneSpatialIndex& Spatial()noexcept{return spatial_;}
    const SceneSpatialIndex& Spatial()const noexcept{return spatial_;}
    TODSystem& TimeOfDay()noexcept{return tod_;}
    const TODSystem& TimeOfDay()const noexcept{return tod_;}
    using TODResolver=std::function<std::shared_ptr<const TODProfile>(const std::string&,std::string&)>;
    void SetTODResolver(TODResolver resolver){if(!closing_)todResolver_=std::move(resolver);}
    bool SynchronizeTOD();
    EnvironmentSnapshot Snapshot(double interpolation=1)const{return tod_.Snapshot(frame_,interpolation);}
    bool ConfigureClock(double step,std::size_t maxSteps,bool automatic=true);
    // 运行时模拟暂停仍允许帧阶段和 runWhenPaused 任务，供菜单/UI 使用。
    void SetPaused(bool paused)noexcept;
    bool Paused()const noexcept{return paused_;}
    // 编辑器暂停整个帧，包括 UI、帧回调及 runWhenPaused 任务。
    void SetFramePaused(bool paused)noexcept;
    bool FramePaused()const noexcept{return framePaused_;}
    bool RunFrame(double seconds,const EnvironmentHooks& hooks={});
    // 任一暂停状态下执行恰好一个固定步及完整脚本阶段；context.paused=false，结束保留两种暂停状态。
    bool StepFrame(const EnvironmentHooks& hooks={});
    bool RunFixed(double seconds,const EnvironmentHooks& hooks={});
    const FixedStepClock& Clock()const noexcept{return clock_;}
    double SimulationSeconds()const noexcept{return simulationSeconds_;}
    std::uint64_t FrameIndex()const noexcept{return frame_;}
    std::uint64_t FixedStepIndex()const noexcept{return completedFixedSteps_;}
    bool IsRunningFrame()const noexcept{return runningFrame_;}
    const std::string& LastError()const noexcept{return error_;}
private:
    bool FixedStep(World& world,double seconds,double frameSeconds,const EnvironmentHooks& hooks);
    bool Phase(World& world,EnvironmentPhase phase,TickContext context,const EnvironmentHooks& hooks);
    bool Stage(World& world,TickStage stage,double frameSeconds,double simulationDelta,const EnvironmentHooks& hooks);
    bool SchedulingPaused()const noexcept{return (paused_||framePaused_)&&!forceFixedStep_;}
    World world_;
    TickScheduler ticks_;
    EventBus events_;
    SceneManager scenes_;
    SceneSpatialIndex spatial_{world_,scenes_};
    TODSystem tod_{events_};
    TODResolver todResolver_;
    SceneHandle todScene_;std::uint64_t todRevision_=0;
    FixedStepClock clock_;
    std::uint64_t frame_=0,fixedStep_=0,completedFixedSteps_=0;
    double simulationSeconds_=0;
    bool paused_=false,framePaused_=false,runningFrame_=false,automatic_=true,closing_=false,forceFixedStep_=false;
    std::string error_;
};
}
