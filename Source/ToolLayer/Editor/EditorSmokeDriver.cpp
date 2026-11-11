#include "EditorSmokeDriver.h"
#include "EditorSession.h"
#include "ImGuiEditor.h"
#include "ResourceLayer/Scenes/SceneSerializer.h"
#include "ResourceLayer/Assets/AssetIO.h"
#include <Windows.h>
#include <Psapi.h>
#include <nlohmann/json.hpp>
#include <charconv>
#include <chrono>
#include <cmath>
#include <fstream>
#include <string>
#include <system_error>
#include <utility>
#include <algorithm>

namespace se
{
namespace
{
using Json=nlohmann::json;
namespace fs=std::filesystem;
std::wstring EnvironmentText(const wchar_t* key)
{
    const auto count=GetEnvironmentVariableW(key,nullptr,0);if(count==0||count>32768)return {};
    std::wstring value(count,L'\0');const auto read=GetEnvironmentVariableW(key,value.data(),count);
    if(read==0||read>=count)return {};value.resize(read);return value;
}
std::string Utf8(const fs::path& path)
{const auto text=path.generic_u8string();return {reinterpret_cast<const char*>(text.data()),text.size()};}
fs::path FromUtf8(const std::string& text)
{return fs::path(std::u8string(reinterpret_cast<const char8_t*>(text.data()),text.size()));}
Json ReadJson(const fs::path& path)
{
    std::error_code error;
    if(!fs::is_regular_file(path,error)||fs::file_size(path,error)>8*1024*1024)return {};
    std::ifstream input(path,std::ios::binary);if(!input)return {};
    try{return Json::parse(input);}catch(...){return {};}
}
bool WriteReport(const fs::path& path,const Json& report)
{
    if(path.empty())return false;
    std::error_code error;fs::create_directories(path.parent_path(),error);if(error)return false;
    const auto temporary=path.wstring()+L".smoke.tmp";
    {std::ofstream output(temporary,std::ios::binary|std::ios::trunc);if(!output)return false;output<<report.dump(2)<<"\n";if(!output)return false;}
    return MoveFileExW(temporary.c_str(),path.c_str(),MOVEFILE_REPLACE_EXISTING|MOVEFILE_WRITE_THROUGH)!=FALSE;
}
Json TransformValue(const Transform& value)
{
    return Json{{"position",{value.position.x,value.position.y,value.position.z}},
        {"rotation",{value.rotation.x,value.rotation.y,value.rotation.z,value.rotation.w}},
        {"scale",{value.scale.x,value.scale.y,value.scale.z}}};
}
Json EntityValue(const World* world,StableEntityId id)
{
    Json value{{"id",std::to_string(id)},{"alive",false},{"enabled",false},{"script_components",0},{"transform",nullptr}};
    if(!world||!id)return value;
    const auto handle=world->FindByStableId(id);value["alive"]=world->IsAlive(handle);value["enabled"]=world->IsEnabled(handle);
    if(const auto transform=world->GetTransform(handle))value["transform"]=TransformValue(*transform);
    std::size_t count=0;for(const auto component:world->Components(handle))if(world->GetScriptComponent(component))++count;
    value["script_components"]=count;return value;
}
StableEntityId SpinEntity(const fs::path& root)
{
    try
    {
        const auto build=ReadJson(root/"build.json");if(!build.is_object())return 0;
        const auto relative=FromUtf8(build.value("bindings",std::string{}));if(relative.empty()||relative.is_absolute())return 0;
        const auto path=(root/relative).lexically_normal(),check=path.lexically_relative(root.lexically_normal());
        if(check.empty()||*check.begin()==L"..")return 0;
        const auto bindings=ReadJson(path);if(!bindings.is_object()||!bindings.contains("Scripts"))return 0;
        for(const auto& script:bindings.at("Scripts"))
        {
            if(script.value("Type",std::string{})!="emptygame.spin"||!script.value("Enabled",true))continue;
            const auto text=script.at("Entity").get<std::string>();StableEntityId id=0;
            const auto parsed=std::from_chars(text.data(),text.data()+text.size(),id);
            if(parsed.ec==std::errc{}&&parsed.ptr==text.data()+text.size())return id;
        }
    }
    catch(...){}
    return 0;
}
bool RotationChanged(const Json& before,const Json& after)
{
    if(!before.is_object()||!after.is_object()||before["transform"].is_null()||after["transform"].is_null())return false;
    double difference=0;
    for(std::size_t index=0;index<4;++index)difference+=std::abs(before["transform"]["rotation"][index].get<double>()-after["transform"]["rotation"][index].get<double>());
    return difference>1e-7;
}
bool SamePose(const Json& before,const Json& after)
{return before["alive"]==after["alive"]&&before["enabled"]==after["enabled"]&&before["transform"]==after["transform"];}
bool NoReparse(const fs::path& path)
{
    for(auto current=fs::absolute(path).lexically_normal();!current.empty();current=current.parent_path())
    {
        const auto attributes=GetFileAttributesW(current.c_str());
        if(attributes!=INVALID_FILE_ATTRIBUTES&&(attributes&FILE_ATTRIBUTE_REPARSE_POINT))return false;
        if(current==current.root_path())break;
    }
    return true;
}
Json Resources()
{
    PROCESS_MEMORY_COUNTERS_EX memory{};memory.cb=sizeof(memory);DWORD handles=0;
    const auto process=GetCurrentProcess();const bool memoryRead=K32GetProcessMemoryInfo(process,reinterpret_cast<PROCESS_MEMORY_COUNTERS*>(&memory),sizeof(memory))!=FALSE;
    GetProcessHandleCount(process,&handles);
    return Json{{"process_id",GetCurrentProcessId()},{"handles",handles},{"working_set",memory.WorkingSetSize},{"private_bytes",memory.PrivateUsage},
        {"memory_read",memoryRead},{"gdi_objects",GetGuiResources(process,GR_GDIOBJECTS)},{"user_objects",GetGuiResources(process,GR_USEROBJECTS)}};
}
std::uint64_t UnsignedEnvironment(const wchar_t* name)
{
    const auto value=Utf8(fs::path(EnvironmentText(name)));std::uint64_t result=0;
    const auto parsed=std::from_chars(value.data(),value.data()+value.size(),result);
    return parsed.ec==std::errc{}&&parsed.ptr==value.data()+value.size()?result:0;
}
double RotationAngle(const Json& before,const Json& after)
{
    double dot=0,left=0,right=0;
    for(std::size_t index=0;index<4;++index)
    {
        const auto a=before["transform"]["rotation"][index].get<double>(),b=after["transform"]["rotation"][index].get<double>();
        dot+=a*b;left+=a*a;right+=b*b;
    }
    return left>0&&right>0?2*std::acos(std::clamp(std::abs(dot/std::sqrt(left*right)),0.0,1.0)):-1;
}
}
bool EditorSmokeDriver::ValidateWorkflow(const fs::path& projectRoot,const fs::path& config,std::size_t& stageCount,std::string& detail)
{
    stageCount=0;detail="Editor.WorkflowSmokeInvalid";
    try
    {
        const auto root=fs::absolute(projectRoot).lexically_normal(),file=fs::absolute(config).lexically_normal();
        if(file.filename()!=L"workflow.json"||file.parent_path().parent_path()!=root/"out/Editor/workflow-smoke"||!NoReparse(file)
            ||!NoReparse(root/"Scripts/Spin.cs")||!fs::is_regular_file(root/"Scripts/Spin.cs"))return false;
        const auto value=ReadJson(file);
        if(!value.is_object()||value.value("schemaVersion",0)!=1||value.value("kind",std::string{})!="SelfEngine.EditorWorkflowSmoke"
            ||fs::absolute(FromUtf8(value.at("projectRoot").get<std::string>())).lexically_normal()!=root)return false;
        const auto& stages=value.at("stages");if(!stages.is_array()||stages.empty()||stages.size()>21)return false;
        std::size_t plays=0;
        for(std::size_t index=0;index<stages.size();++index)
        {
            const auto& stage=stages[index];const auto mode=stage.value("mode",std::string("playback"));
            if(mode=="playback")
            {
                const auto cycles=stage.value("playCycles",1);const auto speed=stage.value("radiansPerSecond",1.0);
                if(cycles<1||cycles>20||!std::isfinite(speed)||speed<=0||speed>16)return false;
                plays+=static_cast<std::size_t>(cycles);if(plays>40)return false;
                const auto expected=stage.value("expectedBuild",std::string("ready"));if(expected!="ready"&&expected!="stale")return false;
                const auto patch=FromUtf8(stage.value("patchAfter",std::string{}));
                if(index+1<stages.size())
                {
                    if(patch.empty()||patch.has_parent_path()||patch.extension()!=L".txt"||!NoReparse(file.parent_path()/patch)
                        ||!fs::is_regular_file(file.parent_path()/patch)||fs::file_size(file.parent_path()/patch)>1024*1024
                        ||stage.value("sceneEditName",std::string{}).empty())return false;
                }
                else if(!patch.empty())return false;
            }
            else if(mode=="export")
            {
                const auto name=stage.value("exportName",std::string{}),expected=stage.value("expectedResult",std::string{});
                if(stages.size()!=1||name.empty()||name.size()>100||name.find_first_of("<>:\"/\\|?*")!=std::string::npos
                    ||(expected!="succeeded"&&expected!="failed"&&expected!="cancelled"))return false;
            }
            else return false;
        }
        stageCount=stages.size();detail.clear();return true;
    }
    catch(...){return false;}
}
struct EditorSmokeDriver::State
{
    enum class Phase{Warmup,PausedWait,StepWait,Done};
    fs::path projectRoot,reportPath;
    std::string exportName,detail,editSnapshot;
    bool playback=false,managed=false,started=false,done=false,success=false,reportWritten=false,cancelRequested=false;
    std::uint64_t cancelMilliseconds=0,frames=0,phaseFrame=0,fixedBefore=0,fixedAfter=0;
    StableEntityId selection=0,spinEntity=0;
    bool dirtyBefore=false;
    bool workflow=false,workflowPrepared=false;
    std::size_t workflowStage=0,completedCycles=0;
    std::uint64_t warmupFrame=0,cancelTick=0,lastExportTick=0;
    fs::path workflowConfig;
    Json workflowSettings;
    double stepSeconds=0;
    std::size_t undoBefore=0,redoBefore=0;
    Phase phase=Phase::Warmup;
    std::chrono::steady_clock::time_point began=std::chrono::steady_clock::now();
    Json report=Json::object(),spinBefore,spinAfter;
    std::function<EditorCommandOutcome(std::string_view)> execute;
    std::function<void()> requestClose;

