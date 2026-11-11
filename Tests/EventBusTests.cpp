#include "FunctionLayer/Runtime/EventBus.h"
#include "FunctionLayer/Runtime/Environment.h"
#include <nlohmann/json.hpp>
#include <array>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <stdexcept>

using namespace se;
namespace
{
constexpr EventTypeId ping=1025,pong=1026;
void Check(bool condition,const char* message){if(!condition)throw std::runtime_error(message);}
ComponentHandle Owner(World& world,std::uint64_t control){return world.AttachScriptComponent(world.Create("event owner"),control);}
EventPostResult Post(const EventBus::Publisher& publisher,std::uint8_t value,EventPhase phase=EventPhase::AfterLogic,EventTypeId type=ping)
{return publisher.Post(type,1,std::span(&value,1),phase);}
struct Frame
{
    World& world;EventBus& bus;
    Frame(World& worldValue,EventBus& busValue,std::uint64_t index):world(worldValue),bus(busValue)
    {bus.BeginFrame(index);Check(world.BeginFrame(),"world frame boundary");}
    ~Frame(){world.FinishFrame();bus.EndFrame();}
};
void Register(EventBus& bus)
{Check(bus.RegisterType({ping,"test.ping",1,8})&&bus.RegisterType({pong,"test.pong",1,8}),"register two types");}
}
int main(int argc,char** argv)
{
    try
    {
        World world,foreignWorld;EventBus bus(world,32,65536),foreign(foreignWorld);Register(bus);Register(foreign);
        Check(bus.RegisterType({ping,"test.ping",1,8}),"exact registration idempotent");
        Check(!bus.RegisterType({ping,"other",1,8})&&!bus.RegisterType({1040,"test.ping",1,8}),"ID and key collisions rejected");
        const auto a=Owner(world,1),b=Owner(world,2),c=Owner(world,3),other=Owner(foreignWorld,4);
        auto publisher=bus.GetPublisher();std::vector<std::string> order;
        EventBus::SubscriptionToken bToken,late;
        auto aToken=bus.Subscribe(a,ping,[&](const EventMessage& message,EventSubscriptionId){
            const auto value=message.payload->at(0);order.push_back("a"+std::to_string(value));
            if(value==1)
            {
                Check(Post(publisher,2)==EventPostResult::Accepted,"same-stage handler publication");
                Check(Post(publisher,9,EventPhase::NextFrame,pong)==EventPostResult::Accepted,"handler next-frame publication");
                bToken.Close();late=bus.Subscribe(c,ping,[&](const EventMessage& next,EventSubscriptionId){order.push_back("c"+std::to_string(next.payload->at(0)));return true;});
            }
            return true;
        });
        bToken=bus.Subscribe(b,ping,[&](const EventMessage&,EventSubscriptionId){order.push_back("b");return true;});
        auto pongToken=bus.Subscribe(a,pong,[&](const EventMessage& message,EventSubscriptionId){if(message.payload->at(0)!=9)return false;order.push_back("p9");Check(Post(publisher,7)==EventPostResult::Accepted,"later-stage publication stays in this frame");return true;});
        Check(aToken.Id()&&bToken.Id()&&pongToken.Id(),"subscription identities");
        Check(!bus.Subscribe(other,ping,[](const EventMessage&,EventSubscriptionId){return true;}).Id(),"foreign owner rejected");
        auto bytes=std::array<std::uint8_t,1>{1};Check(publisher.Post(ping,1,bytes)==EventPostResult::Accepted,"copy owned payload");bytes[0]=42;
        Check(Post(publisher,3)==EventPostResult::Accepted,"second message in batch");
        {
            Frame frame(world,bus,1);Check(bus.Drain(EventPhase::NextFrame)&&bus.Drain(EventPhase::AfterLogic),"first phase drains");
            Check(order==std::vector<std::string>{"a1","a3"},"fixed subscription/message batches and immediate cancellation");
            Check(!bus.Drain(EventPhase::AfterLogic)&&bus.LastError()=="Event.InvalidPhase","cannot manually replay a logical stage in the same frame");
            Check(!bus.Drain(EventPhase::AfterPhysics),"zero fixed step cannot fabricate physics delivery");
        }
        {
            Frame frame(world,bus,2);Check(bus.Drain(EventPhase::NextFrame)&&bus.Drain(EventPhase::AfterLogic),"next frame and later logic drains");
            Check(order==std::vector<std::string>{"a1","a3","p9","a2","c2","a7","c7"},"same-stage waits, future stage proceeds and new subscription starts next batch");
        }
        Check(foreign.Statistics().accepted==0&&foreign.Statistics().delivered==0,"two worlds never share queues");
        Check(publisher.Post(9999,1,bytes)==EventPostResult::UnknownType&&publisher.Post(ping,2,bytes)==EventPostResult::UnsupportedVersion,"bad type/version visible");
        const auto large=std::array<std::uint8_t,9>{};Check(publisher.Post(ping,1,large)==EventPostResult::TooLarge,"registered byte maximum");
        Check(publisher.Post(ping,1,bytes,EventPhase::AfterLogic,{},0,{},42)==EventPostResult::InvalidScope,"unavailable scene resolver explicitly rejects scope");
        const auto doomed=Owner(world,5);const auto target=world.GetComponent(doomed)->Owner();const auto stable=world.Get(target)->stableId;
        Check(publisher.Post(ping,1,bytes,EventPhase::AfterLogic,{},0,target)==EventPostResult::Accepted,"targeted queued event");
        Check(world.Destroy(target),"target logically retired");
        bool historical=false;auto history=bus.Subscribe(a,pong,[&](const EventMessage& message,EventSubscriptionId){historical=message.envelope.source==target&&!world.IsAlive(target)&&message.envelope.sourceStableId==stable;return true;});
        Check(publisher.Post(pong,1,bytes,EventPhase::AfterLogic,target,stable)==EventPostResult::Accepted,"historical source owns values");
        {Frame frame(world,bus,3);Check(bus.Drain(EventPhase::AfterLogic)&&historical,"invalid source is informational while invalid target is discarded");}
        Check(bus.Statistics().droppedTargets==1,"stale target counted");
        auto oldChannel=bus.CreatePublisher();int oldCalls=0,newCalls=0;
        auto oldSubscription=bus.Subscribe(a,ping,[&](const EventMessage&,EventSubscriptionId){++oldCalls;return true;},&oldChannel);
        Check(Post(oldChannel,5)==EventPostResult::Accepted,"old run event queued");oldChannel.Close();
        auto newChannel=bus.CreatePublisher();auto newSubscription=bus.Subscribe(a,ping,[&](const EventMessage&,EventSubscriptionId){++newCalls;return true;},&newChannel);
        {Frame frame(world,bus,4);Check(bus.Drain(EventPhase::AfterLogic)&&oldCalls==0&&newCalls==0,"old run cannot deliver to a new run subscription");}
        Check(Post(oldChannel,6)==EventPostResult::Closed&&Post(newChannel,6)==EventPostResult::Accepted,"closed publisher rejects late post");
        {Frame frame(world,bus,5);Check(bus.Drain(EventPhase::AfterLogic)&&newCalls==1,"new run channel receives its own event");}
        EventBus full(world,4,sizeof(EventMessage)+1);Register(full);auto limited=full.GetPublisher();bool rejectedInFlight=false;
        auto fullSubscription=full.Subscribe(a,ping,[&](const EventMessage&,EventSubscriptionId){rejectedInFlight=Post(limited,3)==EventPostResult::Full;return true;});
        Check(Post(limited,1)==EventPostResult::Accepted&&Post(limited,2)==EventPostResult::Full,"byte budget rejects without losing accepted message");
        {Frame frame(world,full,6);Check(full.Drain(EventPhase::AfterLogic)&&rejectedInFlight,"dispatch still consumes bounded budget");}
        Check(full.Statistics().mailbox.pending==0&&full.Statistics().mailbox.bytes==0,"dispatch lease releases budget");
        World threadedWorld;EventBus threaded(threadedWorld,64,65536);Register(threaded);const auto threadedOwner=Owner(threadedWorld,6);auto worker=threaded.GetPublisher();
        std::vector<unsigned> received;const auto mainThread=std::this_thread::get_id();bool onMain=true;
        auto threadedSubscription=threaded.Subscribe(threadedOwner,ping,[&](const EventMessage& message,EventSubscriptionId){onMain=onMain&&std::this_thread::get_id()==mainThread;received.push_back(message.payload->at(0));return true;});
        bool accepted=true;std::thread producer([&]{for(unsigned i=0;i<32;++i)if(Post(worker,std::uint8_t(i))!=EventPostResult::Accepted)accepted=false;});producer.join();
        Check(accepted&&received.empty(),"worker posts without executing behaviours");
        {Frame frame(threadedWorld,threaded,1);Check(threaded.Drain(EventPhase::AfterLogic)&&onMain&&received.size()==32,"main thread delivers worker batch");}
        for(unsigned i=0;i<32;++i)Check(received[i]==i,"same-producer event order");
        auto previous=threaded.GetPublisher();threaded.ResetWorld(threadedWorld);Check(Post(previous,1)==EventPostResult::Closed&&threaded.SubscriptionCount()==0,"world epoch reset closes old workers and subscriptions");
        World faultsWorld;EventBus faults(faultsWorld);Register(faults);const auto badOwner=Owner(faultsWorld,10),healthy=Owner(faultsWorld,11);int healthyCalls=0;
        auto badSubscription=faults.Subscribe(badOwner,ping,[](const EventMessage&,EventSubscriptionId)->bool{throw std::runtime_error("intentional event failure");});
        auto healthySubscription=faults.Subscribe(healthy,ping,[&](const EventMessage&,EventSubscriptionId){++healthyCalls;return true;});
        Check(Post(faults.GetPublisher(),1)==EventPostResult::Accepted,"fault fixture");
        {Frame frame(faultsWorld,faults,1);Check(faults.Drain(EventPhase::AfterLogic)&&healthyCalls==1&&!faultsWorld.GetComponent(badOwner)->Enabled(),"failed receiver isolated from healthy receiver");}
        Check(faults.Statistics().callbackFailures==1,"callback failure trace");
        auto lateWorker=faults.GetPublisher();std::thread finalizer([&]{faults.Close();});finalizer.join();Check(Post(lateWorker,2)==EventPostResult::Closed,"idle finalizer close rejects late worker event");faults.Sweep();Check(faults.SubscriptionCount()==0,"owner clears subscriptions after finalizer closes shared state");
        World physicsWorld;EventBus physicsEvents(physicsWorld);Register(physicsEvents);const auto physicsOwner=Owner(physicsWorld,20);auto physicsPublisher=physicsEvents.GetPublisher();std::vector<unsigned> physicsMessages;
        auto physicsSubscription=physicsEvents.Subscribe(physicsOwner,ping,[&](const EventMessage& message,EventSubscriptionId){const auto value=message.payload->at(0);physicsMessages.push_back(value);if(value==1)Check(Post(physicsPublisher,2,EventPhase::AfterPhysics)==EventPostResult::Accepted,"physics callback re-entry queues the next actual substep");return true;});
        Check(Post(physicsPublisher,1,EventPhase::AfterPhysics)==EventPostResult::Accepted,"physics fixture publication");
        {Frame frame(physicsWorld,physicsEvents,1);physicsEvents.SetFixedStep(1);Check(physicsEvents.Drain(EventPhase::AfterPhysics)&&physicsMessages==std::vector<unsigned>{1},"first physical occurrence");Check(!physicsEvents.Drain(EventPhase::AfterPhysics),"same physical substep cannot drain twice");physicsEvents.SetFixedStep(2);Check(physicsEvents.Drain(EventPhase::AfterPhysics)&&physicsMessages==std::vector<unsigned>{1,2},"next occurrence can be the next substep in this frame");}
        Environment environment;auto& runtime=environment.GetWorld();Register(environment.Events());const auto runtimeOwner=Owner(runtime,30);auto runtimePublisher=environment.Events().GetPublisher();std::vector<unsigned> runtimeOrder;int physicalSteps=0;
        auto runtimeSubscription=environment.Events().Subscribe(runtimeOwner,ping,[&](const EventMessage& message,EventSubscriptionId){runtimeOrder.push_back(message.payload->at(0));return true;});
        TickDescriptor tick;tick.key="event.integration";tick.component=runtimeOwner;tick.stage=TickStage::Tick;tick.callback=[&](const TickContext&){Check(Post(runtimePublisher,1)==EventPostResult::Accepted,"Tick posts to later AfterLogic");};
        Check(environment.Ticks().Register(runtime,std::move(tick))!=0&&Post(runtimePublisher,9,EventPhase::NextFrame)==EventPostResult::Accepted&&Post(runtimePublisher,4,EventPhase::AfterPhysics)==EventPostResult::Accepted,"Environment integration fixture");
        EnvironmentHooks runtimeHooks;runtimeHooks.physics=[&](double){++physicalSteps;return true;};
        Check(environment.RunFrame(0,runtimeHooks)&&runtimeOrder==std::vector<unsigned>{9,1}&&physicalSteps==0,"Environment delivers NextFrame and AfterLogic but no fake AfterPhysics");
        Check(environment.RunFrame(1.0/30,runtimeHooks)&&physicalSteps==2&&runtimeOrder==std::vector<unsigned>{9,1,4,1},"Environment drains queued physics on actual step and logic after Tick");
        auto stoppedPublisher=runtimePublisher;environment.Clear();Check(Post(stoppedPublisher,1)==EventPostResult::Closed&&environment.RunFrame(0),"Environment close invalidates publisher and starts a fresh domain");
        const std::filesystem::path proof=argc>1?argv[1]:"out/event-bus/verification.json";std::filesystem::create_directories(proof.parent_path());
        std::ofstream(proof)<<nlohmann::json{{"success",true},{"newType",true},{"worldIsolation",true},{"ownedPayload",true},{"fixedBatch",true},{"cancelImmediate",true},{"nextFrame",true},{"laterPhase",true},{"zeroPhysics",true},{"physicalOccurrences",2},{"environmentIntegration",true},{"staleTarget",true},{"historicalSource",true},{"runChannelIsolation",true},{"boundedInFlight",true},{"badTypeVersion",true},{"workerMessages",32},{"mainThreadCallbacks",true},{"lateWorkerRejected",true},{"callbackIsolation",true},{"trace",order}}.dump(2);
        std::cout<<"EventBus passed (owned phased batches, lifetimes, budget, channels and workers)\n";return 0;
    }
    catch(const std::exception& exception){std::cerr<<"[FAIL] "<<exception.what()<<'\n';return 1;}
}
