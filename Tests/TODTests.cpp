#include "FunctionLayer/Runtime/Environment.h"
#include "FunctionLayer/Application/FrameData.h"
#include "ToolLayer/Editor/TODPreview.h"
#include "ToolLayer/Editor/EditorSession.h"
#include "ToolLayer/Tools/Project.h"
#include "ResourceLayer/Assets/AssetIO.h"
#include <nlohmann/json.hpp>
#include <fstream>
#include <iostream>
#include <limits>

using namespace se;
namespace
{
void Check(bool value,const char* message){if(!value)throw std::runtime_error(message);}
bool Near(double a,double b,double epsilon=1e-8){return std::abs(a-b)<=epsilon;}
SceneDocument Document(const std::string& id,StableEntityId entity,bool preserve=false)
{
    World world;world.Create("TOD actor",entity);Check(world.SetEnvironmentSettings({id,preserve}),"fixture settings");std::string text,error;
    Check(SerializeScene(world,text,error),"fixture serialize");SceneDocument doc;Check(ParseSceneDocument(text,doc,error),"fixture parse");return doc;
}
}
int main(int argc,char** argv)
{
    try{
        Check(argc==2,"output path");const auto output=std::filesystem::path(argv[1]);std::filesystem::create_directories(output.parent_path());
        const auto root=output.parent_path()/L"昼夜资产";std::filesystem::create_directories(root);
        TODProfile profile;profile.dayLengthSeconds=1;profile.initialTime=0;profile.keys={
            {0,{{0.2f,0.3f,0.6f},{0.01f,0.02f,0.08f},{0.1f,0.2f,0.3f},0.1f,0.03f}},
            {0.5,{{1,0.9f,0.7f},{0.2f,0.4f,0.8f},{0.4f,0.5f,0.6f},4,0.002f}}};
        std::string error,text,roundtrip;Check(SerializeTODProfile(profile,text,error),"serialize profile");TODProfile loaded;
        Check(ParseTODProfile(text,loaded,error)&&SerializeTODProfile(loaded,roundtrip,error)&&roundtrip==text,"profile roundtrip");
        auto bad=profile;bad.dayLengthSeconds=0;Check(!SerializeTODProfile(bad,roundtrip,error),"zero day rejected");bad=profile;bad.keys[1].time=0;Check(!ValidateTODProfile(bad,error),"duplicate keys rejected");bad=profile;bad.keys[0].value.fogDensity=std::numeric_limits<float>::infinity();Check(!ValidateTODProfile(bad,error),"infinite parameter rejected");
        Check(!ParseTODProfile("{\"format\":\"SelfEngine.TODProfile\",\"version\":1,\"version\":1}",loaded,error),"duplicate JSON rejected");
        const auto before=SampleTODProfile(profile,1-1e-10),after=SampleTODProfile(profile,1e-10);Check(Near(before.sunIntensity,after.sunIntensity,1e-6),"cyclic key continuity");
        Check(SaveTODProfile(root/L"昼夜.tod",profile,error)&&ReadTODProfile(root/L"昼夜.tod",loaded,error),"Unicode file IO");
        Check(!SaveTODProfile(root/L"昼夜.tod",bad,error)&&ReadTODProfile(root/L"昼夜.tod",loaded,error)&&loaded.dayLengthSeconds==1,"invalid save preserves file");
        AssetStore assets(root,root/"cache");std::vector<AssetDiagnostic> diagnostics;AssetId id;Check(assets.RegisterSource(L"昼夜.tod",{},id,diagnostics),"register TOD AssetId");
        const auto lease=assets.LoadTODProfile(id,diagnostics);Check(lease&&lease==assets.LoadTODProfile(id,diagnostics),"immutable content cache");
        Environment environment;Check(environment.ConfigureClock(0.25,8),"clock");const auto actor=environment.GetWorld().Create("event observer");Check(environment.RunFrame(0),"prepare initial scene");
        auto& tod=environment.TimeOfDay();Check(tod.SelectProfile(profile),"select profile");
        const auto owner=environment.GetWorld().FindComponent(actor,ComponentTypes::Transform);
        std::vector<std::pair<std::uint64_t,TODEvent>> events;
        const auto receive=[&](const EventMessage& message,EventSubscriptionId){TODEvent value;Check(message.envelope.phase==EventPhase::AfterLogic&&DecodeTODEvent(*message.payload,value),"typed TOD wire");events.emplace_back(message.envelope.type,value);return true;};
        auto rise=environment.Events().Subscribe(owner,TODSunrise,receive),set=environment.Events().Subscribe(owner,TODSunset,receive),changed=environment.Events().Subscribe(owner,TODTimeChanged,receive);
        Check(environment.RunFrame(0)&&events.size()==1&&events[0].first==TODTimeChanged,"selection notification");events.clear();
        Check(environment.RunFrame(0.1)&&Near(tod.State().time,0)&&environment.FixedStepIndex()==0,"zero step frame");
        Check(environment.RunFrame(0.65)&&Near(tod.State().time,0.75)&&environment.FixedStepIndex()==3,"three actual steps");
        Check(events.size()==2&&events[0].first==TODSunrise&&events[1].first==TODSunset&&Near(events[0].second.simulationSeconds,0.25),"ordered unique day boundaries");
        environment.SetPaused(true);Check(environment.RunFrame(20)&&Near(tod.State().time,0.75),"pause preserves time");environment.SetPaused(false);
        Check(environment.RunFrame(0.25)&&tod.State().day==1&&Near(tod.State().time,0),"resume midnight");
        Check(tod.SetTime(2,0.9)&&tod.Advance(0.2),"display midnight fixture");const auto state=tod.State();const auto half=tod.Snapshot(31,0.5);
        Check(half.day==3&&Near(half.time,0)&&tod.State().day==state.day&&tod.State().time==state.time,"cyclic display interpolation never writes state");
        events.clear();Check(environment.RunFrame(0),"drain jump");events.clear();
        Check(tod.SetTime(-200,0.4)&&environment.RunFrame(0)&&events.size()==1&&events[0].first==TODTimeChanged,"backward jump emits only changed");
        events.clear();Check(tod.SetTime(-199,0.8,true)&&environment.RunFrame(0)&&events.size()==4,"explicit forward bounded replay");
        const auto safe=tod.State();Check(!tod.SetTime(1000000,0,true)&&tod.State().day==safe.day&&tod.State().time==safe.time,"precomputed replay budget preserves state");
        Check(!tod.SetTime(-999,0,true)&&tod.LastError()=="TOD.ReplayBackwards","backward replay rejected");
        Check(!tod.SetRate(1e100)&&tod.State().rate==1,"rate budget");bad=profile;bad.dayLengthSeconds=1e-300;
        Check(!tod.SelectProfile(bad)&&tod.Profile()->dayLengthSeconds==1,"extreme short day rejected before publish");
        Check(!tod.Advance(1e300)&&tod.State().day==safe.day,"actual step budget");
        Check(!tod.Advance(std::numeric_limits<double>::infinity())&&!tod.SetTime(0,std::numeric_limits<double>::quiet_NaN()),"nonfinite time rejected");
        Check(tod.SetRate(0)&&environment.RunFrame(1)&&tod.State().day==safe.day&&tod.State().time==safe.time,"zero rate preserves day while clock advances");
        Check(tod.SetRate(2)&&environment.RunFrame(0.25)&&Near(tod.State().time,0.3),"rate applies once");
        TODProfile budget=profile;budget.rate=128;Check(tod.SelectProfile(budget),"64 event step allowed");events.clear();Check(environment.RunFrame(0),"drain config");events.clear();
        Check(environment.RunFrame(0.25)&&events.size()==64,"exact normal step budget");Check(!tod.SetRate(129),"normal configuration budget before advance");
        const auto budgetState=tod.State();Check(!environment.ConfigureClock(0.5,8)&&environment.Clock().Step()==0.25&&tod.State().day==budgetState.day,"clock change preserves active configuration");
        Environment scenes;scenes.SetTODResolver([&](const std::string& asset,std::string& result){auto p=assets.LoadTODProfile(asset,diagnostics);if(!p)result="TOD.ResolveFailed";return p;});
        const auto first=scenes.Scenes().Create("primary");Check(scenes.Scenes().RequestDocument(first,Document(id,900))&&scenes.RunFrame(0),"scene profile activation");
        Check(scenes.TimeOfDay().Profile()&&Near(scenes.TimeOfDay().State().time,0),"main scene chooses profile");
        Check(scenes.TimeOfDay().SetTime(9,0.6),"set primary time");
        auto secondProfile=profile;secondProfile.initialTime=0.3;secondProfile.dayLengthSeconds=2;Check(SaveTODProfile(root/L"second.tod",secondProfile,error),"second resource");AssetId secondId;Check(assets.RegisterSource(L"second.tod",{},secondId,diagnostics),"second id");
        const auto additive=scenes.Scenes().Create("additive");Check(scenes.Scenes().RequestDocument(additive,Document(secondId,901),SceneLoadMode::Additive)&&scenes.RunFrame(0),"additive scene");Check(scenes.TimeOfDay().Profile()->dayLengthSeconds==1&&Near(scenes.TimeOfDay().State().time,0.6),"additive cannot steal sun");
        const auto replacement=scenes.Scenes().Create("replacement");const auto previous=scenes.GetWorld().FindByStableId(900);
        Check(scenes.Scenes().RequestDocument(replacement,Document("missing-profile",902),SceneLoadMode::Replace,first)&&scenes.RunFrame(0),"bad resource processed");
        Check(scenes.Scenes().Inspect(replacement).state==SceneState::Failed&&scenes.GetWorld().IsAlive(previous)&&Near(scenes.TimeOfDay().State().time,0.6),"bad resource preserves scene and clock");
        Check(scenes.Scenes().RequestDocument(replacement,Document(secondId,902,true),SceneLoadMode::Replace,first)&&scenes.RunFrame(0),"preserve switch");Check(scenes.TimeOfDay().State().day==9&&Near(scenes.TimeOfDay().State().time,0.6)&&scenes.TimeOfDay().Profile()->dayLengthSeconds==2,"explicit preserve policy");
        Check(scenes.Scenes().Unload(replacement)&&scenes.RunFrame(0)&&Near(scenes.TimeOfDay().State().time,0.3),"fallback main uses its declared reset policy");
        FrameData frame;BuildFrameData(scenes,nullptr,nullptr,frame);Check(frame.timeOfDay&&frame.timeOfDay->frameIndex==scenes.FrameIndex(),"actual FrameData CPU handoff");const auto snapshot=*frame.timeOfDay;
        Check(scenes.RunFrame(1.0/60)&&frame.timeOfDay->time==snapshot.time&&Near(scenes.TimeOfDay().State().simulationSeconds,scenes.SimulationSeconds()),"owned snapshot and unique simulation clock");
        auto fast=profile;fast.rate=1900;Check(SaveTODProfile(root/L"fast.tod",fast,error),"fast resource");AssetId fastId;Check(assets.RegisterSource(L"fast.tod",{},fastId,diagnostics),"fast id");
        const auto fastScene=scenes.Scenes().Create("fast additive");Check(scenes.Scenes().RequestDocument(fastScene,Document(fastId,903),SceneLoadMode::Additive)&&scenes.RunFrame(0),"budget-valid additive");
        Check(!scenes.ConfigureClock(1.0/30,8)&&Near(scenes.Clock().Step(),1.0/60),"clock change checks every active scene profile");
        std::string sceneText;Check(SerializeScene(scenes.GetWorld(),sceneText,error),"scene settings save");World restored;Check(DeserializeScene(sceneText,restored,error)&&restored.EnvironmentSettings().todProfile==secondId,"scene settings roundtrip");
        TODPreview preview;Check(preview.Open(assets,id),"editor opens profile");const auto playBefore=scenes.TimeOfDay().State();preview.SetPaused(false);Check(preview.Advance(0.2)&&preview.SetTime(0.8),"independent preview clock");
        auto edited=preview.Profile();edited.dayLengthSeconds=3;Check(preview.Apply(edited)&&preview.Dirty()&&preview.Save()&&!preview.Dirty(),"editor candidate publish save");
        Check(lease->dayLengthSeconds==1&&assets.LoadTODProfile(id,diagnostics)->dayLengthSeconds==3&&scenes.TimeOfDay().State().time==playBefore.time,"immutable old lease and Play isolation");
        EditorSession editor;Check(editor.Apply("TOD.Editor",[&](World& w){return w.SetEnvironmentSettings({secondId,true});}),"scene inspector settings transaction");Check(editor.Undo()&&editor.EditWorld().EnvironmentSettings().todProfile.empty()&&editor.Redo()&&editor.EditWorld().EnvironmentSettings().preserveTime,"scene settings undo redo");
        const auto projectRoot=output.parent_path()/asset_io::FromUtf8(asset_io::NewId());std::vector<ProjectDiagnostic> projectDiagnostics;
        Check(CreateProject("Resources/Templates/EmptyGame",projectRoot,projectDiagnostics),"project fixture");
        AssetStore projectAssets(projectRoot/"Content",projectRoot/"cache");Check(projectAssets.Scan(diagnostics)&&SaveTODProfile(projectRoot/"Content/DayNight.tod",profile,error),"project TOD resource");
        AssetId projectId;Check(projectAssets.RegisterSource("DayNight.tod",{},projectId,diagnostics),"project TOD id");
        auto manifest=nlohmann::json::parse(std::ifstream(projectRoot/"project.json"));manifest["assets"].push_back({{"id",projectId},{"type","tod"}});std::ofstream(projectRoot/"project.json")<<manifest.dump(2);
        World projectWorld;Check(LoadScene(projectRoot/"Scenes/Main.scene.json",projectWorld,error)&&projectWorld.SetEnvironmentSettings({projectId,false})&&SaveScene(projectRoot/"Scenes/Main.scene.json",projectWorld,error),"project scene reference");
        ProjectValidation validation;Check(ValidateProject(projectRoot/"project.json",validation,projectDiagnostics)&&validation.dependencies.size()==11,"project dependency closure includes profile and sidecar");
        Check(ExportProjectDependencies(projectRoot/"project.json",projectRoot/"dependencies.json",projectDiagnostics),"export dependency list");
        manifest["assets"].back()["type"]="image";std::ofstream(projectRoot/"project.json")<<manifest.dump(2);Check(!ValidateProject(projectRoot/"project.json",validation,projectDiagnostics),"wrong dependency type rejected");
        const auto stats=environment.Events().Statistics();Check(!stats.callbackFailures&&!tod.NotificationFailures(),"no TOD callback or queue failures");
        nlohmann::json proof{{"success",true},{"profileRoundtrip",true},{"immutableCache",true},{"zeroMultiPause",true},{"normalEventBudget",64},{"boundedExplicitReplay",true},{"invalidPreserves",true},{"midnightInterpolation",true},{"sceneAtomicity",true},{"primaryOnly",true},{"frameData",true},{"previewIsolated",true},{"sceneUndoRedo",true},{"allActiveProfilesPreflight",true},{"projectDependencyClosure",true}};
        std::ofstream(output)<<proof.dump(2);std::cout<<"TOD profile, clock, events, scene, snapshot and preview passed\n";return 0;
    }catch(const std::exception& e){std::cerr<<e.what()<<'\n';return 1;}
}
