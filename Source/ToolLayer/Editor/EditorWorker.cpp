#include "EditorWorker.h"
#include "EditorLoadingScreen.h"
#include "ProjectWorkspaceSession.h"
#include "EditorApplication.h"
#include "EditorSession.h"
#include "ImGuiEditor.h"
#include "EditorProcessRunner.h"
#if defined(FLARE_LOCAL_VALIDATION)
#include "EditorSmokeDriver.h"
#endif
#include "CoreLayer/Hash.h"
#include "CoreLayer/Localization.h"
#include "CoreLayer/Log.h"
#include "PlatformLayer/Windows/ExecutablePath.h"
#include "EditorRenderer.h"
#include "FunctionLayer/Rendering/SandboxRenderer.h"
#include "ResourceLayer/Assets/Primitives.h"
#include "ResourceLayer/Assets/AssetIO.h"
#include <nlohmann/json.hpp>
#include <Windows.h>
#include <Psapi.h>
#include <objbase.h>
#include <algorithm>
#include <atomic>
#include <fstream>
#include <iostream>
#include <iterator>
#include <set>

namespace se
{
namespace
{
namespace fs=std::filesystem;
using Json=nlohmann::json;
std::string PathText(const fs::path& path)
{const auto value=path.generic_u8string();return {reinterpret_cast<const char*>(value.data()),value.size()};}
fs::path TextPath(const std::string& text)
{return fs::path(std::u8string(reinterpret_cast<const char8_t*>(text.data()),text.size()));}
std::wstring WideText(const std::string& text)
{return TextPath(text).wstring();}
Json ReadJson(const fs::path& path)
{
    std::error_code error;
    if(!fs::is_regular_file(path,error)||fs::file_size(path,error)>8*1024*1024)return {};
    std::ifstream input(path,std::ios::binary);if(!input)return {};
    try{return Json::parse(input);}catch(...){return {};}
}
std::string FileHash(const fs::path& path)
{
    std::ifstream input(path,std::ios::binary);if(!input)return {};
    const std::string bytes{std::istreambuf_iterator<char>(input),{}};
    return input.bad()?std::string{}:Sha256(bytes);
}
bool Within(const fs::path& path,const fs::path& root)
{
    const auto candidate=fs::absolute(path).lexically_normal().wstring();
    auto prefix=fs::absolute(root).lexically_normal().wstring();if(prefix.back()!=L'\\')prefix+=L'\\';
    return candidate.size()>prefix.size()&&_wcsnicmp(candidate.c_str(),prefix.c_str(),prefix.size())==0;
}
bool WriteJsonAtomic(const fs::path& path,const Json& json)
{
    const auto temporary=path.wstring()+L".tmp";
    {std::ofstream output(temporary,std::ios::binary|std::ios::trunc);if(!output)return false;output<<json.dump(2)<<"\n";if(!output)return false;}
    return MoveFileExW(temporary.c_str(),path.c_str(),MOVEFILE_REPLACE_EXISTING|MOVEFILE_WRITE_THROUGH)!=FALSE;
}
std::wstring Localized(const Localization& strings,const char* key)
{const auto value=strings.GetText(key);return WideText(value.empty()?key:value);}
void TickBuildProgress(const fs::path& operation)
{
    const auto progress=ReadJson(operation/"build-progress.json");if(!progress.is_object())return;
    const auto phase=progress.value("phase",std::string{});
    if(phase=="restore_host")ReportStartup(StartupStage::RestoreHost);
    else if(phase=="publish_host")ReportStartup(StartupStage::PublishHost);
    else if(phase=="verify_runtime")ReportStartup(StartupStage::VerifyRuntime);
    else if(phase=="script_name"||phase=="restore_scripts")ReportStartup(StartupStage::RestoreScripts);
    else if(phase=="build_scripts")ReportStartup(StartupStage::BuildScripts);
    else if(phase=="inspect_scripts"||phase=="ready"||phase=="reused")ReportStartup(StartupStage::ValidateScripts);
}
bool ValidCandidate(const fs::path& root,const fs::path& operations,const fs::path& project,const fs::path& native,const std::string& configuration,Json& manifest)
{
    try
    {
        if(!Within(root,operations))return false;
        manifest=ReadJson(root/"candidate.json");
        if(manifest.value("schemaVersion",0)!=1||manifest.value("kind",std::string{})!="SelfEngine.EditorScriptCandidate"
            ||manifest.value("configuration",std::string{})!=configuration||TextPath(manifest.value("projectRoot",std::string{}))!=project)return false;
        std::set<fs::path> listed;
        for(const auto& file:manifest.at("files"))
        {
            const auto relative=TextPath(file.at("path").get<std::string>());const auto path=root/relative;
            if(relative.is_absolute()||!Within(path,root)||!listed.insert(relative).second||fs::file_size(path)!=file.at("bytes").get<std::uint64_t>())return false;
            if(FileHash(path)!=file.at("sha256").get<std::string>())return false;
        }
        for(const auto& entry:fs::recursive_directory_iterator(root))
        {
            if((GetFileAttributesW(entry.path().c_str())&FILE_ATTRIBUTE_REPARSE_POINT)!=0)return false;
            if(entry.is_regular_file()&&entry.path().filename()!=L"candidate.json"&&!listed.contains(fs::relative(entry.path(),root)))return false;
        }
        const auto runtime=TextPath(manifest.at("runtimeExecutable").get<std::string>()),assembly=TextPath(manifest.at("assembly").get<std::string>());
        return listed.contains(runtime)&&listed.contains(assembly)&&FileHash(root/"Runtime/EngineNative.dll")==FileHash(native);
    }
    catch(...){return false;}
}
fs::path NewOperation(const fs::path& operations)
{
    for(auto directory=operations;!directory.empty();directory=directory.parent_path())
    {
        const auto attributes=GetFileAttributesW(directory.c_str());
        if(attributes!=INVALID_FILE_ATTRIBUTES&&(attributes&FILE_ATTRIBUTE_REPARSE_POINT)!=0)return {};
        if(directory==directory.root_path())break;
    }
    GUID id{};if(FAILED(CoCreateGuid(&id)))return {};
    wchar_t text[40]{};StringFromGUID2(id,text,40);
    const auto path=operations/fs::path(text);std::error_code error;fs::create_directories(path,error);return error?fs::path{}:path;
}
void SetPathEnvironment(const wchar_t* name,const fs::path& value)
{SetEnvironmentVariableW(name,value.empty()?nullptr:value.c_str());}
Json SupervisorResources()
{
    PROCESS_MEMORY_COUNTERS_EX memory{};memory.cb=sizeof(memory);DWORD handles=0;
    K32GetProcessMemoryInfo(GetCurrentProcess(),reinterpret_cast<PROCESS_MEMORY_COUNTERS*>(&memory),sizeof(memory));
    GetProcessHandleCount(GetCurrentProcess(),&handles);
    return Json{{"process_id",GetCurrentProcessId()},{"handles",handles},{"working_set",memory.WorkingSetSize},{"private_bytes",memory.PrivateUsage}};
}
Json FailedBuild(const fs::path& candidate,const Json& manifest,const fs::path& operation,const std::string& key,const std::string& configuration)
{
    Json status{{"schemaVersion",1},{"state",candidate.empty()?"failed":"stale"},{"hasCandidate",!candidate.empty()},
        {"sourceIdentity",""},{"candidateIdentity",manifest.is_object()?manifest.value("sourceIdentity",std::string{}):std::string{}},{"candidateRoot",PathText(candidate)},
        {"diagnosticKey",key},{"diagnosticsPath",PathText(operation/"build.log")},{"configuration",configuration},{"managed",true},
        {"sourceDirectories",Json::array({"."})},{"sourceExtensions",Json::array({".cs",".csproj",".props",".targets",".lock.json"})},
        {"sourceFileNames",Json::array({"build.json","global.json","NuGet.Config"})},{"sourceFiles",manifest.is_object()?manifest.value("sourceFiles",Json::array()):Json::array()}};
    if(!candidate.empty())
    {
        status["runtimeExecutable"]=PathText(candidate/TextPath(manifest.at("runtimeExecutable").get<std::string>()));
        status["assembly"]=PathText(candidate/TextPath(manifest.at("assembly").get<std::string>()));
    }
    return status;
}
int RunNativeEditorWorker(const EditorWorkerOptions& options);
int RunManagedEditorWorker(const EditorWorkerOptions& options,const fs::path& engineRoot,const fs::path& userDirectory,EditorLoadingScreen& loading)
{
    std::size_t workflowStages=0,workflowStage=0;std::string workflowError;
#if defined(FLARE_LOCAL_VALIDATION)
    const bool workflow=!options.smokeWorkflow.empty();
    if(workflow&&!EditorSmokeDriver::ValidateWorkflow(options.projectRoot,options.smokeWorkflow,workflowStages,workflowError))return 2;
#else
    const bool workflow=!options.smokeWorkflow.empty();
    if(workflow)return 2;
#endif
    Json workflowReport{{"schemaVersion",1},{"success",false},{"supervisor_process_id",GetCurrentProcessId()},
        {"stages",Json::array()},{"manual_input_verified",false},{"initial_resources",SupervisorResources()}};
    const bool packaged=fs::is_regular_file(engineRoot/"editor-package.json");
#ifdef NDEBUG
    const std::string configuration="Release";
#else
    const std::string configuration=packaged?"Release":"Debug";
#endif
    const auto nativeDirectory=GetExecutableDirectory();
    const auto native=packaged?engineRoot/"Player/EngineNative.dll":nativeDirectory/"EngineNative.dll";
    const auto project=fs::absolute(options.projectRoot).lexically_normal();
    const auto user=project/"out/Editor"/userDirectory.filename();const auto operations=user/"operations";
    Localization strings;std::string localizationError;strings.Load(engineRoot/("Resources/Localization/"+options.locale+".txt"),localizationError);
    fs::path lastCandidate;Json lastManifest=Json::object();
    const auto last=ReadJson(user/("last-success-"+(configuration=="Debug"?std::string("debug"):std::string("release"))+".json"));
    if(last.is_object())
    {
        const auto path=TextPath(last.value("candidateRoot",std::string{}));
        if(!path.empty()&&ValidCandidate(path,operations,project,native,configuration,lastManifest))lastCandidate=path;
    }
    bool firstLoad=true;
    for(;;)
    {
        if(!firstLoad)loading.Restart();firstLoad=false;
        if(!ReportStartup(StartupStage::Scripts))return 0;
        const auto operation=NewOperation(operations);
        if(operation.empty())
        {
            std::wcerr<<L"[Error] Editor.OperationDirectoryUnavailable: "<<operations.wstring()<<L"\n";
            return 5;
        }
        const auto buildStarted=GetTickCount64();std::uint64_t lastProgress=buildStarted,maximumProgressGap=0,progressTicks=0;
        ProcessRunResult build;
        const std::vector<std::wstring> arguments{L"-NoProfile",L"-ExecutionPolicy",L"Bypass",L"-File",(engineRoot/"Scripts/Build-EditorScriptCandidate.ps1").wstring(),
            L"-ProjectRoot",project.wstring(),L"-NativeDirectory",nativeDirectory.wstring(),L"-OperationDirectory",operation.wstring(),L"-Configuration",WideText(configuration),L"-Locale",WideText(options.locale)};
        RunEditorProcess(WindowsPowerShellPath(),arguments,engineRoot,operation/"build.log",&loading.Cancellation(),build,[&]{
            const auto now=GetTickCount64();maximumProgressGap=std::max(maximumProgressGap,now-lastProgress);lastProgress=now;++progressTicks;
            TickBuildProgress(operation);
        });
        const auto buildFinished=GetTickCount64();
        if(build.cancelled||loading.Cancelled())return 0;
        auto status=ReadJson(operation/"build-status.json");
        fs::path candidate;Json candidateManifest;
        if(status.is_object()&&status.value("schemaVersion",0)==1&&status.value("hasCandidate",false))
        {
            const auto root=TextPath(status.value("candidateRoot",std::string{}));
            if(ValidCandidate(root,operations,project,native,configuration,candidateManifest))candidate=root;
        }
        if(build.cancelled||!build.started||!status.is_object()||status.value("schemaVersion",0)!=1||(status.value("hasCandidate",false)&&candidate.empty())
            ||(candidate.empty()&&!lastCandidate.empty()&&status.value("state",std::string{})!="ready"))
        {
            const auto key=build.cancelled?std::string("Editor.ScriptBuildCancelled"):status.is_object()?status.value("diagnosticKey",std::string("Preview.BuildFailed")):std::string("Preview.BuildFailed");
            status=FailedBuild(lastCandidate,lastManifest,operation,key,configuration);
            candidate=lastCandidate;candidateManifest=lastManifest;
            if(!WriteJsonAtomic(operation/"build-status.json",status))return 5;
        }
        if(!candidate.empty()){lastCandidate=candidate;lastManifest=candidateManifest;}
        SetPathEnvironment(L"SELFENGINE_EDITOR_RESTART_REQUEST",operation/"restart.json");
        SetPathEnvironment(L"SELFENGINE_EDITOR_BUILD_STATUS",operation/"build-status.json");
        SetPathEnvironment(L"SELFENGINE_EDITOR_BUILD_DIAGNOSTICS",operation/"build.log");
        SetPathEnvironment(L"SELFENGINE_EDITOR_ENGINE_ROOT",engineRoot);
        if(workflow)
        {
            SetEnvironmentVariableW(L"SELFENGINE_EDITOR_SMOKE_WORKFLOW_STAGE",std::to_wstring(workflowStage).c_str());
            SetEnvironmentVariableW(L"SELFENGINE_EDITOR_SMOKE_HOST_START",std::to_wstring(GetTickCount64()).c_str());
        }
        if(!ReportStartup(StartupStage::Runtime))return 0;
        int exitCode=1;
        if(candidate.empty())
        {
            // 首次编译失败也保留可用编辑器；无有效脚本候选时由状态协议禁止 Play。
            exitCode=RunNativeEditorWorker(options);
        }
        else
        {
            auto settings=ReadJson(project/"build.json"),manifest=ReadJson(project/"project.json");
            if(!settings.is_object()||status.value("state",std::string{})!="ready")settings=candidateManifest.value("buildSettings",Json::object());
            if(!manifest.is_object())manifest=candidateManifest.value("projectSettings",Json::object());
            const auto scene=options.scene.empty()?project/TextPath(manifest.value("entryScene",std::string{})):fs::absolute(options.scene);
            const auto ui=TextPath(settings.value("entryUi",std::string{}));
            SetPathEnvironment(L"SELFENGINE_EDITOR_SCENE_FILE",scene);
            SetPathEnvironment(L"SELFENGINE_EDITOR_UI_FILE",ui.empty()?fs::path{}:project/ui);
            SetEnvironmentVariableW(L"SELFENGINE_EDITOR_SCENE_EXPLICIT",(!options.scene.empty()||options.smokePlayback)?L"1":nullptr);
            SetEnvironmentVariableW(L"SELFENGINE_EDITOR_MANAGED_HOST",L"1");
            SetPathEnvironment(L"SELFENGINE_EDITOR_CAPTURE_FILE",options.capture.empty()?fs::path{}:fs::absolute(options.capture));
            SetEnvironmentVariableW(L"SELFENGINE_EDITOR_CAPTURE_FRAME",std::to_wstring(options.frames?options.frames:30).c_str());
            std::vector<std::wstring> run{L"--editor",L"--project-root",project.wstring(),L"--project",(candidate/TextPath(candidateManifest.at("assembly").get<std::string>())).wstring(),
                L"--scene",scene.wstring(),L"--locale",WideText(options.locale)};
            const auto bindings=TextPath(settings.value("bindings",std::string{}));if(!bindings.empty()){run.push_back(L"--bindings");run.push_back((project/bindings).wstring());}
            if(!ui.empty()){run.push_back(L"--ui");run.push_back((project/ui).wstring());}
            const auto content=TextPath(manifest.value("contentRoot",std::string{}));
            if(!content.empty()){run.push_back(L"--asset-root");run.push_back((project/content).wstring());run.push_back(L"--asset-cache");run.push_back((project/".cache/assets").wstring());}
            if(options.hidden)run.push_back(L"--hidden");
            if(options.frames){run.push_back(L"--frames");run.push_back(std::to_wstring(options.frames));}
            ProcessRunResult child;
            std::atomic<bool> workflowTimeout=false,hostCancelled=false;const auto hostStarted=GetTickCount64();
            const auto workflowMode=workflow?ReadJson(options.smokeWorkflow).at("stages")[workflowStage].value("mode",std::string("playback")):std::string{};
            const auto timeoutMs=workflowMode=="export"?std::uint64_t{905000}:std::uint64_t{120000};
            RunEditorProcess((candidate/TextPath(candidateManifest.at("runtimeExecutable").get<std::string>())).wstring(),run,engineRoot,operation/"editor.log",
                &hostCancelled,child,[&]{
                    if(workflow&&GetTickCount64()-hostStarted>timeoutMs)workflowTimeout=true;
                    if(workflowTimeout||loading.Cancelled())hostCancelled=true;
                },!options.hidden);
            exitCode=child.started?child.exitCode:5;
        }
        const auto restart=ReadJson(operation/"restart.json");
        const bool requested=restart.is_object()&&restart.value("schemaVersion",0)==1&&restart.value("action",std::string{})=="compile";
        if(workflow)
        {
            const auto host=ReadJson(options.smokeWorkflow.parent_path()/("stage-"+std::to_string(workflowStage)+".json"));
            const bool expectedRestart=workflowStage+1<workflowStages;
            const bool passed=exitCode==0&&host.is_object()&&host.value("success",false)&&requested==expectedRestart;
            workflowReport["stages"].push_back(Json{{"index",workflowStage},{"operation",PathText(operation)},{"build_exit_code",build.exitCode},
                {"build_cancelled",build.cancelled},{"build_elapsed_ms",buildFinished-buildStarted},{"progress_poll_count",progressTicks},
                {"maximum_progress_poll_gap_ms",maximumProgressGap},{"host_exit_code",exitCode},{"host_process_id",host.is_object()?host.value("process_id",std::uint64_t{0}):0},
                {"restart_requested",requested},{"passed",passed},{"resources",SupervisorResources()}});
            workflowReport["success"]=passed&&!expectedRestart;
            if(!WriteJsonAtomic(options.smokeWorkflow.parent_path()/"supervisor.json",workflowReport))return 5;
            if(!passed)return exitCode?exitCode:1;
            if(!expectedRestart)return 0;
            ++workflowStage;
        }
        // 只有 RunEditorApp 正常保存/关闭后才发布请求；崩溃或取消关闭没有请求。
        if(!requested)return exitCode;
    }
}
int RunNativeEditorWorker(const EditorWorkerOptions& options)
{
    if(!ReportStartup(StartupStage::Runtime))return 0;
    SetEnvironmentVariableW(L"SELFENGINE_EDITOR_MANAGED_HOST",nullptr);
    EditorRenderer renderer;
    if(!options.capture.empty())renderer.SetCaptureOutput(options.capture,options.frames?options.frames:30);
    EditorApplicationHooks hooks;
    hooks.editorUi=options.editorUi;
    hooks.initialize=[&](EditorSession& editor,UiTree&){
        if(!options.scene.empty())
        {
            if(editor.Open(options.scene,true))return true;
            Log(LogLevel::Warning,"Editor.SceneDefaultCreated");
        }
        if(options.demo){std::string error;return editor.NewDefaultScene(error);}
        if(options.scene.empty()&&!options.projectRoot.empty())
        {
            const auto manifest=ReadJson(options.projectRoot/"project.json");
            const auto entry=manifest.is_object()?manifest.value("entryScene",std::string{}):std::string{};
            if(!entry.empty()&&editor.Open(options.projectRoot/TextPath(entry),true))return true;
            if(!entry.empty())Log(LogLevel::Warning,"Editor.SceneDefaultFallback: "+editor.LastError());
        }
        if(options.smokePlayback)
        {
            // 显式烟测夹具优先于会话恢复；驱动只观察并执行正式命令。
            const auto mainScene=options.projectRoot/L"Scenes"/L"Main.scene.json";
            std::error_code error;
            if(std::filesystem::is_regular_file(mainScene,error))return editor.Open(mainScene,true);
        }
        if(editor.Path().empty()&&editor.EditWorld().Size()==0)
        {
            std::string error;
            if(editor.NewDefaultScene(error))Log(LogLevel::Info,"Editor.SceneDefaultCreated");
            else {Log(LogLevel::Warning,"Editor.SceneDefaultFailed: "+error);return false;}
        }
        return true;
    };
    const int result=RunEditorApp(renderer,options.locale,options.projectRoot,options.hidden,options.frames,&hooks);
    if(result)return result;
    if(options.capture.empty())return 0;
    return renderer.CaptureSucceeded()?0:1;
}
}
int RunEditorWorker(const EditorWorkerOptions& options)
{
    SetConsoleOutputCP(CP_UTF8);
    const auto supervisorStarted=GetTickCount64();
#if defined(FLARE_LOCAL_VALIDATION)
    if(!options.smokeWorkflow.empty())
    {
        std::size_t stages=0;std::string error;
        if(!EditorSmokeDriver::ValidateWorkflow(options.projectRoot,options.smokeWorkflow,stages,error))
        {std::cerr<<"[Error] Editor.WorkflowSmokeInvalid: "<<error<<"\n";return 2;}
        const auto build=ReadJson(options.projectRoot/"build.json");
        if(!build.is_object()||!build.contains("scriptsProject")||!build["scriptsProject"].is_string()||build["scriptsProject"].get<std::string>().empty())return 2;
    }
#else
    if(!options.smokeWorkflow.empty())return 2;
#endif
    // 监督进程持续持有唯一项目租约；编译和实际编辑子进程绝不交叠。
    const auto engineRoot=ProjectWorkspaceSession::EngineRootFromExecutable();
    const auto userDirectory=ProjectWorkspaceSession::UserDataDirectory(engineRoot);
    Localization loadingStrings;std::string loadingError;
    loadingStrings.Load(engineRoot/("Resources/Localization/"+options.locale+".txt"),loadingError);
    EditorLoadingScreen loading(loadingStrings,options.projectRoot,options.hidden);
    if(!options.projectRoot.empty()&&fs::is_regular_file(engineRoot/"editor-package.json")&&asset_io::Within(engineRoot,options.projectRoot))
    {
        Localization strings;std::string error;strings.Load(engineRoot/("Resources/Localization/"+options.locale+".txt"),error);
        auto text=Localized(strings,"Editor.PackageProjectReadOnly");
        if(text==L"Editor.PackageProjectReadOnly")text=L"Projects inside the editor package are read-only. Use Open Sample to create a copy, or copy the project outside the package before opening it.";
        std::wcerr<<L"[Error] Editor.PackageProjectReadOnly: "<<text<<L"\n";
        if(!options.hidden)MessageBoxW(nullptr,text.c_str(),L"Flare Engine",MB_ICONERROR|MB_OK);return 3;
    }
    const auto manifest=options.projectRoot.empty()?fs::path{}:options.projectRoot/L"project.json";
    bool acquired=true;std::string occupant;ProjectLease lease;
    if(!manifest.empty())lease=ProjectLease::Acquire(userDirectory,manifest,acquired,occupant);
    if(!acquired)
    {
        Localization strings;std::string error;strings.Load(engineRoot/("Resources/Localization/"+options.locale+".txt"),error);
        const auto text=Localized(strings,"Editor.LauncherLeaseBusy");std::wcerr<<text<<L"\n";
        if(!options.hidden)MessageBoxW(nullptr,text.c_str(),L"Flare Engine",MB_ICONERROR|MB_OK);return 6;
    }
    SetEnvironmentVariableW(L"SELFENGINE_EDITOR_SCENE_EXPLICIT",(!options.scene.empty()||options.smokePlayback)?L"1":nullptr);
    // 仅正式 Worker 参数设置烟测环境；RunEditorApp 的共用驱动负责执行和报告。
    SetPathEnvironment(L"SELFENGINE_EDITOR_SMOKE_REPORT",options.smokeExportReport.empty()?fs::path{}:fs::absolute(options.smokeExportReport));
    SetEnvironmentVariableW(L"SELFENGINE_EDITOR_SMOKE_EXPORT",options.smokeExportName.empty()?nullptr:WideText(options.smokeExportName).c_str());
    SetEnvironmentVariableW(L"SELFENGINE_EDITOR_SMOKE_PLAYBACK",options.smokePlayback?L"1":nullptr);
    SetEnvironmentVariableW(L"SELFENGINE_EDITOR_SMOKE_CANCEL_MS",std::to_wstring(options.smokeCancelMilliseconds).c_str());
    SetPathEnvironment(L"SELFENGINE_EDITOR_SMOKE_WORKFLOW",options.smokeWorkflow.empty()?fs::path{}:fs::absolute(options.smokeWorkflow));
    SetEnvironmentVariableW(L"SELFENGINE_EDITOR_SMOKE_WORKFLOW_STAGE",nullptr);
    SetEnvironmentVariableW(L"SELFENGINE_EDITOR_SMOKE_HOST_START",nullptr);
    SetEnvironmentVariableW(L"SELFENGINE_EDITOR_SMOKE_SUPERVISOR_PID",options.smokeWorkflow.empty()?nullptr:std::to_wstring(GetCurrentProcessId()).c_str());
    SetEnvironmentVariableW(L"SELFENGINE_EDITOR_SMOKE_SUPERVISOR_START",options.smokeWorkflow.empty()?nullptr:std::to_wstring(supervisorStarted).c_str());
    bool firstNativeLoad=true;
    for(;;)
    {
        if(!firstNativeLoad)loading.Restart();firstNativeLoad=false;
        const auto build=ReadJson(options.projectRoot/"build.json");
        if(fs::is_regular_file(options.projectRoot/"build.json")&&(!build.is_object()
            ||(build.contains("scriptsProject")&&(!build["scriptsProject"].is_string()||!build["scriptsProject"].get<std::string>().empty()))))
            return RunManagedEditorWorker(options,engineRoot,userDirectory,loading);
        // 空项目也有受控重开入口：首次创建 Behaviour 后无需手动关掉启动器。
        if(options.projectRoot.empty())return RunNativeEditorWorker(options);
        const auto operation=NewOperation(fs::absolute(options.projectRoot)/"out/Editor"/userDirectory.filename()/"operations");
        if(operation.empty())
        {
            std::wcerr<<L"[Error] Editor.OperationDirectoryUnavailable: "
                      <<(fs::absolute(options.projectRoot)/"out/Editor"/userDirectory.filename()/"operations").wstring()<<L"\n";
            return 5;
        }
        const Json status{{"schemaVersion",1},{"state","ready"},{"managed",false},{"hasCandidate",false},
            {"sourceIdentity",""},{"candidateIdentity",""},{"candidateRoot",""},{"diagnosticKey",""},{"diagnosticsPath",PathText(operation/"build.log")},{"sourceFiles",Json::array()}};
        if(!WriteJsonAtomic(operation/"build-status.json",status))return 5;
        SetPathEnvironment(L"SELFENGINE_EDITOR_RESTART_REQUEST",operation/"restart.json");
        SetPathEnvironment(L"SELFENGINE_EDITOR_BUILD_STATUS",operation/"build-status.json");
        SetPathEnvironment(L"SELFENGINE_EDITOR_BUILD_DIAGNOSTICS",operation/"build.log");
        SetPathEnvironment(L"SELFENGINE_EDITOR_ENGINE_ROOT",engineRoot);
        const auto result=RunNativeEditorWorker(options);
        const auto restart=ReadJson(operation/"restart.json");
        if(!restart.is_object()||restart.value("schemaVersion",0)!=1||restart.value("action",std::string{})!="compile")return result;
    }
}
}
