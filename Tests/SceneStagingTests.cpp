#include "FunctionLayer/Runtime/World.h"
#include "ResourceLayer/Scenes/SceneSerializer.h"
#include "CoreLayer/TaskQueue.h"
#include <nlohmann/json.hpp>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <stdexcept>
#include <thread>
#include <future>

using namespace se;
namespace {void Check(bool value,const char* text){if(!value)throw std::runtime_error(text);}}
int main(int argc,char** argv)
{
    try
    {
        int cancelledCallbacks=0;
        {World cancelled{World::StagingTag{}};const auto owner=cancelled.Create("cancelled",1);Check(static_cast<bool>(cancelled.AttachScriptComponent(owner,1,[&](const ComponentEvent&){++cancelledCallbacks;})),"staged component identity");Check(cancelledCallbacks==0,"candidate construction cannot invoke lifecycle");}
        Check(cancelledCallbacks==0,"discarding candidate cannot invoke detach");
        int attaches=0,enables=0,disables=0,detaches=0;EntityHandle born;World live;
        World prepared{World::StagingTag{}};const auto candidate=prepared.Create("new scene",1);
        const auto component=prepared.AttachScriptComponent(candidate,2,[&](const ComponentEvent& event){
            if(event.kind==ComponentEventKind::Attach){++attaches;born=live.Create("lifecycle birth");}
            if(event.kind==ComponentEventKind::Enable)++enables;
            if(event.kind==ComponentEventKind::Disable)++disables;
            if(event.kind==ComponentEventKind::Detach)++detaches;
        });
        Check(static_cast<bool>(component)&&attaches==0&&prepared.IsAlive(candidate),"prepared GO is private CPU state");
        const auto old=live.Create("old",1);live=std::move(prepared);Check(!live.IsAlive(old)&&live.IsAlive(candidate)&&attaches==0,"storage publication invalidates old handles before lifecycle");
        Check(live.BeginFrame()&&live.ActivateStagedComponents()&&attaches==1&&enables==1&&live.IsPending(born)&&!live.IsAlive(born),"published lifecycle runs once and birth is deferred");
        Check(live.ActivateStagedComponents()&&attaches==1&&live.FinishFrame(),"activation is idempotent");
        Check(live.BeginFrame()&&live.IsAlive(born)&&live.FinishFrame(),"lifecycle birth participates at next FrameBegin");
        std::string text,error;Check(SerializeScene(live,text,error),"normal scene serialization");
        SceneDocument parsed;bool parsedOk=false;std::thread parser([&]{parsedOk=ParseSceneDocument(text,parsed,error);});parser.join();
        World parsedWorld{World::StagingTag{}};Check(parsedOk&&InstantiateScene(parsed,parsedWorld,error)&&parsedWorld.IsStaging(),"background parsing owns only an immutable document");
        World document{World::StagingTag{}};Check(DeserializeScene(text,document,error)&&document.IsStaging(),"serializer can prepare without publishing");
        Check(live.BeginFrame()&&!InstantiateScene(parsed,live,error)&&error=="Scene.PendingCommands"&&live.IsAlive(candidate)&&live.FinishFrame(),"direct replacement during a running frame is rejected before move");
        Check(!DeserializeScene("{",live,error)&&live.IsAlive(candidate)&&attaches==1,"parse failure preserves published world and lifecycle");
        live.Clear();Check(disables==1&&detaches==1,"published lifecycle releases once");
        World fault{World::StagingTag{}};const auto faulty=fault.Create("fault"),healthy=fault.Create("healthy");int healthyEnables=0;
        const auto bad=fault.AttachScriptComponent(faulty,3,[](const ComponentEvent& event){if(event.kind==ComponentEventKind::Attach)throw std::runtime_error("fixture attach fault");});
        fault.AttachScriptComponent(healthy,4,[&](const ComponentEvent& event){if(event.kind==ComponentEventKind::Enable)++healthyEnables;});
        Check(!fault.ActivateStagedComponents()&&fault.IsAlive(faulty)&&!fault.GetComponent(bad)->Enabled()&&healthyEnables==1,"post-publication fault disables its component without rolling back healthy state");
        int workerCallbacks=0;std::promise<void> started;auto startedFuture=started.get_future();auto queue=std::make_unique<TaskQueue>(1,4);
        const auto token=queue->Submit([&](const CancellationToken& cancellation){started.set_value();while(!cancellation.IsCancelled())std::this_thread::yield();return [&]{++workerCallbacks;};});
        Check(token.Accepted()&&startedFuture.wait_for(std::chrono::seconds(2))==std::future_status::ready,"background candidate task started");
        std::thread finalizer([&]{queue.reset();});finalizer.join();Check(token.IsCancelled()&&workerCallbacks==0,"idle host finalizer cancels and joins without publishing a world callback");
        const std::filesystem::path proof=argc>1?argv[1]:"out/scene-staging/verification.json";std::filesystem::create_directories(proof.parent_path());
        std::ofstream(proof)<<nlohmann::json{{"success",true},{"cancelNoCallbacks",true},{"publishThenLifecycle",true},{"attachOnce",true},{"deferredLifecycleBirth",true},{"oldHandleInvalid",true},{"serializerStaging",true},{"backgroundDocument",true},{"frameReplaceRejected",true},{"finalizerNoPublish",true},{"badJsonKeepsLive",true},{"postCommitFaultIsolated",true}}.dump(2);
        std::cout<<"SceneStaging passed (private candidates, controlled publication, lifecycle and failure)\n";return 0;
    }
    catch(const std::exception& exception){std::cerr<<"[FAIL] "<<exception.what()<<'\n';return 1;}
}
