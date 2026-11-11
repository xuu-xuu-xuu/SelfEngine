#pragma once
#include "World.h"
#include <memory>
#include <span>

namespace se
{
enum class TickStage:std::uint32_t {FrameBegin,PreTick,FixedPreTick,FixedPostTick,Tick,PostTick};
enum class TickTimeDomain {Frame,Simulation};
enum class TickDependencyMode {OrderOnly,Fresh};
using TickToken=std::uint64_t;
struct TickPrerequisite {TickToken task=0;TickDependencyMode mode=TickDependencyMode::OrderOnly;};
struct TickContext
{
    TickStage stage=TickStage::FrameBegin;
    std::uint64_t frame=0,fixedStep=0;
    double deltaSeconds=0,frameSeconds=0,simulationSeconds=0;
    bool paused=false;
};
struct TickDescriptor
{
    std::string key;
    ComponentHandle component;
    TickStage stage=TickStage::Tick;
    TickTimeDomain time=TickTimeDomain::Frame;
    bool runWhenPaused=false;
    std::vector<TickPrerequisite> prerequisites;
    std::function<void(const TickContext&)> callback;
};
struct TickDiagnostic {std::string key,task,prerequisite;std::uint64_t frame=0,fixedStep=0;};
class TickScheduler
{
public:
    TickToken Register(World& world,TickDescriptor descriptor);
    bool SetPrerequisites(TickToken task,std::vector<TickPrerequisite> prerequisites);
    bool Unregister(TickToken task);
    void SweepDead(World& world);
    void Clear();
    void Close()noexcept{closed_=true;}
    bool BeginFrame(World& world,std::uint64_t frame);
    void FinishFrame()noexcept{inFrame_=false;}
    bool RunStage(World& world,TickContext context,double simulationDelta);
    std::size_t Size()const noexcept{return tasks_.size();}
    const std::vector<TickDiagnostic>& Diagnostics()const noexcept{return diagnostics_;}
    const std::string& LastError()const noexcept{return error_;}
private:
    struct Task {TickToken token=0;TickDescriptor descriptor;std::uint64_t eligibleFrame=0,lastFrame=0,lastStep=0;bool enabled=true;};
    bool Validate(const Task& task,const std::vector<TickPrerequisite>& prerequisites);
    bool Sort(TickStage stage,std::vector<std::shared_ptr<Task>>& result);
    void Record(std::string key,const Task& task,std::string prerequisite={},std::uint64_t step=0);
    World* world_=nullptr;
    std::unordered_map<TickToken,std::shared_ptr<Task>> tasks_;
    std::vector<TickDiagnostic> diagnostics_;
    std::string error_;
    std::uint64_t frame_=0;
    bool inFrame_=false,closed_=false;
};
}
