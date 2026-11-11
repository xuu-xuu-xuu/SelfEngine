#include "TickScheduler.h"
#include <algorithm>
#include <atomic>
#include <unordered_set>

namespace se
{
namespace
{
std::atomic<std::uint64_t> nextTickToken{1};
TickToken AllocateToken()
{
    auto prior=nextTickToken.load();
    while(prior){const auto next=prior==UINT64_MAX?0:prior+1;if(nextTickToken.compare_exchange_weak(prior,next))return prior;}
    return 0;
}
bool Fixed(TickStage stage){return stage==TickStage::FixedPreTick||stage==TickStage::FixedPostTick;}
}
void TickScheduler::Record(std::string key,const Task& task,std::string prerequisite,std::uint64_t step)
{
    if(diagnostics_.size()<1024)diagnostics_.push_back({std::move(key),task.descriptor.key,std::move(prerequisite),frame_,step});
}
bool TickScheduler::Validate(const Task& task,const std::vector<TickPrerequisite>& prerequisites)
{
    std::unordered_set<TickToken> unique;
    for(const auto& prerequisite:prerequisites)
    {
        const auto it=tasks_.find(prerequisite.task);
        if(it==tasks_.end()||!unique.insert(prerequisite.task).second||prerequisite.task==task.token)
        {error_=prerequisite.task==task.token?"Tick.DependencyCycle":"Tick.InvalidPrerequisite";Record(error_,task);return false;}
        if(static_cast<unsigned>(it->second->descriptor.stage)>static_cast<unsigned>(task.descriptor.stage))
        {error_="Tick.FutureDependency";Record(error_,task,it->second->descriptor.key);return false;}
        if(prerequisite.mode!=TickDependencyMode::OrderOnly&&prerequisite.mode!=TickDependencyMode::Fresh)
        {error_="Tick.InvalidPrerequisite";return false;}
    }
    return true;
}
bool TickScheduler::Sort(TickStage stage,std::vector<std::shared_ptr<Task>>& result)
{
    std::vector<std::shared_ptr<Task>> candidates;
    for(const auto& [_,task]:tasks_)if(task->descriptor.stage==stage)candidates.push_back(task);
    std::sort(candidates.begin(),candidates.end(),[](const auto& a,const auto& b){return a->token<b->token;});
    std::unordered_set<TickToken> emitted;
    while(result.size()<candidates.size())
    {
        bool found=false;
        for(const auto& task:candidates)
        {
            if(emitted.contains(task->token))continue;bool ready=true;
            for(const auto& prerequisite:task->descriptor.prerequisites)
            {const auto it=tasks_.find(prerequisite.task);if(it!=tasks_.end()&&it->second->descriptor.stage==stage&&!emitted.contains(prerequisite.task)){ready=false;break;}}
            if(!ready)continue;result.push_back(task);emitted.insert(task->token);found=true;break;
        }
        if(!found){error_="Tick.DependencyCycle";for(const auto& task:candidates)if(!emitted.contains(task->token))Record(error_,*task);return false;}
    }
    return true;
}
TickToken TickScheduler::Register(World& world,TickDescriptor descriptor)
{
    if(closed_){error_="Environment.Closed";return 0;}
    error_.clear();
    if(inFrame_&&frame_==UINT64_MAX){error_="Tick.TokenExhausted";return 0;}
    if((world_&&world_!=&world&&!tasks_.empty())||!world.GetComponent(descriptor.component))
    {error_="Tick.InvalidComponent";return 0;}
    if(descriptor.key.empty()||descriptor.key.size()>256||!descriptor.callback||tasks_.size()>=65536
        ||static_cast<unsigned>(descriptor.stage)>static_cast<unsigned>(TickStage::PostTick)
        ||(descriptor.time!=TickTimeDomain::Frame&&descriptor.time!=TickTimeDomain::Simulation))
    {error_="Tick.InvalidDescriptor";return 0;}
    for(const auto& [_,task]:tasks_)if(task->descriptor.key==descriptor.key){error_="Tick.DuplicateTask";return 0;}
    auto task=std::make_shared<Task>();task->token=AllocateToken();if(!task->token){error_="Tick.TokenExhausted";return 0;}
    task->descriptor=std::move(descriptor);task->eligibleFrame=inFrame_?frame_+1:0;
    if(!Validate(*task,task->descriptor.prerequisites))return 0;
    world_=&world;tasks_.emplace(task->token,task);return task->token;
}
bool TickScheduler::SetPrerequisites(TickToken token,std::vector<TickPrerequisite> prerequisites)
{
    if(closed_){error_="Environment.Closed";return false;}
    if(inFrame_){error_="Tick.MutationDuringFrame";return false;}
    error_.clear();const auto it=tasks_.find(token);if(it==tasks_.end()){error_="Tick.InvalidTask";return false;}
    auto task=it->second;if(!Validate(*task,prerequisites))return false;
    auto previous=std::move(task->descriptor.prerequisites);task->descriptor.prerequisites=std::move(prerequisites);
    std::vector<std::shared_ptr<Task>> sorted;
    if(Sort(task->descriptor.stage,sorted))return true;
    task->descriptor.prerequisites=std::move(previous);return false;
}
bool TickScheduler::Unregister(TickToken token)
{
    const auto it=tasks_.find(token);if(it==tasks_.end())return false;
    it->second->enabled=false;tasks_.erase(it);return true;
}
void TickScheduler::Clear()
{for(auto& [_,task]:tasks_)task->enabled=false;tasks_.clear();if(!inFrame_)world_=nullptr;diagnostics_.clear();error_.clear();closed_=false;}
bool TickScheduler::BeginFrame(World& world,std::uint64_t frame)
{
    if(closed_){error_="Environment.Closed";return false;}
    if(inFrame_||(world_&&world_!=&world&&!tasks_.empty())){error_="Tick.ContextMismatch";return false;}
    world_=&world;frame_=frame;diagnostics_.clear();inFrame_=true;
    SweepDead(world);return true;
}
void TickScheduler::SweepDead(World& world)
{
    std::vector<TickToken> dead;for(const auto& [token,task]:tasks_)if(!world.GetComponent(task->descriptor.component))dead.push_back(token);
    for(const auto token:dead)Unregister(token);
}
bool TickScheduler::RunStage(World& world,TickContext context,double simulationDelta)
{
    if(!inFrame_||world_!=&world||context.frame!=frame_){error_="Tick.ContextMismatch";return false;}
    std::vector<std::shared_ptr<Task>> ordered;if(!Sort(context.stage,ordered))return false;
    for(const auto& task:ordered)
    {
        const auto* component=world.GetComponent(task->descriptor.component);
        if(!task->enabled||task->eligibleFrame>frame_||!component||!component->Enabled()||!component->Active()||!world.IsEnabled(component->Owner())
            ||(context.paused&&!task->descriptor.runWhenPaused))continue;
        bool ready=true;
        for(const auto& prerequisite:task->descriptor.prerequisites)
        {
            const auto it=tasks_.find(prerequisite.task);
            if(it==tasks_.end()){Record("Tick.PrerequisiteUnavailable",*task,std::to_string(prerequisite.task),context.fixedStep);ready=false;break;}
            const auto& prior=*it->second;
            if(!world.GetComponent(prior.descriptor.component))
            {Record("Tick.PrerequisiteUnavailable",*task,prior.descriptor.key,context.fixedStep);ready=false;break;}
            if(prerequisite.mode==TickDependencyMode::Fresh&&(prior.lastFrame!=frame_||(Fixed(prior.descriptor.stage)&&prior.lastStep!=context.fixedStep)))
            {Record("Tick.FreshDataUnavailable",*task,prior.descriptor.key,context.fixedStep);ready=false;break;}
        }
        if(!ready)continue;
        context.deltaSeconds=Fixed(context.stage)?simulationDelta:task->descriptor.time==TickTimeDomain::Frame?context.frameSeconds:simulationDelta;
        try
        {
            world.InvokeComponentCallback([&]{task->descriptor.callback(context);},task->descriptor.component);
            if(!world.FlushPhaseCommands()){error_=world.LastError();return false;}
            SweepDead(world);
            const auto* completed=world.GetComponent(task->descriptor.component);
            if(completed&&completed->Enabled()&&completed->Active()&&world.IsEnabled(completed->Owner()))
            {task->lastFrame=frame_;task->lastStep=context.fixedStep;}
        }
        catch(...)
        {task->enabled=false;world.SetComponentEnabled(task->descriptor.component,false);Record("Tick.CallbackFailed",*task,{},context.fixedStep);}
    }
    return true;
}
}
