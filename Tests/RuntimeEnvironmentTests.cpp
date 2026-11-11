#include "FunctionLayer/Runtime/Environment.h"
#include "FunctionLayer/Application/FrameData.h"
#include <nlohmann/json.hpp>
#include <fstream>
#include <array>
#include <filesystem>
#include <stdexcept>
#include <type_traits>
#include <iostream>
using namespace se;
namespace
{
void Check(bool value,const char* message){if(!value)throw std::runtime_error(message);}
void Test()
{
    static_assert(sizeof(EntityHandle)==8&&sizeof(ComponentHandle)==8&&sizeof(CameraComponent)==16&&std::is_trivially_copyable_v<Transform>&&std::is_trivially_copyable_v<CameraComponent>);
    std::vector<ComponentEventKind> trace;
    Environment environment;auto& world=environment.GetWorld();const auto parent=world.Create("parent"),child=world.Create("child");Check(world.SetParent(child,parent),"child hierarchy");
    const auto callback=[&](const ComponentEvent& event){Check(event.owner==child&&event.type==ComponentTypes::Script,"component callback identity");trace.push_back(event.kind);};
    const auto script=world.AttachScriptComponent(child,1,callback);Check(script&&trace==std::vector{ComponentEventKind::Attach,ComponentEventKind::Enable},"attach before enable");
    Check(world.SetComponentEnabled(script,false)&&world.SetComponentEnabled(script,false)&&trace.size()==3,"one disable");
    Check(world.SetEnabled(parent,false)&&world.SetComponentEnabled(script,true)&&trace.size()==3&&!world.GetComponent(script)->Active(),"ancestor suppresses activation");
    Check(world.SetEnabled(parent,true)&&trace.size()==4&&world.GetComponent(script)->Active(),"ancestor activation");
    const auto second=world.AttachScriptComponent(child,2);Check(second&&second!=script&&!world.AttachScriptComponent(child,2),"distinct instances and controlled ID uniqueness");
    Check(world.RemoveComponent(script)&&!world.GetComponent(script)&&trace==std::vector{ComponentEventKind::Attach,ComponentEventKind::Enable,ComponentEventKind::Disable,ComponentEventKind::Enable,ComponentEventKind::Disable,ComponentEventKind::Detach},"single cleanup sequence");
    Check(!world.RemoveComponent(script)&&!world.RemoveComponent(world.FindComponent(child,ComponentTypes::Transform)),"old component and required Transform rejected");
    Check(world.SetCamera(child,{})&&world.SetMesh(child,{"asset",{}})&&world.SetLight(child,{}),"runtime value wrappers");const auto camera=world.FindComponent(child,ComponentTypes::Camera);
    Check(world.GetComponent(camera)->Owner()==child&&world.GetCamera(child)->nearPlane==.1f,"runtime wrapper owns compatible properties");
    Check(world.SetCamera(child,{Pi/4,.2f,100,true})&&world.FindComponent(child,ComponentTypes::Camera)==camera,"property edits preserve component identity");
    Check(world.RemoveCamera(child)&&world.SetCamera(child,{})&&!world.GetComponent(camera)&&world.FindComponent(child,ComponentTypes::Camera)!=camera,"component generation after slot reuse");
    FrameData frame;BuildFrameData(world,nullptr,nullptr,frame);Check(frame.meshes.size()==1&&frame.cameras.size()==1&&frame.lights.size()==1,"enabled value snapshot");
    Check(world.SetComponentEnabled(world.FindComponent(child,ComponentTypes::Mesh),false),"disable render component");BuildFrameData(world,nullptr,nullptr,frame);Check(frame.meshes.empty()&&world.GetMesh(child),"inactive component keeps properties but leaves render snapshot");
    World copy(world);const auto copied=copy.FindByStableId(world.Get(child)->stableId);Check(copied&&!copy.IsAlive(child)&&!copy.GetComponent(second)&&!copy.GetComponent(copy.FindComponent(copied,ComponentTypes::Mesh))->Enabled()&&!copy.FindComponent(copied,ComponentTypes::Script),"clone properties and runtime identity are independent");
    const auto copyTransform=copy.FindComponent(copied,ComponentTypes::Transform);Check(!world.GetComponent(copyTransform),"cross-world component handle rejected");
    World moved(std::move(copy));Check(moved.IsAlive(copied)&&moved.GetComponent(copyTransform)&&!copy.IsAlive(copied),"move transfers the context without duplicate ownership");
    std::vector<StableEntityId> removed;const auto parentId=world.Get(parent)->stableId,childId=world.Get(child)->stableId;
    const auto p=world.AttachScriptComponent(parent,3,[&](const ComponentEvent& event){if(event.kind==ComponentEventKind::Detach)removed.push_back(parentId);});
    const auto c=world.AttachScriptComponent(child,4,[&](const ComponentEvent& event){if(event.kind==ComponentEventKind::Detach)removed.push_back(childId);});Check(p&&c,"subtree lifecycle probes");
    world.ForEach([&](EntityHandle entity){if(entity!=parent)return;Check(world.Destroy(parent)&&!world.IsAlive(parent)&&!world.IsAlive(child)&&!world.GetComponent(c)&&!world.SetTransform(child,{}),"same-iteration subtree access immediately rejected");Check(removed.empty(),"physical cleanup waits for safe point");});
    Check(environment.Flush()&&removed==std::vector{childId,parentId}&&world.Size()==0,"child cleanup precedes parent at safe point");environment.Clear();Check(removed.size()==2,"repeat clear never repeats callbacks");
    const auto old=world.Create("old");const auto oldComponent=world.FindComponent(old,ComponentTypes::Transform);Check(world.Destroy(old),"retire old GO");const auto replacement=world.Create("replacement");Check(replacement.index==old.index&&!world.IsAlive(old)&&!world.GetComponent(oldComponent),"GO and component stale generations");
    ComponentHandle cancelled;std::vector<ComponentEventKind> cancelTrace;
    world.ForEach([&](EntityHandle){const auto pending=world.Create("cancel before activation");Check(world.SetCamera(pending,{}),"queued component before cancellation");cancelled=world.AttachScriptComponent(pending,10,[&](const ComponentEvent& event){cancelTrace.push_back(event.kind);});Check(world.Destroy(pending)&&!world.IsAlive(pending),"cancel reserved owner");});
    Check(world.FlushCommands()&&cancelTrace.empty()&&!world.GetComponent(cancelled)&&world.AttachScriptComponent(replacement,10),"cancelled attach has no lifecycle and releases controlled ID");
    for(const auto failure:std::array{ComponentEventKind::Attach,ComponentEventKind::Enable})
    {
        std::vector<ComponentEventKind> events;const auto failed=world.AttachScriptComponent(replacement,20,[&](const ComponentEvent& event){events.push_back(event.kind);if(event.kind==failure)throw std::runtime_error("expected lifecycle error");});
        Check(!failed&&events.front()==ComponentEventKind::Attach&&events.back()==ComponentEventKind::Detach&&events.size()==(failure==ComponentEventKind::Attach?2u:4u),"failed attach/enable still cleans exactly once");
        const auto reused=world.AttachScriptComponent(replacement,20);Check(reused&&world.RemoveComponent(reused),"failed lifecycle releases controlled identity");
    }
    const auto reentrant=world.AttachScriptComponent(replacement,30,[&](const ComponentEvent& event){if(event.kind==ComponentEventKind::Attach){Check(!world.FlushCommands(),"cannot flush inside lifecycle callback");world.Create("created by callback");}});
    Check(reentrant&&world.HasPendingCommands()&&world.FlushCommands()&&world.Size()==2,"callback structural edits defer to a safe point");
    world.Clear();Check(world.Size()==0&&!world.GetComponent(reentrant),"complete environment cleanup");
}
}
int main(int argc,char** argv)
{
    try{Test();const std::filesystem::path path=argc>1?argv[1]:"out/validation/runtime-environment/verification.json";std::filesystem::create_directories(path.parent_path());std::ofstream file(path);file<<nlohmann::json{{"success",true},{"componentHandleBytes",8},{"valueCameraBytes",16},{"lifecycle",true},{"ancestorEnable",true},{"multiInstance",true},{"slotReuse",true},{"crossWorld",true},{"clone",true},{"move",true},{"immediateDestroy",true},{"safeCleanup",true},{"subtreeOrder",true},{"cancelReserved",true},{"callbackFailure",true},{"callbackReentry",true},{"snapshotEnable",true}}.dump(2);Check(bool(file),"environment evidence");std::cout<<"RuntimeEnvironmentTests passed (ownership, typed components/lifecycle, identity, logical destruction, safe cleanup and rendering values).\n";return 0;}catch(const std::exception& error){std::cerr<<error.what()<<'\n';return 1;}
}
