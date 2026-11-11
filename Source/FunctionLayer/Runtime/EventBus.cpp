#include "EventBus.h"
#include <algorithm>
#include <deque>
#include <stdexcept>

namespace se
{
namespace
{
std::atomic<std::uint64_t> nextEventWorld{1},nextEventChannel{1},nextEventSubscription{1};
std::uint64_t Allocate(std::atomic<std::uint64_t>& counter)
{
    auto prior=counter.load();while(prior){const auto next=prior==UINT64_MAX?0:prior+1;if(counter.compare_exchange_weak(prior,next))return prior;}return 0;
}
}
struct EventBus::Channel {std::uint64_t id=0;std::atomic<bool> active{true};};
struct EventBus::Shared
{
    mutable std::mutex mutex;
    BoundedMailbox<EventMessage> mailbox;
    std::unordered_map<EventTypeId,EventTypeDescriptor> types;
    std::shared_ptr<Channel> defaultChannel=std::make_shared<Channel>();
    std::vector<std::weak_ptr<Channel>> channels;
    std::uint64_t domain=Allocate(nextEventWorld),frame=0,step=0,sequence=0,physicsOccurrence=0,logicOccurrence=0;
    std::uint64_t lastNextFrame=0,lastLogicFrame=0,lastPhysicsFrame=0,lastPhysicsStep=0;
    bool closed=false,inFrame=false,sceneFiltering=false;
    EventStatistics statistics;
    std::deque<EventTrace> trace;
    Shared(std::size_t count,std::size_t bytes):mailbox(count,bytes){channels.push_back(defaultChannel);if(!domain)closed=true;}
    void Record(EventTraceKind kind,const EventEnvelope& envelope,std::uint64_t subscriber=0)
    {if(trace.size()==1024)trace.pop_front();trace.push_back({kind,envelope,subscriber,mailbox.Statistics().queued,frame,step});}
};
struct EventBus::Subscription
{
    EventSubscriptionId id=0;ComponentHandle owner;EventTypeId type=0;EntityHandle target;std::uint64_t scene=0;
    std::shared_ptr<Channel> channel;Callback callback;std::atomic<bool> active{true};
};
EventPostResult EventBus::Publisher::Post(EventTypeId type,std::uint32_t version,std::span<const std::uint8_t> bytes,EventPhase phase,
    EntityHandle source,StableEntityId sourceStableId,EntityHandle target,std::uint64_t scene)const
{
    const auto state=state_.lock();if(!state||!channel_)return EventPostResult::Closed;
    std::lock_guard lock(state->mutex);
    EventMessage message;message.envelope={state->domain,channel_->id,type,source,target,sourceStableId,scene,state->frame,state->step,0,version,phase};
    auto reject=[&](EventPostResult result,EventTraceKind kind){++state->statistics.rejected;state->Record(kind,message.envelope);return result;};
    if(state->closed||!channel_->active.load())return reject(EventPostResult::Closed,EventTraceKind::Closed);
    const auto descriptor=state->types.find(type);
    if(descriptor==state->types.end())return reject(EventPostResult::UnknownType,EventTraceKind::UnknownType);
    if(version!=descriptor->second.version)return reject(EventPostResult::UnsupportedVersion,EventTraceKind::BadVersion);
    if(static_cast<unsigned>(phase)>2||(scene&&!state->sceneFiltering))return reject(EventPostResult::InvalidScope,EventTraceKind::InvalidScope);
    if(bytes.size()>descriptor->second.maximumBytes)return reject(EventPostResult::TooLarge,EventTraceKind::TooLarge);
    if(state->sequence==UINT64_MAX)return reject(EventPostResult::SequenceExhausted,EventTraceKind::SequenceExhausted);
    const auto occurrence=phase==EventPhase::NextFrame?state->frame:phase==EventPhase::AfterPhysics?state->physicsOccurrence:state->logicOccurrence;
    if(occurrence==UINT64_MAX)return reject(EventPostResult::SequenceExhausted,EventTraceKind::SequenceExhausted);
    message.eligible=occurrence+1;message.envelope.sequence=++state->sequence;
    message.payload=std::make_shared<const std::vector<std::uint8_t>>(bytes.begin(),bytes.end());
    const auto envelope=message.envelope;
    const auto result=state->mailbox.GetPublisher().Push(std::move(message),sizeof(EventMessage)+bytes.size());
    if(result!=MailboxResult::Accepted)
        return reject(result==MailboxResult::Full?EventPostResult::Full:result==MailboxResult::TooLarge?EventPostResult::TooLarge:EventPostResult::Closed,
            result==MailboxResult::Full?EventTraceKind::Full:result==MailboxResult::TooLarge?EventTraceKind::TooLarge:EventTraceKind::Closed);
    ++state->statistics.accepted;state->Record(EventTraceKind::Accepted,envelope);return EventPostResult::Accepted;
}
void EventBus::Publisher::Close()noexcept{if(channel_)channel_->active=false;}
std::uint64_t EventBus::Publisher::ChannelId()const noexcept{return channel_?channel_->id:0;}
void EventBus::SubscriptionToken::Close()noexcept{if(state_){state_->active=false;state_.reset();}}
EventSubscriptionId EventBus::SubscriptionToken::Id()const noexcept{return state_?state_->id:0;}
bool EventBus::SubscriptionToken::Active()const noexcept{return state_&&state_->active.load()&&state_->channel->active.load();}
EventBus::EventBus(World& world,std::size_t capacity,std::size_t byteBudget):world_(&world),state_(std::make_shared<Shared>(capacity,byteBudget)),capacity_(capacity),byteBudget_(byteBudget){}
EventBus::~EventBus(){Close();}
bool EventBus::Fail(const char* key){error_=key;return false;}
bool EventBus::OwnerThread()const{return std::this_thread::get_id()==ownerThread_;}
bool EventBus::BindWorld(World& world)
{
    if(!OwnerThread())return Fail("Event.WrongThread");
    if(world_==&world)return true;
    {std::lock_guard lock(state_->mutex);if(draining_||state_->inFrame)return Fail("Event.ContextMismatch");}
    ResetWorld(world);return true;
}
void EventBus::ResetWorld(World& world)
{
    if(!OwnerThread()||draining_){Fail("Event.ContextMismatch");return;}
    std::unordered_map<EventTypeId,EventTypeDescriptor> types;
    {std::lock_guard lock(state_->mutex);types=state_->types;}
    Close();world_=&world;state_=std::make_shared<Shared>(capacity_,byteBudget_);state_->types=std::move(types);
    state_->sceneFiltering=bool(sceneResolver_);error_.clear();
}
bool EventBus::RegisterType(EventTypeDescriptor descriptor)
{
    if(!OwnerThread())return Fail("Event.WrongThread");
    if(!descriptor.id||descriptor.key.empty()||descriptor.key.size()>256||!descriptor.version||descriptor.maximumBytes>16*1024*1024)
        return Fail("Event.InvalidType");
    std::lock_guard lock(state_->mutex);if(state_->closed)return Fail("Event.Closed");
    for(const auto& [id,registered]:state_->types)
    {
        if(id==descriptor.id&&registered.key==descriptor.key&&registered.version==descriptor.version&&registered.maximumBytes==descriptor.maximumBytes)return true;
        if(id==descriptor.id||registered.key==descriptor.key)return Fail("Event.DuplicateType");
    }
    if(state_->types.size()>=4096)return Fail("Event.TypeBudget");
    state_->types.emplace(descriptor.id,std::move(descriptor));error_.clear();return true;
}
EventBus::Publisher EventBus::GetPublisher()const{return Publisher{state_,state_->defaultChannel};}
EventBus::Publisher EventBus::CreatePublisher()
{
    if(!OwnerThread()){Fail("Event.WrongThread");return {};}
    std::lock_guard lock(state_->mutex);if(state_->closed){Fail("Event.Closed");return {};}
    auto channel=std::make_shared<Channel>();channel->id=Allocate(nextEventChannel);
    if(!channel->id){Fail("Event.IdentityExhausted");return {};}
    state_->channels.erase(std::remove_if(state_->channels.begin(),state_->channels.end(),[](const auto& value){const auto channel=value.lock();return !channel||!channel->active.load();}),state_->channels.end());
    if(state_->channels.size()>=65536){Fail("Event.PublisherBudget");return {};}
    state_->channels.push_back(channel);return Publisher{state_,std::move(channel)};
}
EventBus::SubscriptionToken EventBus::Subscribe(ComponentHandle owner,EventTypeId type,Callback callback,const Publisher* channel,EntityHandle target,std::uint64_t scene)
{
    if(!OwnerThread()){Fail("Event.WrongThread");return {};}
    if(!world_->GetComponent(owner)||!callback||subscriptions_.size()>=65536){Fail("Event.InvalidOwner");return {};}
    const auto publisher=channel?*channel:GetPublisher();
    std::lock_guard lock(state_->mutex);
    if(publisher.state_.lock()!=state_||!publisher.channel_||!publisher.channel_->active.load()||state_->closed){Fail("Event.Closed");return {};}
    if(!state_->types.contains(type)){Fail("Event.UnknownType");return {};}
    if((scene&&!sceneResolver_)||(target&&!world_->IsAlive(target))){Fail("Event.InvalidScope");return {};}
    auto subscription=std::make_shared<Subscription>();subscription->id=Allocate(nextEventSubscription);
    if(!subscription->id){Fail("Event.IdentityExhausted");return {};}
    subscription->owner=owner;subscription->type=type;subscription->callback=std::move(callback);subscription->channel=publisher.channel_;subscription->target=target;subscription->scene=scene;
    subscriptions_.emplace(subscription->id,subscription);error_.clear();return SubscriptionToken{std::move(subscription)};
}
bool EventBus::Unsubscribe(EventSubscriptionId id)
{
    if(!OwnerThread())return Fail("Event.WrongThread");const auto it=subscriptions_.find(id);
    if(it==subscriptions_.end())return false;it->second->active=false;subscriptions_.erase(it);return true;
}
void EventBus::Sweep()
{
    if(!OwnerThread())return;
    for(auto it=subscriptions_.begin();it!=subscriptions_.end();)
    {
        auto& subscription=*it->second;
        if(!subscription.active.load()||!subscription.channel->active.load()||!world_->GetComponent(subscription.owner))
        {subscription.active=false;it=subscriptions_.erase(it);}else ++it;
    }
}
void EventBus::BeginFrame(std::uint64_t frame)
{if(!OwnerThread()){Fail("Event.WrongThread");return;}std::lock_guard lock(state_->mutex);if(!frame||state_->inFrame){Fail("Event.InvalidPhase");return;}state_->frame=frame;state_->step=0;state_->inFrame=true;}
void EventBus::SetFixedStep(std::uint64_t step)
{if(!OwnerThread()){Fail("Event.WrongThread");return;}std::lock_guard lock(state_->mutex);state_->step=step;}
void EventBus::SynchronizeIdleClock(std::uint64_t frame,std::uint64_t step)
{if(!OwnerThread()){Fail("Event.WrongThread");return;}std::lock_guard lock(state_->mutex);if(!state_->inFrame){state_->frame=frame;state_->step=step;}}
void EventBus::EndFrame()
{if(!OwnerThread())return;std::lock_guard lock(state_->mutex);state_->inFrame=false;}
void EventBus::Record(EventTraceKind kind,const EventMessage& message,EventSubscriptionId subscriber)
{std::lock_guard lock(state_->mutex);state_->Record(kind,message.envelope,subscriber);}
bool EventBus::Drain(EventPhase phase)
{
    if(!OwnerThread())return Fail("Event.WrongThread");
    if(draining_||!world_->IsInFrame()||static_cast<unsigned>(phase)>2)return Fail("Event.InvalidPhase");
    std::uint64_t eligible=0;
    {
        std::lock_guard lock(state_->mutex);if(state_->closed)return Fail("Event.Closed");
        if(!state_->inFrame||(phase==EventPhase::NextFrame&&state_->lastNextFrame==state_->frame)
            ||(phase==EventPhase::AfterLogic&&state_->lastLogicFrame==state_->frame)
            ||(phase==EventPhase::AfterPhysics&&(!state_->step||(state_->lastPhysicsFrame==state_->frame&&state_->lastPhysicsStep==state_->step))))
            return Fail("Event.InvalidPhase");
        auto& occurrence=phase==EventPhase::AfterPhysics?state_->physicsOccurrence:state_->logicOccurrence;
        if(phase==EventPhase::NextFrame)eligible=state_->frame;
        else{if(occurrence==UINT64_MAX)return Fail("Event.IdentityExhausted");eligible=++occurrence;}
        if(phase==EventPhase::NextFrame)state_->lastNextFrame=state_->frame;
        else if(phase==EventPhase::AfterLogic)state_->lastLogicFrame=state_->frame;
        else{state_->lastPhysicsFrame=state_->frame;state_->lastPhysicsStep=state_->step;}
    }
    Sweep();std::vector<std::shared_ptr<Subscription>> subscriptions;
    for(const auto& [_,subscription]:subscriptions_)subscriptions.push_back(subscription);
    std::sort(subscriptions.begin(),subscriptions.end(),[](const auto& a,const auto& b){return a->id<b->id;});
    auto batch=state_->mailbox.TakeIf([&](const EventMessage& message){return message.envelope.phase==phase&&message.eligible<=eligible;});
    draining_=true;struct Guard{bool& value;~Guard(){value=false;}} guard{draining_};
    for(auto& lease:batch)
    {
        const auto& message=lease.Get();const auto& envelope=message.envelope;
        if(envelope.world!=state_->domain||(envelope.target&&!world_->IsAlive(envelope.target)))
        {std::lock_guard lock(state_->mutex);++state_->statistics.droppedTargets;state_->Record(EventTraceKind::InvalidTarget,envelope);lease.Reset();continue;}
        bool delivered=false;
        for(const auto& subscription:subscriptions)
        {
            const auto* owner=world_->GetComponent(subscription->owner);
            if(!subscription->active.load()||!subscription->channel->active.load()||!owner||!owner->Enabled()||!owner->Active()||!world_->IsEnabled(owner->Owner())
                ||subscription->type!=envelope.type||(subscription->channel->id&&envelope.channel&&subscription->channel->id!=envelope.channel)
                ||(envelope.target&&owner->Owner()!=envelope.target)||(subscription->target&&subscription->target!=envelope.target)
                ||(envelope.scene&&(!sceneResolver_||!sceneResolver_(envelope.scene,owner->Owner())))
                ||(subscription->scene&&(!sceneResolver_||!sceneResolver_(subscription->scene,owner->Owner()))))continue;
            bool called=false;
            try{world_->InvokeComponentCallback([&]{called=subscription->callback(message,subscription->id);},subscription->owner);}
            catch(const EventPayloadRejected&)
            {std::lock_guard lock(state_->mutex);++state_->statistics.invalidPayload;state_->Record(EventTraceKind::InvalidPayload,envelope,subscription->id);}
            catch(...)
            {world_->SetComponentEnabled(subscription->owner,false);std::lock_guard lock(state_->mutex);++state_->statistics.callbackFailures;state_->Record(EventTraceKind::CallbackFailed,envelope,subscription->id);}
            if(!world_->FlushPhaseCommands())return Fail("Event.WorldOperationFailed");
            if(called){delivered=true;std::lock_guard lock(state_->mutex);++state_->statistics.delivered;state_->Record(EventTraceKind::Delivered,envelope,subscription->id);}
        }
        if(!delivered){std::lock_guard lock(state_->mutex);++state_->statistics.noReceiver;state_->Record(EventTraceKind::NoReceiver,envelope);}
        lease.Reset();
    }
    Sweep();error_.clear();return true;
}
void EventBus::Close()
{
    if(!state_)return;
    {std::lock_guard lock(state_->mutex);state_->closed=true;for(auto& weak:state_->channels)if(auto channel=weak.lock())channel->active=false;state_->mailbox.Close();}
    // SafeHandle 终结器可关闭已静止的会话；共享发布状态先关闭，订阅容器只由所属线程清理。
    if(OwnerThread()){for(auto& [_,subscription]:subscriptions_)subscription->active=false;subscriptions_.clear();}
}
std::size_t EventBus::SubscriptionCount()const
{std::size_t count=0;for(const auto& [_,subscription]:subscriptions_)if(subscription->active.load()&&subscription->channel->active.load()&&world_->GetComponent(subscription->owner))++count;return count;}
EventStatistics EventBus::Statistics()const
{std::lock_guard lock(state_->mutex);auto result=state_->statistics;result.mailbox=state_->mailbox.Statistics();return result;}
std::vector<EventTrace> EventBus::Trace()const
{std::lock_guard lock(state_->mutex);return {state_->trace.begin(),state_->trace.end()};}
void EventBus::SetSceneResolver(std::function<bool(std::uint64_t,EntityHandle)> resolver)
{if(!OwnerThread()){Fail("Event.WrongThread");return;}sceneResolver_=std::move(resolver);std::lock_guard lock(state_->mutex);state_->sceneFiltering=bool(sceneResolver_);}
}
