#pragma once
#include "World.h"
#include "CoreLayer/BoundedMailbox.h"
#include <atomic>
#include <span>
#include <thread>
#include <exception>

namespace se
{
using EventTypeId=std::uint64_t;
using EventSubscriptionId=std::uint64_t;
enum class EventPhase:std::uint32_t {NextFrame,AfterPhysics,AfterLogic};
enum class EventPostResult:std::uint32_t {Accepted,Closed,UnknownType,UnsupportedVersion,InvalidScope,InvalidTarget,Full,TooLarge,SequenceExhausted};
struct EventTypeDescriptor {EventTypeId id=0;std::string key;std::uint32_t version=1;std::size_t maximumBytes=65536;};
struct EventEnvelope
{
    std::uint64_t world=0,channel=0;EventTypeId type=0;
    EntityHandle source,target;StableEntityId sourceStableId=0;
    std::uint64_t scene=0,frame=0,fixedStep=0,sequence=0;
    std::uint32_t version=1;EventPhase phase=EventPhase::AfterLogic;
};
struct EventMessage
{
    EventEnvelope envelope;
    std::shared_ptr<const std::vector<std::uint8_t>> payload;
    std::uint64_t eligible=0;
};
enum class EventTraceKind:std::uint32_t {Accepted,Delivered,UnknownType,BadVersion,Full,Closed,InvalidScope,InvalidTarget,NoReceiver,CallbackFailed,TooLarge,SequenceExhausted,InvalidPayload};
struct EventPayloadRejected:std::exception {const char* what()const noexcept override{return "Event.InvalidPayload";}};
struct EventTrace {EventTraceKind kind;EventEnvelope envelope;std::uint64_t subscriber=0;std::size_t queued=0;std::uint64_t deliveryFrame=0,deliveryStep=0;};
struct EventStatistics
{
    std::uint64_t accepted=0,delivered=0,rejected=0,droppedTargets=0,noReceiver=0,callbackFailures=0,invalidPayload=0;
    MailboxStatistics mailbox;
};
class EventBus
{
    struct Shared;
    struct Channel;
    struct Subscription;
public:
    class Publisher
    {
    public:
        Publisher()=default;
        EventPostResult Post(EventTypeId type,std::uint32_t version,std::span<const std::uint8_t> payload,EventPhase phase=EventPhase::AfterLogic,
            EntityHandle source={},StableEntityId sourceStableId=0,EntityHandle target={},std::uint64_t scene=0)const;
        void Close()noexcept;
        std::uint64_t ChannelId()const noexcept;
    private:
        friend class EventBus;
        Publisher(std::weak_ptr<Shared> state,std::shared_ptr<Channel> channel):state_(std::move(state)),channel_(std::move(channel)){}
        std::weak_ptr<Shared> state_;std::shared_ptr<Channel> channel_;
    };
    class SubscriptionToken
    {
    public:
        SubscriptionToken()=default;~SubscriptionToken(){Close();}
        SubscriptionToken(const SubscriptionToken&)=delete;SubscriptionToken& operator=(const SubscriptionToken&)=delete;
        SubscriptionToken(SubscriptionToken&& other)noexcept:state_(std::move(other.state_)){}
        SubscriptionToken& operator=(SubscriptionToken&& other)noexcept{if(this!=&other){Close();state_=std::move(other.state_);}return *this;}
        void Close()noexcept;
        EventSubscriptionId Id()const noexcept;
        bool Active()const noexcept;
    private:
        friend class EventBus;
        explicit SubscriptionToken(std::shared_ptr<Subscription> state):state_(std::move(state)){}
        std::shared_ptr<Subscription> state_;
    };
    using Callback=std::function<bool(const EventMessage&,EventSubscriptionId)>; // false 表示接收端当前不参与，不能伪造已投递统计。
    explicit EventBus(World& world,std::size_t capacity=4096,std::size_t byteBudget=16*1024*1024);
    ~EventBus();
    EventBus(const EventBus&)=delete;EventBus& operator=(const EventBus&)=delete;
    bool BindWorld(World& world);
    void ResetWorld(World& world);
    bool RegisterType(EventTypeDescriptor descriptor);
    Publisher GetPublisher()const;
    Publisher CreatePublisher();
    SubscriptionToken Subscribe(ComponentHandle owner,EventTypeId type,Callback callback,const Publisher* channel=nullptr,EntityHandle target={},std::uint64_t scene=0);
    bool Unsubscribe(EventSubscriptionId token);
    void Sweep();
    void BeginFrame(std::uint64_t frame);
    void SetFixedStep(std::uint64_t step);
    void SynchronizeIdleClock(std::uint64_t frame,std::uint64_t step);
    void EndFrame();
    bool Drain(EventPhase phase);
    void Close();
    std::size_t SubscriptionCount()const;
    EventStatistics Statistics()const;
    std::vector<EventTrace> Trace()const;
    const std::string& LastError()const noexcept{return error_;}
    void SetSceneResolver(std::function<bool(std::uint64_t,EntityHandle)> resolver);
private:
    bool Fail(const char* key);
    bool OwnerThread()const;
    void Record(EventTraceKind kind,const EventMessage& message,EventSubscriptionId subscriber=0);
    World* world_;
    std::shared_ptr<Shared> state_;
    std::unordered_map<EventSubscriptionId,std::shared_ptr<Subscription>> subscriptions_;
    std::function<bool(std::uint64_t,EntityHandle)> sceneResolver_;
    std::size_t capacity_,byteBudget_;
    std::thread::id ownerThread_=std::this_thread::get_id();
    bool draining_=false;
    std::string error_;
};
}