    void Save(){report["frames"]=frames;report["success"]=success;reportWritten=WriteReport(reportPath,report);}
    bool Command(ImGuiEditor& frontend,const char* id)
    {
        const auto outcome=execute?execute(id):frontend.ExecuteCommand(id);
        report["commands"].push_back(Json{{"id",id},{"succeeded",outcome.state==EditorCommandResult::Succeeded},{"diagnostic_key",outcome.diagnosticKey}});
        if(outcome.state==EditorCommandResult::Succeeded)return true;
        detail=outcome.diagnosticKey.empty()?"Editor.PlaybackInvalidState":outcome.diagnosticKey;return false;
    }
    void Finish(ImGuiEditor& frontend,EditorSession& session,bool passed,const std::string& check={})
    {
        success=passed;done=true;phase=Phase::Done;
        if(session.IsPlaying()&&!Command(frontend,"editor.stop")){success=false;report["failure_check"]="stop_command";}
        if(!check.empty())report["failure_check"]=check;
        if(!detail.empty())report["detail"]=detail;
        Save();if(requestClose)requestClose();else frontend.RequestClose();
    }
    bool RejectedPlay(ImGuiEditor& frontend)
    {
        const auto outcome=execute?execute("editor.play"):frontend.ExecuteCommand("editor.play");
        report["commands"].push_back(Json{{"id","editor.play"},{"succeeded",outcome.state==EditorCommandResult::Succeeded},{"diagnostic_key",outcome.diagnosticKey},{"expected_rejection",true}});
        return outcome.state!=EditorCommandResult::Succeeded&&outcome.diagnosticKey=="Editor.BuildRequired";
    }
    bool PrepareWorkflow(ImGuiEditor& frontend,EditorSession& session)
    {
        workflowPrepared=true;report["ready_resources"]=Resources();
        const auto hostStart=UnsignedEnvironment(L"SELFENGINE_EDITOR_SMOKE_HOST_START");
        report["ready_elapsed_ms"]=hostStart&&GetTickCount64()>=hostStart?GetTickCount64()-hostStart:0;
        const auto supervisorStart=UnsignedEnvironment(L"SELFENGINE_EDITOR_SMOKE_SUPERVISOR_START");
        report["open_elapsed_ms"]=supervisorStart&&GetTickCount64()>=supervisorStart?GetTickCount64()-supervisorStart:0;
        report["scene_path"]=Utf8(session.Path());report["selection_on_open"]=std::to_string(session.Selected());
        const auto status=ReadJson(fs::path(EnvironmentText(L"SELFENGINE_EDITOR_BUILD_STATUS")));
        report["build_status"]=status;
        if(!managed||!status.is_object()||!status.value("hasCandidate",false)
            ||status.value("state",std::string{})!=workflowSettings.value("expectedBuild",std::string("ready")))return false;
        std::vector<std::uint8_t> assembly;
        if(!asset_io::Read(FromUtf8(status.value("assembly",std::string{})),assembly))return false;
        report["assembly_sha256"]=asset_io::Hash(assembly);
        const auto expectedName=workflowSettings.value("expectedSceneName",std::string{});
        if(!expectedName.empty())
        {
            const auto handle=session.EditWorld().FindByStableId(spinEntity);const auto* entity=session.EditWorld().Get(handle);
            const bool restored=entity&&entity->name==expectedName&&session.Selected()==spinEntity&&!session.Dirty();
            report["saved_scene_and_selection_restored"]=restored;if(!restored)return false;
        }
        if(status.value("state",std::string{})=="stale")
        {
            const bool blocked=RejectedPlay(frontend)&&!session.IsPlaying();report["stale_play_rejected"]=blocked;
            if(!blocked||!Command(frontend,"editor.use-last-build"))return false;
            report["old_build_explicitly_authorized"]=true;
        }
        return true;
    }
    void CompleteWorkflowPlayback(ImGuiEditor& frontend,EditorSession& session)
    {
        report["cycles"].push_back(Json{{"index",completedCycles},{"fixed_step_before",fixedBefore},{"fixed_step_after",fixedAfter},
            {"radians",report["spin"].value("radians",0.0)},{"expected_radians",workflowSettings.value("radiansPerSecond",1.0)*stepSeconds},
            {"resources",Resources()},{"play_world_released",session.PlayWorld()==nullptr},{"edit_snapshot_unchanged",report["edit_snapshot_unchanged"]}});
        ++completedCycles;
        if(completedCycles<workflowSettings.value("playCycles",std::size_t{1}))
        {
            phase=Phase::Warmup;warmupFrame=frames;report["paused_wait_samples"]=Json::array();report["after_step_samples"]=Json::array();return;
        }
        const auto patch=workflowSettings.value("patchAfter",std::string{});
        if(patch.empty()){Finish(frontend,session,true);return;}
        std::vector<std::uint8_t> bytes;
        const auto source=projectRoot/"Scripts/Spin.cs",replacement=workflowConfig.parent_path()/FromUtf8(patch);
        if(!NoReparse(source)||!NoReparse(replacement)||!asset_io::Read(replacement,bytes)||bytes.empty()||bytes.size()>1024*1024
            ||!asset_io::WriteAtomic(source,bytes))
        {Finish(frontend,session,false,"source_patch");return;}
        report["patched_source_sha256"]=asset_io::Hash(bytes);
        if(workflowSettings.value("expectedBuild",std::string("ready"))=="ready")
        {
            const bool blocked=RejectedPlay(frontend)&&!session.IsPlaying();report["changed_source_play_rejected"]=blocked;
            if(!blocked){Finish(frontend,session,false,"changed_source_not_blocked");return;}
        }
        const auto name=workflowSettings.at("sceneEditName").get<std::string>();
        if(!session.Rename(spinEntity,name)||!session.Select(spinEntity)||!session.Dirty())
        {Finish(frontend,session,false,"prepare_dirty_scene");return;}
        report["dirty_scene_before_compile"]=true;report["saved_scene_name"]=name;
        if(!Command(frontend,"editor.compile")||session.Dirty())
        {Finish(frontend,session,false,"compile_command_save");return;}
        // 编译命令自己保存并请求关闭；驱动不得写 restart.json 或另起候选进程。
        report["compile_requested"]=true;report["scene_clean_after_compile"]=!session.Dirty();
        success=true;done=true;phase=Phase::Done;Save();
    }
    void PlaybackFrame(ImGuiEditor& frontend,EditorSession& session)
    {
        if(phase==Phase::Warmup)
        {
            if(frames-warmupFrame<10)return;
            if(workflow&&!workflowPrepared&&!PrepareWorkflow(frontend,session)){Finish(frontend,session,false,"workflow_host_state");return;}
            // 无既有选择的固定夹具也验证一个真实稳定 ID，避免只比较两个空选择。
            if(session.Selected()==0)
            {
                auto selected=spinEntity;
                if(!selected)for(const auto entity:session.EditWorld().Entities())if(const auto record=session.EditWorld().Get(entity)){selected=record->stableId;break;}
                if(selected)report["selection_prepared"]=session.Select(selected);
            }
            if(!SerializeScene(session.EditWorld(),editSnapshot,detail)){Finish(frontend,session,false,"capture_edit_snapshot");return;}
            selection=session.Selected();dirtyBefore=session.Dirty();undoBefore=session.UndoCount();redoBefore=session.RedoCount();
            report["selection_before"]=std::to_string(selection);report["edit_entities"]=session.EditWorld().Size();
            if(!Command(frontend,"editor.play")){Finish(frontend,session,false,"play_command");return;}
            if(!session.IsPlaying()||!session.PlayEnvironment()){Finish(frontend,session,false,"play_world_missing");return;}
            report["runtime_entities"]=session.PlayWorld()->Size();
            if(!Command(frontend,"editor.pause")){Finish(frontend,session,false,"pause_command");return;}
            fixedBefore=session.PlayEnvironment()->FixedStepIndex();report["fixed_step_before"]=fixedBefore;
            stepSeconds=session.PlayFixedStep();
            spinBefore=EntityValue(session.PlayWorld(),spinEntity);
            report["spin"]["before"]=spinBefore;
            phase=Phase::PausedWait;phaseFrame=frames;return;
        }
        if(!session.IsPlaying()||!session.PlayEnvironment()){Finish(frontend,session,false,"runtime_ended_early");return;}
        const auto fixed=session.PlayEnvironment()->FixedStepIndex();
        if(phase==Phase::PausedWait)
        {
            report["paused_wait_samples"].push_back(fixed);
            if(!session.PlayPaused()||fixed!=fixedBefore){Finish(frontend,session,false,"paused_clock_advanced");return;}
            if(spinEntity&&!SamePose(EntityValue(session.PlayWorld(),spinEntity),spinBefore)){Finish(frontend,session,false,"paused_spin_changed");return;}
            if(frames-phaseFrame<5)return;
            report["paused_wait_stable"]=true;
            if(!Command(frontend,"editor.step")){Finish(frontend,session,false,"step_command");return;}
            phase=Phase::StepWait;phaseFrame=frames;return;
        }
        if(phase==Phase::StepWait)
        {
            fixedAfter=fixed;report["fixed_step_after"]=fixedAfter;report["paused_after_step"]=session.PlayPaused();
            report["after_step_samples"].push_back(fixedAfter);
            if(!session.PlayPaused()||fixedAfter!=fixedBefore+1){Finish(frontend,session,false,"single_step_count");return;}
            const auto currentSpin=EntityValue(session.PlayWorld(),spinEntity);
            if(frames-phaseFrame==1)spinAfter=currentSpin;
            else if(spinEntity&&!SamePose(currentSpin,spinAfter)){Finish(frontend,session,false,"paused_spin_changed_after_step");return;}
            if(frames-phaseFrame<4)return;
            report["spin"]["after"]=spinAfter;
            bool spinPassed=!spinEntity||(spinBefore.value("alive",false)&&spinAfter.value("alive",false)
                &&spinAfter.value("enabled",false)&&spinAfter.value("script_components",std::size_t{0})>0&&RotationChanged(spinBefore,spinAfter));
            report["spin"]["changed"]=spinEntity&&RotationChanged(spinBefore,spinAfter);
            if(workflow&&spinPassed&&spinEntity)
            {
                const auto angle=RotationAngle(spinBefore,spinAfter),expected=workflowSettings.value("radiansPerSecond",1.0)*stepSeconds;
                report["spin"]["radians"]=angle;report["spin"]["expected_radians"]=expected;
                spinPassed=std::isfinite(angle)&&std::abs(angle-expected)<=std::max(0.0001,expected*0.03);
            }
            if(!Command(frontend,"editor.stop")){Finish(frontend,session,false,"stop_command");return;}
            std::string after,error;const bool captured=SerializeScene(session.EditWorld(),after,error);
            const bool editSame=captured&&after==editSnapshot;
            const bool selectedSame=session.Selected()==selection;
            const bool historySame=session.UndoCount()==undoBefore&&session.RedoCount()==redoBefore&&session.Dirty()==dirtyBefore;
            report["edit_snapshot_unchanged"]=editSame;report["selection_unchanged"]=selectedSame;
            report["selection_after"]=std::to_string(session.Selected());report["edit_history_unchanged"]=historySame;
            report["play_world_released"]=session.PlayWorld()==nullptr;
            const bool passed=spinPassed&&editSame&&selectedSame&&historySame&&!session.IsPlaying();
            if(workflow&&passed){CompleteWorkflowPlayback(frontend,session);return;}
            Finish(frontend,session,passed,!spinPassed?"spin_transform_unchanged":!editSame?"edit_snapshot_changed":!selectedSame?"selection_changed":!historySame?"edit_history_changed":session.IsPlaying()?"play_world_not_released":"");
        }
    }
    void ExportFrame(ImGuiEditor& frontend,EditorSession& session)
    {
        if(!started)
        {
            if(frames<10)return;
            if(workflow&&(!PrepareWorkflow(frontend,session)||!Command(frontend,"editor.save-all")))
            {Finish(frontend,session,false,"export_prepare");return;}
            began=std::chrono::steady_clock::now();started=true;
            if(!frontend.StartExport(exportName,detail)){ExportReport(frontend);Finish(frontend,session,false,"export_start");}
            return;
        }
        const auto elapsed=std::chrono::steady_clock::now()-began;
        if(workflow)
        {
            const auto tick=GetTickCount64();
            if(lastExportTick)report["maximum_ui_frame_gap_ms"]=std::max(report.value("maximum_ui_frame_gap_ms",std::uint64_t{0}),tick-lastExportTick);
            lastExportTick=tick;
            const auto snapshot=frontend.ExportSnapshot();const auto raw=ReadJson(snapshot.reportPath);
            const auto state=std::string(ExportOperationStateId(snapshot.report.state));
            if(report.value("observed_state",std::string{})!=state||report.value("observed_stage",std::string{})!=snapshot.report.stage)
            {
                report["progress"].push_back(Json{{"elapsed_ms",std::chrono::duration_cast<std::chrono::milliseconds>(elapsed).count()},{"state",state},{"stage",snapshot.report.stage}});
                report["observed_state"]=state;report["observed_stage"]=snapshot.report.stage;
            }
            if(!cancelRequested&&workflowSettings.value("expectedResult",std::string{})=="cancelled"&&raw.is_object())
            {
                const auto stage=FromUtf8(raw.value("staging",std::string{}));std::error_code error;
                if(!stage.empty()&&fs::is_directory(stage,error))
                {
                    report["cancel_stage_existed"]=true;report["cancel_stage"]=Utf8(stage);cancelTick=tick;cancelRequested=true;frontend.RequestCancelExport();
                }
            }
            if(cancelRequested&&!report.contains("cancelling_latency_ms")&&(state=="Cancelling"||state=="Cancelled"||state=="cancelling"||state=="cancelled"))
                report["cancelling_latency_ms"]=tick-cancelTick;
        }
        if(!cancelRequested&&cancelMilliseconds>0&&std::chrono::duration_cast<std::chrono::milliseconds>(elapsed).count()>=static_cast<long long>(cancelMilliseconds))
        {cancelRequested=true;frontend.RequestCancelExport();}
        if(frontend.ExportFinished())
        {
            const auto snapshot=frontend.ExportSnapshot();ExportReport(frontend);
            bool passed=snapshot.report.state==ExportOperationState::Succeeded;
            if(workflow)
            {
                const auto expected=workflowSettings.value("expectedResult",std::string{});
                passed=expected=="succeeded"?passed:expected=="failed"?snapshot.report.state==ExportOperationState::Failed
                    :snapshot.report.state==ExportOperationState::Cancelled&&cancelRequested&&report.value("cancel_stage_existed",false);
                if(cancelTick)report["cancel_cleanup_ms"]=GetTickCount64()-cancelTick;
            }
            Finish(frontend,session,passed,passed?"":"export_failed");return;
        }
        if(elapsed>=std::chrono::minutes(15))
        {detail="Editor.ExportTimeout";frontend.RequestCancelExport();ExportReport(frontend);Finish(frontend,session,false,"export_timeout");}
    }
    void ExportReport(ImGuiEditor& frontend)
    {
        const auto snapshot=frontend.ExportSnapshot();
        report["start_error"]=detail;report["state"]=ExportOperationStateId(snapshot.report.state);report["stage"]=snapshot.report.stage;
        report["diagnostic_key"]=snapshot.report.diagnosticKey;report["exit_code"]=snapshot.report.exitCode;report["log_path"]=snapshot.report.logPath;
        report["output_package"]=Utf8(snapshot.outputPackage);report["report_path"]=Utf8(snapshot.reportPath);report["cancelled"]=snapshot.cancelled;
        if(workflow)report["export_report"]=ReadJson(snapshot.reportPath);
    }
};
EditorSmokeDriver::EditorSmokeDriver(std::unique_ptr<State> state):state_(std::move(state)){}
EditorSmokeDriver::~EditorSmokeDriver()=default;
EditorSmokeDriver::EditorSmokeDriver(EditorSmokeDriver&&) noexcept=default;
EditorSmokeDriver& EditorSmokeDriver::operator=(EditorSmokeDriver&&) noexcept=default;
EditorSmokeDriver EditorSmokeDriver::FromEnvironment(const fs::path& projectRoot)
{
    const auto report=EnvironmentText(L"SELFENGINE_EDITOR_SMOKE_REPORT"),name=EnvironmentText(L"SELFENGINE_EDITOR_SMOKE_EXPORT");
    const bool playback=EnvironmentText(L"SELFENGINE_EDITOR_SMOKE_PLAYBACK")==L"1";
    const auto workflow=fs::path(EnvironmentText(L"SELFENGINE_EDITOR_SMOKE_WORKFLOW"));
    if(report.empty()&&name.empty()&&!playback&&workflow.empty())return EditorSmokeDriver(nullptr);
    auto state=std::make_unique<State>();state->projectRoot=projectRoot;state->reportPath=report;state->exportName=Utf8(fs::path(name));state->playback=playback;
    state->managed=EnvironmentText(L"SELFENGINE_EDITOR_MANAGED_HOST")==L"1";
    const auto cancel=Utf8(fs::path(EnvironmentText(L"SELFENGINE_EDITOR_SMOKE_CANCEL_MS")));
    if(!cancel.empty())
    {
        const auto parsed=std::from_chars(cancel.data(),cancel.data()+cancel.size(),state->cancelMilliseconds);
        if(parsed.ec!=std::errc{}||parsed.ptr!=cancel.data()+cancel.size()||state->cancelMilliseconds>600000)state->detail="Editor.PlaybackInvalidState";
    }
    if(report.empty()||playback==!name.empty())state->detail="Editor.PlaybackInvalidState";
    if(!workflow.empty())
    {
        state->workflow=true;state->workflowConfig=workflow;state->workflowStage=UnsignedEnvironment(L"SELFENGINE_EDITOR_SMOKE_WORKFLOW_STAGE");
        std::size_t count=0;
        if(ValidateWorkflow(projectRoot,workflow,count,state->detail)&&state->workflowStage<count)
        {
            state->workflowSettings=ReadJson(workflow).at("stages")[state->workflowStage];
            state->playback=state->workflowSettings.value("mode",std::string("playback"))=="playback";
            state->exportName=state->workflowSettings.value("exportName",std::string{});
            state->reportPath=workflow.parent_path()/("stage-"+std::to_string(state->workflowStage)+".json");
        }
        else state->detail="Editor.WorkflowSmokeInvalid";
    }
    state->spinEntity=state->playback?SpinEntity(projectRoot):0;
    state->report=Json{{"schema",1},{"smoke_contract",2},{"success",false},{"frames",0},{"command_driver",true},{"manual_input_verified",false},
        {"managed_host",state->managed},{"host_exit_code",nullptr},{"commands",Json::array()},{"failure_check",""},{"detail",""}};
    state->report["process_id"]=GetCurrentProcessId();state->report["cycles"]=Json::array();
    if(state->workflow)
    {
        state->report["workflow_stage"]=state->workflowStage;state->report["workflow_config"]=Utf8(workflow);
        state->report["supervisor_process_id"]=UnsignedEnvironment(L"SELFENGINE_EDITOR_SMOKE_SUPERVISOR_PID");
        state->report["progress"]=Json::array();state->report["gpu_reference_counts_measured"]=false;state->report["managed_alc_counts_measured"]=false;
    }
    if(state->playback)
    {
        state->report.update(Json{{"fixed_step_before",0},{"fixed_step_after",0},{"paused_after_step",false},{"paused_wait_stable",false},
            {"paused_wait_samples",Json::array()},{"after_step_samples",Json::array()},{"edit_snapshot_unchanged",false},{"selection_unchanged",false},
            {"spin",Json{{"applicable",state->spinEntity!=0},{"entity",std::to_string(state->spinEntity)},{"changed",false}}}});
    }
    return EditorSmokeDriver(std::move(state));
}
bool EditorSmokeDriver::Enabled()const noexcept{return state_!=nullptr;}
void EditorSmokeDriver::OperationFrame(ImGuiEditor& frontend,EditorSession& session,
    std::function<EditorCommandOutcome(std::string_view)> execute,std::function<void()> requestClose)
{
    if(!state_||state_->done)return;++state_->frames;
    state_->execute=std::move(execute);state_->requestClose=std::move(requestClose);
    if(!state_->detail.empty()){state_->Finish(frontend,session,false,"invalid_smoke_options");return;}
    try{if(state_->playback)state_->PlaybackFrame(frontend,session);else state_->ExportFrame(frontend,session);}
    catch(const std::exception& error){state_->report["exception"]=error.what();state_->Finish(frontend,session,false,"driver_exception");}
}
int EditorSmokeDriver::FinalResult(int hostExit)
{
    if(!state_)return hostExit;
    if(!state_->done){state_->success=false;state_->report["failure_check"]="host_ended_before_smoke";}
    if(hostExit!=0){state_->success=false;state_->report["smoke_failure_check"]=state_->report["failure_check"];state_->report["failure_check"]="host_exit";}
    if(state_->workflow)
    {
        state_->report["final_resources"]=Resources();
        if(state_->report.value("compile_requested",false))
        {
            const auto request=ReadJson(fs::path(EnvironmentText(L"SELFENGINE_EDITOR_RESTART_REQUEST")));
            const bool published=request.is_object()&&request.value("schemaVersion",0)==1&&request.value("action",std::string{})=="compile";
            state_->report["restart_published_after_close"]=published;
            if(!published){state_->success=false;state_->report["failure_check"]="normal_close_did_not_publish_restart";}
        }
    }
    state_->report["host_exit_code"]=hostExit;state_->Save();
    return hostExit!=0?hostExit:state_->done&&state_->success&&state_->reportWritten?0:1;
}
}
