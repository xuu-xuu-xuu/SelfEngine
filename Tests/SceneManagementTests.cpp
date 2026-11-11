#include "FunctionLayer/Runtime/Environment.h"
#include "FunctionLayer/Runtime/ComponentMetadata.h"
#include "FunctionLayer/Physics/PhysicsWorld.h"
#include <nlohmann/json.hpp>
#include <array>
#include <filesystem>
#include <fstream>
#include <future>
#include <iostream>
#include <stdexcept>

using namespace se;
namespace
{
void Check(bool value,const char* message){if(!value)throw std::runtime_error(message);}
class Link final:public ComponentBase {public:static constexpr ComponentTypeId TypeId=65539;PropertyEntityReference target;};
const std::array fields{PropertyDescriptor{"sceneLink.target","Scene.LinkTarget",PropertyType::EntityReference,PropertyEntityReference{}}};
bool ReadLink(const World& world,ComponentHandle handle,std::size_t,PropertyValue& value,std::string& error)
{const auto* link=world.GetRegisteredComponent<Link>(handle);if(!link){error="World.InvalidComponent";return false;}value=link->target;return true;}
bool WriteLink(World& world,ComponentHandle handle,std::size_t,const PropertyValue& value,std::string& error)
{const auto* link=world.GetRegisteredComponent<Link>(handle);if(!link){error="World.InvalidComponent";return false;}auto next=*link;next.target=std::get<PropertyEntityReference>(value);if(world.UpdateRegisteredComponent(handle,next))return true;error=world.LastError();return false;}
ComponentHandle CreateLink(World& world,EntityHandle entity,std::span<const PropertyValue> values,bool enabled,std::string& error)
{Link link;link.target=std::get<PropertyEntityReference>(values[0]);const auto handle=world.AttachRegisteredComponent(entity,link,enabled);if(!handle)error=world.LastError();return handle;}
SceneDocument Document(StableEntityId id,float x=0,StableEntityId target=0,std::string* saved=nullptr)
{
    World world{World::StagingTag{}};const auto entity=world.Create("actor",id);auto transform=*world.GetTransform(entity);transform.position.x=x;world.SetTransform(entity,transform);
    std::string text,error;Check(SerializeScene(world,text,error),"fixture serialization");
    if(target){auto json=nlohmann::json::parse(text);json["entities"][0]["components"].push_back({{"type",std::to_string(Link::TypeId)},{"version",1},{"enabled",true},{"properties",{{"target",std::to_string(target)}}}});text=json.dump();}
    SceneDocument result;Check(ParseSceneDocument(text,result,error),"fixture CPU document");if(saved)*saved=text;return result;
}
template<class Predicate> void Until(Environment& environment,Predicate predicate)
{for(int i=0;i<2000&&!predicate();++i){Check(environment.RunFrame(0),"completion frame");std::this_thread::sleep_for(std::chrono::milliseconds(1));}Check(predicate(),"bounded completion wait");}
struct Gate
{
    std::shared_ptr<std::promise<void>> started=std::make_shared<std::promise<void>>();std::promise<void> release;std::shared_future<void> go=release.get_future().share();
    SceneManager::Reader Reader(SceneDocument document)
    {return [signal=started,go=go,document](const std::filesystem::path&,SceneDocument& result,std::string&,const CancellationToken&){signal->set_value();go.wait();result=document;return true;};}
    void Wait(){Check(started->get_future().wait_for(std::chrono::seconds(2))==std::future_status::ready,"reader started");}
};
}
int main(int argc,char** argv)
{
    try
    {
        std::string error;Check(RegisterComponent({"sceneLink","Scene.LinkTarget",fields,true,Link::TypeId,1,false,ReadLink,WriteLink,CreateLink,nullptr},error),"registered reference component");
        Environment environment;auto& world=environment.GetWorld();auto& scenes=environment.Scenes();
        const auto a=scenes.Create("A"),b=scenes.Create("B"),c=scenes.Create("C"),conflict=scenes.Create("conflict");
        Check(scenes.RequestDocument(a,Document(1))!=0&&world.Size()==0&&environment.RunFrame(0),"initial activation waits for FrameBegin");
        auto original=world.FindByStableId(1);const auto oldScope=scenes.Inspect(a).scope;Check(original&&scenes.ActiveScene()==a&&scenes.SceneOf(original)==a,"A membership is owned by World");
        int ticks=0,detaches=0;const auto probe=world.AttachScriptComponent(original,1,[&](const ComponentEvent& event){if(event.kind==ComponentEventKind::Detach)++detaches;});
        TickDescriptor tick;tick.key="scene.old";tick.component=probe;tick.stage=TickStage::Tick;tick.callback=[&](const TickContext&){++ticks;};Check(environment.Ticks().Register(world,std::move(tick))!=0,"old runtime tick");
        scenes.SetValidator([](const World&,std::string& failure){failure="Scene.DependencyFailed";return false;});
        Check(scenes.RequestDocument(b,Document(2))&&environment.RunFrame(0)&&scenes.Inspect(b).state==SceneState::Failed&&world.IsAlive(original)&&world.Size()==1&&ticks==1&&detaches==0,"dependency rejection preserves old live world and tasks");scenes.SetValidator({});
        Check(scenes.RequestDocument(a,Document(1,7))&&environment.RunFrame(0),"same-ID reload");
        auto reloaded=world.FindByStableId(1);Check(reloaded&&reloaded!=original&&reloaded.index==original.index&&!world.IsAlive(original)&&world.ResolveReference(1)==reloaded&&world.GetTransform(reloaded)->position.x==7&&detaches==1&&ticks==1,"reload reuses storage with fresh handles and persistent ID mapping");
        Check(scenes.Inspect(a).scope!=oldScope&&environment.Events().RegisterType({100,"test.scene_ping",1,1}),"scene reload changes the message scope generation");
        const auto scopeOwner=world.AttachScriptComponent(reloaded,50);int scopeMessages=0;auto scoped=environment.Events().Subscribe(scopeOwner,100,[&](const EventMessage&,EventSubscriptionId){++scopeMessages;return true;});const std::array<std::uint8_t,1> scopedValue{1};
        Check(environment.Events().GetPublisher().Post(100,1,scopedValue,EventPhase::AfterLogic,{},0,{},oldScope)==EventPostResult::Accepted&&environment.RunFrame(0)&&scopeMessages==0,"old scope cannot reach new members after same-record reload");
        Check(environment.Events().GetPublisher().Post(100,1,scopedValue,EventPhase::AfterLogic,{},0,{},scenes.Inspect(a).scope)==EventPostResult::Accepted&&environment.RunFrame(0)&&scopeMessages==1,"current scope reaches its actual scene");
        Check(scenes.RequestDocument(c,Document(3,3,1),SceneLoadMode::Additive)&&environment.RunFrame(0),"additive scene with explicit external stable reference");
        const auto cEntity=world.FindByStableId(3);const auto cLink=world.FindComponent(cEntity,Link::TypeId);
        Check(world.IsAlive(reloaded)&&scenes.SceneOf(cEntity)==c&&world.GetRegisteredComponent<Link>(cLink)->target.stableId==1,"additive commit preserves existing runtime identity");
        Check(!world.SetParent(cEntity,reloaded)&&world.LastError()=="Scene.CrossSceneParent","cross-scene parenting rejected");
        const auto child=scenes.CreateEntity(c,"child",4);Check(child&&world.SetParent(child,cEntity)&&scenes.SceneOf(child)==c,"explicit scene creation and same-scene parent");
        Check(scenes.RequestDocument(conflict,Document(1),SceneLoadMode::Additive)&&environment.RunFrame(0)&&scenes.Inspect(conflict).state==SceneState::Failed&&world.IsAlive(reloaded)&&world.IsAlive(cEntity),"additive stable-ID collision leaves both old scenes usable");
        Check(scenes.RequestDocument(a,Document(1,8,3))&&environment.RunFrame(0),"replacement can reference a retained scene");
        const auto currentA=world.FindByStableId(1);Check(currentA!=reloaded&&world.GetRegisteredComponent<Link>(cLink)->target.stableId==1&&world.ResolveReference(1)==currentA,"retained persistent reference resolves to replacement, retained component identity stays valid");
        EntityHandle cBorn;const auto cProbe=world.AttachScriptComponent(cEntity,2);TickDescriptor cTick;cTick.key="scene.member";cTick.component=cProbe;cTick.stage=TickStage::Tick;cTick.callback=[&](const TickContext&){if(!cBorn)cBorn=world.Create("C callback birth",5);};
        Check(environment.Ticks().Register(world,std::move(cTick))&&environment.RunFrame(0)&&cBorn&&world.IsPending(cBorn)&&scenes.SceneOf(cBorn)==c,"component callback birth inherits its scene and waits for next frame");Check(environment.RunFrame(0)&&world.IsAlive(cBorn),"member birth commits next frame");
        PhysicsWorld physics;BodyDesc body;body.entityStableId=3;const auto cBody=physics.CreateBody(body);Check(physics.IsAlive(cBody),"actual physical proxy");int unloadHooks=0;
        scenes.SetUnloadHook([&](SceneHandle scene,std::span<const EntityHandle> members,std::span<const StableEntityId> invalidated){if(scene==c){++unloadHooks;Check(!members.empty()&&world.IsAlive(cEntity)&&std::find(invalidated.begin(),invalidated.end(),3)!=invalidated.end(),"teardown precedes physical GO destruction and identifies removed references");physics.DestroyBody(cBody);}});
        Check(environment.Events().RegisterType({100,"test.scene_ping",1,1}),"scene event type");auto publisher=environment.Events().GetPublisher();int laterMember=0,healthy=0;
        auto unload=environment.Events().Subscribe(cProbe,100,[&](const EventMessage&,EventSubscriptionId){Check(scenes.Unload(c),"unload from a callback");return true;});
        const auto childProbe=world.AttachScriptComponent(child,3);auto skipped=environment.Events().Subscribe(childProbe,100,[&](const EventMessage&,EventSubscriptionId){++laterMember;return true;});
        const auto aProbe=world.AttachScriptComponent(currentA,4);auto remaining=environment.Events().Subscribe(aProbe,100,[&](const EventMessage&,EventSubscriptionId){++healthy;return true;});
        const std::array<std::uint8_t,1> value{1};Check(publisher.Post(100,1,value)==EventPostResult::Accepted&&environment.RunFrame(0)&&laterMember==0&&healthy==1&&!physics.IsAlive(cBody)&&unloadHooks==1&&!world.IsEnabled(cEntity),"logical unload stops later members and releases physical proxy while healthy scene continues");
        Check(environment.RunFrame(0)&&!world.IsAlive(cEntity)&&!world.IsAlive(child)&&!world.IsAlive(cBorn)&&scenes.Inspect(c).state==SceneState::Unloaded&&world.GetRegisteredComponent<Link>(world.FindComponent(currentA,Link::TypeId))->target.stableId==0,"unload reclaims all members and clears retained references to removed IDs");

        const auto slow=scenes.Create("slow"),fast=scenes.Create("fast");Gate slowGate,fastGate;
        Check(scenes.RequestFile(slow,"slow.mock",SceneLoadMode::Replace,{},slowGate.Reader(Document(11)))!=0,"slow request");slowGate.Wait();
        Check(scenes.RequestFile(fast,"fast.mock",SceneLoadMode::Replace,{},fastGate.Reader(Document(12)))!=0,"newer request");fastGate.Wait();fastGate.release.set_value();
        Until(environment,[&]{return scenes.Inspect(fast).state==SceneState::ReadyToActivate;});
        Check(!scenes.Inspect(fast).active&&world.IsAlive(currentA)&&!world.FindByStableId(12),"newly received candidate waits for next submission boundary");
        Check(environment.RunFrame(0)&&scenes.Inspect(fast).active,"received candidate activates at next FrameBegin");const auto fastEntity=world.FindByStableId(12);Check(fastEntity&&!world.IsAlive(currentA),"newer result activates first");slowGate.release.set_value();
        Until(environment,[&]{return scenes.Inspect(slow).state==SceneState::Cancelled;});Check(world.IsAlive(fastEntity)&&!world.FindByStableId(11)&&scenes.Statistics().staleResults>=1,"late old result cannot replace newer scene");
        const auto cancelled=scenes.Create("cancelled");Gate cancelGate;Check(scenes.RequestFile(cancelled,"cancel.mock",SceneLoadMode::Additive,{},cancelGate.Reader(Document(20)))!=0,"cancel fixture");cancelGate.Wait();Check(scenes.Cancel(cancelled),"cancel pending load");cancelGate.release.set_value();Until(environment,[&]{return scenes.Statistics().pendingJobs==0;});Check(!scenes.Inspect(cancelled).active&&!world.FindByStableId(20),"cancelled CPU result is released without activation");
        Gate unloadGate;Check(scenes.RequestFile(fast,"reload.mock",SceneLoadMode::Replace,{},unloadGate.Reader(Document(12,99)))!=0,"reload pending");unloadGate.Wait();Check(scenes.Unload(fast)&&environment.RunFrame(0)&&!world.IsAlive(fastEntity),"unload cancels an active reload");unloadGate.release.set_value();Until(environment,[&]{return scenes.Statistics().pendingJobs==0;});Check(!world.FindByStableId(12)&&scenes.Inspect(fast).state==SceneState::Unloaded,"completion after unload cannot resurrect scene");
        const auto directory=(argc>1?std::filesystem::path(argv[1]):std::filesystem::path("out/scene-management/verification.json")).parent_path();std::filesystem::create_directories(directory/L"中文场景");std::string source;Document(30,1,0,&source);const auto path=directory/L"中文场景/B.scene.json";std::ofstream(path,std::ios::binary)<<source;
        const auto disk=scenes.Create("disk");Check(scenes.RequestFile(disk,path,SceneLoadMode::Additive)!=0,"actual Unicode file request");Until(environment,[&]{return scenes.Inspect(disk).state==SceneState::Active;});Check(static_cast<bool>(world.FindByStableId(30)),"actual background read and parse");
        const auto missing=scenes.Create("missing");Check(scenes.RequestFile(missing,directory/"missing.scene.json")!=0,"missing-file request");Until(environment,[&]{return scenes.Inspect(missing).state==SceneState::Failed;});Check(world.FindByStableId(30)&&scenes.ActiveScene()==disk,"missing file preserves active scene");
        EntityHandle resurrected;const auto closeOwner=world.FindByStableId(30);world.AttachScriptComponent(closeOwner,99,[&](const ComponentEvent& event){if(event.kind==ComponentEventKind::Detach)resurrected=world.Create("must not resurrect");});environment.Clear();Check(!resurrected&&world.Size()==0&&!world.HasPendingCommands()&&scenes.Statistics().records==0,"context clear cancels jobs and prevents lifecycle resurrection");
        const std::filesystem::path proof=argc>1?argv[1]:"out/scene-management/verification.json";
        std::ofstream(proof)<<nlohmann::json{{"success",true},{"sameIdReload",true},{"freshRuntimeHandles",true},{"storageReused",true},{"oldFailurePreserved",true},{"additivePreservesHandles",true},{"conflictRejected",true},{"crossSceneParentRejected",true},{"callbackBirthMembership",true},{"callbackUnloadStopsMembers",true},{"physicalProxyReleased",true},{"referenceRebound",true},{"referenceCleared",true},{"outOfOrderDiscarded",true},{"cancelledNoActivation",true},{"unloadedNoResurrection",true},{"actualUnicodeFile",true},{"clearStopsBirth",true}}.dump(2);
        std::cout<<"SceneManagement passed (atomic candidates, stable IDs, membership, proxies and late work)\n";return 0;
    }
    catch(const std::exception& exception){std::cerr<<"[FAIL] "<<exception.what()<<'\n';return 1;}
}
