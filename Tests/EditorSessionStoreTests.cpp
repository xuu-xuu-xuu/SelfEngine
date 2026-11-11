#include "ToolLayer/Editor/EditorSessionStore.h"
#include "CoreLayer/Hash.h"
#include <nlohmann/json.hpp>
#include <Windows.h>
#include <winioctl.h>
#include <algorithm>
#include <array>
#include <cstddef>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <limits>
#include <stdexcept>
#include <string_view>
#include <vector>

namespace fs=std::filesystem;
using namespace se;
namespace
{
void Check(bool value,const char* message){if(!value)throw std::runtime_error(message);}
void Put(const fs::path& path,const std::string& text){fs::create_directories(path.parent_path());std::ofstream output(path,std::ios::binary|std::ios::trunc);output<<text;Check(bool(output),"write fixture");}
std::string Read(const fs::path& path){std::ifstream input(path,std::ios::binary);return {std::istreambuf_iterator<char>(input),{}};}
std::uint64_t Bytes(const fs::path& path)
{
    std::uint64_t total=0;
    for(const auto& item:fs::recursive_directory_iterator(path))if(item.is_regular_file())total+=item.file_size();
    return total;
}
using Json=nlohmann::json;
constexpr DWORD CrashExitCode=0xe0070001;
constexpr DWORD FailedChildExitCode=0xe0070002;
struct TestHandle
{
    HANDLE value=nullptr;
    TestHandle()=default;
    explicit TestHandle(HANDLE handle):value(handle){}
    TestHandle(const TestHandle&)=delete;
    TestHandle& operator=(const TestHandle&)=delete;
    ~TestHandle(){if(value&&value!=INVALID_HANDLE_VALUE)CloseHandle(value);}
};
struct TestChild
{
    TestHandle process,thread;
    DWORD id=0;
    ~TestChild()
    {
        // 失败也只终止本测试刚创建且仍持有句柄的进程，不按进程名或 PID 扫描。
        if(process.value&&WaitForSingleObject(process.value,0)==WAIT_TIMEOUT)
        {TerminateProcess(process.value,FailedChildExitCode);WaitForSingleObject(process.value,5000);}
    }
};
std::string PathText(const fs::path& path)
{
    const auto text=path.generic_u8string();return {reinterpret_cast<const char*>(text.data()),text.size()};
}
fs::path UniqueDirectory(const fs::path& parent,const std::string& label)
{
    fs::create_directories(parent);FILETIME time{};GetSystemTimeAsFileTime(&time);
    const auto ticks=(static_cast<std::uint64_t>(time.dwHighDateTime)<<32)|time.dwLowDateTime;
    for(unsigned attempt=0;attempt<100;++attempt)
    {
        const auto path=parent/(label+"-"+std::to_string(GetCurrentProcessId())+"-"+std::to_string(ticks)+"-"+std::to_string(attempt));
        if(fs::create_directory(path))return path;
    }
    throw std::runtime_error("cannot allocate unique test evidence directory");
}
std::wstring QuoteArgument(std::wstring_view value)
{
    std::wstring output=L"\"";std::size_t slashes=0;
    for(const auto character:value)
    {
        if(character==L'\\'){++slashes;continue;}
        output.append(character==L'\"'?slashes*2+1:slashes,L'\\');slashes=0;output.push_back(character);
    }
    output.append(slashes*2,L'\\');output.push_back(L'\"');return output;
}
void StartTestChild(TestChild& child,const fs::path& directory,const std::vector<std::wstring>& arguments)
{
    std::array<wchar_t,32768> module{};const auto length=GetModuleFileNameW(nullptr,module.data(),static_cast<DWORD>(module.size()));
    Check(length>0&&length<module.size(),"locate test executable");
    const std::wstring executable(module.data(),length);auto command=QuoteArgument(executable);
    for(const auto& argument:arguments){command.push_back(L' ');command+=QuoteArgument(argument);}
    STARTUPINFOW startup{};startup.cb=sizeof(startup);PROCESS_INFORMATION process{};
    Check(CreateProcessW(executable.c_str(),command.data(),nullptr,nullptr,FALSE,CREATE_NO_WINDOW,nullptr,directory.c_str(),&startup,&process)!=FALSE,
        "create recovery test child");
    child.process.value=process.hProcess;child.thread.value=process.hThread;child.id=process.dwProcessId;
}
DWORD StopTestChild(TestChild& child,DWORD code)
{
    Check(WaitForSingleObject(child.process.value,0)==WAIT_TIMEOUT,"crash child must still be alive at requested boundary");
    Check(TerminateProcess(child.process.value,code)!=FALSE,"terminate only owned crash child");
    Check(WaitForSingleObject(child.process.value,5000)==WAIT_OBJECT_0,"wait for forced child termination");
    DWORD result=0;Check(GetExitCodeProcess(child.process.value,&result)!=FALSE,"read forced child exit code");return result;
}
void WaitChildEvent(HANDLE event,const TestChild& child,const char* message)
{
    const HANDLE handles[]{event,child.process.value};Check(WaitForMultipleObjects(2,handles,FALSE,10000)==WAIT_OBJECT_0,message);
}
struct RecoveryWatch
{
    TestHandle directory,event;
    OVERLAPPED pending{};
    alignas(DWORD) std::array<std::byte,16384> buffer{};
    bool armed=false;
    explicit RecoveryWatch(const fs::path& path)
        :directory(CreateFileW(path.c_str(),FILE_LIST_DIRECTORY,FILE_SHARE_READ|FILE_SHARE_WRITE|FILE_SHARE_DELETE,nullptr,OPEN_EXISTING,
            FILE_FLAG_BACKUP_SEMANTICS|FILE_FLAG_OVERLAPPED,nullptr)),event(CreateEventW(nullptr,TRUE,FALSE,nullptr))
    {Check(directory.value!=INVALID_HANDLE_VALUE&&event.value,"open recovery directory observer");Arm();}
    void Arm()
    {
        ResetEvent(event.value);pending={};pending.hEvent=event.value;
        Check(ReadDirectoryChangesW(directory.value,buffer.data(),static_cast<DWORD>(buffer.size()),TRUE,
            FILE_NOTIFY_CHANGE_FILE_NAME|FILE_NOTIFY_CHANGE_DIR_NAME|FILE_NOTIFY_CHANGE_SIZE,nullptr,&pending,nullptr)!=FALSE,
            "arm recovery directory observer");armed=true;
    }
    void Consume(Json& observations)
    {
        DWORD bytes=0;Check(GetOverlappedResult(directory.value,&pending,&bytes,FALSE)!=FALSE,"read recovery directory observations");armed=false;
        for(DWORD offset=0;offset<bytes;)
        {
            const auto* change=reinterpret_cast<const FILE_NOTIFY_INFORMATION*>(buffer.data()+offset);
            observations.push_back({{"action",change->Action},{"path",PathText(fs::path(std::wstring(change->FileName,change->FileNameLength/sizeof(wchar_t))))}});
            if(change->NextEntryOffset==0)break;offset+=change->NextEntryOffset;
        }
        Arm();
    }
    ~RecoveryWatch()
    {
        if(armed){CancelIoEx(directory.value,&pending);DWORD ignored=0;GetOverlappedResult(directory.value,&pending,&ignored,TRUE);}
    }
};
constexpr std::array<const char*,3> CrashPaths{"Scenes/Main.scene.json","Materials/Main.material.json","Ui/Main.ui.json"};
constexpr std::array<const char*,3> CrashKinds{"scene","material","ui"};
std::string CrashSource(std::size_t index){return std::string("{\"saved\":\"")+CrashKinds[index]+"\"}\n";}
std::vector<RecoveryDocument> CrashDocuments(const std::string& stage,std::uint64_t revision)
{
    std::vector<RecoveryDocument> result;
    for(std::size_t index=0;index<CrashKinds.size();++index)
    {
        // 仅中途终止用大文件扩大真实写入窗口；完成场景不必反复读取大数据。
        const auto bytes=stage=="candidate"&&revision==2&&index==0?32u*1024u*1024u:1024u;
        auto content=std::string("{\"document\":\"")+CrashKinds[index]+"\",\"revision\":"+std::to_string(revision)+",\"text\":\"";
        content.append(bytes,static_cast<char>('a'+index));content+="\"}\n";
        result.push_back({CrashKinds[index],CrashKinds[index],CrashPaths[index],Sha256(CrashSource(index)),std::move(content),revision});
    }
    return result;
}
Json DocumentEvidence(const std::vector<RecoveryDocument>& documents)
{
    Json result=Json::array();
    for(const auto& document:documents)
        result.push_back({{"id",document.documentId},{"kind",document.kind},{"path",document.relativePath},{"source_sha256",document.sourceDigest},
            {"revision",document.revision},{"bytes",document.content.size()},{"sha256",Sha256(document.content)}});
    return result;
}
Json FileEvidence(const fs::path& path)
{
    const auto size=fs::file_size(path);Check(size<=200ull*1024*1024,"evidence file exceeds test budget");
    std::ifstream input(path,std::ios::binary);std::string bytes(static_cast<std::size_t>(size),'\0');
    input.read(bytes.data(),static_cast<std::streamsize>(bytes.size()));Check(bool(input),"read complete crash evidence file");
    const auto digest=Sha256(bytes);Check(digest.size()==64,"hash crash evidence");return {{"bytes",size},{"sha256",digest}};
}
Json CrashState(const fs::path& project,const fs::path& directory)
{
    Json result{{"sources",Json::array()},{"storage",Json::array()}};
    for(const auto* relative:CrashPaths)
    {
        auto item=FileEvidence(project/relative);item["path"]=relative;item["content"]=Read(project/relative);result["sources"].push_back(std::move(item));
    }
    std::vector<fs::path> paths;
    if(fs::exists(directory))for(const auto& entry:fs::recursive_directory_iterator(directory))paths.push_back(entry.path());
    std::sort(paths.begin(),paths.end());
    for(const auto& path:paths)
    {
        Json item{{"path",PathText(path.lexically_relative(directory))},{"directory",fs::is_directory(path)}};
        if(fs::is_regular_file(path))item.update(FileEvidence(path));result["storage"].push_back(std::move(item));
    }
    return result;
}
std::vector<fs::path> CandidateDirectories(const fs::path& groups)
{
    std::vector<fs::path> result;
    for(const auto& entry:fs::directory_iterator(groups))
        if(entry.is_directory()&&entry.path().filename().wstring().rfind(L".candidate-",0)==0)result.push_back(entry.path());
    return result;
}
int CrashChild(int argc,wchar_t** argv)
{
    Check(argc==7,"usage: --crash-child <case-dir> <stage> <ready-event> <go-event> <committed-event>");
    const auto output=fs::absolute(argv[2]),project=output/L"恢复 项目",directory=EditorSessionStore::ProjectDirectory(project);
    try
    {
        TestHandle ready(OpenEventW(EVENT_MODIFY_STATE|SYNCHRONIZE,FALSE,argv[4]));
        TestHandle go(OpenEventW(EVENT_MODIFY_STATE|SYNCHRONIZE,FALSE,argv[5]));
        TestHandle committed(OpenEventW(EVENT_MODIFY_STATE|SYNCHRONIZE,FALSE,argv[6]));
        Check(ready.value&&go.value&&committed.value,"open child synchronization events");
        std::string error;const EditorSessionStore::CrashMarker marker{GetCurrentProcessId(),EditorSessionStore::CurrentProcessCreation(),"recovery-crash-test"};
        Check(EditorSessionStore::WriteCrashMarker(directory,marker,error),"write live child crash marker");
        Put(output/"child-ready.json",Json{{"process_id",marker.processId},{"process_creation",marker.processCreation}}.dump(2));
        Check(SetEvent(ready.value)!=FALSE,"signal crash child ready");
        Check(WaitForSingleObject(go.value,30000)==WAIT_OBJECT_0,"wait for parent write permission");
        const auto stage=PathText(fs::path(argv[3]));const auto documents=CrashDocuments(stage,2);
        const bool written=EditorSessionStore::WriteRecoveryGroup(directory,project,documents,error);
        Put(output/"child-write-result.json",Json{{"success",written},{"error",error}}.dump(2));
        Check(written,"real recovery writer failed in child");
        TestHandle parked(CreateEventW(nullptr,TRUE,FALSE,nullptr));Check(parked.value,"create post-publication wait");
        Check(SetEvent(committed.value)!=FALSE,"signal recovery publication");
        WaitForSingleObject(parked.value,30000);
        throw std::runtime_error("parent did not terminate child after recovery publication");
    }
    catch(const std::exception& failure)
    {Put(output/"child-failure.txt",failure.what());return 1;}
}
int VerifyCrashCase(const fs::path& output)
{
    Json report{{"passed",false}};
    try
    {
        const auto fixture=Json::parse(Read(output/"fixture.json"));const auto stage=fixture.at("stage").get<std::string>();
        const auto project=output/L"恢复 项目",directory=EditorSessionStore::ProjectDirectory(project);
        std::string error;EditorSessionStore::CrashMarker marker;
        const auto startup=EditorSessionStore::InspectStartup(directory,marker,error);
        report["startup"]=static_cast<int>(startup);report["marker_pid"]=marker.processId;report["marker_creation"]=marker.processCreation;
        const auto child=Json::parse(Read(output/"child-ready.json"));
        Check(error.empty()&&startup==EditorSessionStore::StartupState::UncleanExit,"fresh process must detect actual terminated child");
        Check(marker.processId==child.at("process_id").get<DWORD>()&&marker.processCreation==child.at("process_creation").get<std::uint64_t>(),
            "crash marker retains exact child identity");
        for(std::size_t index=0;index<CrashPaths.size();++index)Check(Read(project/CrashPaths[index])==CrashSource(index),"forced termination never changes source files");
        const auto groups=EditorSessionStore::ListRecoveryGroups(directory,project,error);report["groups"]=Json::array();
        Check(error.empty()&&groups.size()==(stage=="after-manifest"?2u:1u),"reader offers only expected complete groups");
        bool foundBaseline=false,foundNew=false;
        for(const auto& group:groups)
        {
            const auto documents=DocumentEvidence(group.documents);report["groups"].push_back({{"id",group.id},{"documents",documents}});
            if(group.id==fixture.at("baseline_id").get<std::string>())
            {Check(documents==fixture.at("baseline_documents"),"previous complete group survives forced termination");foundBaseline=true;}
            else
            {Check(stage=="after-manifest"&&documents==fixture.at("new_documents"),"published group contains every new document with exact bytes");foundNew=true;}
        }
        Check(foundBaseline&&foundNew==(stage=="after-manifest"),"old/new recovery groups do not mix revisions");
        // 保留原始崩溃目录，启动清理和再次发布只在副本中验证，失败仍有完整现场。
        const auto restart=output/"restart-storage";fs::copy(directory,restart,fs::copy_options::recursive);
        Check(EditorSessionStore::CleanupAbandonedCandidates(restart,error),"restart safely reclaims actual abandoned candidate");
        Check(CandidateDirectories(restart/"recovery"/"groups").empty(),"restart removes generated abandoned candidate");
        Check(EditorSessionStore::WriteRecoveryGroup(restart,project,CrashDocuments(stage,3),error),"recovery writer remains usable after forced termination");
        const auto afterRestart=EditorSessionStore::ListRecoveryGroups(restart,project,error);
        Check(error.empty()&&afterRestart.size()==groups.size()+1&&DocumentEvidence(afterRestart.front().documents)==DocumentEvidence(CrashDocuments(stage,3)),
            "restart publishes a complete independent next revision");
        report["restart_group_count"]=afterRestart.size();report["passed"]=true;Put(output/"verification.json",report.dump(2));return 0;
    }
    catch(const std::exception& failure)
    {report["error"]=failure.what();Put(output/"verification.json",report.dump(2));return 1;}
}
bool RunCrashCase(const fs::path& output,const std::string& stage)
{
    const auto project=output/L"恢复 项目",directory=EditorSessionStore::ProjectDirectory(project),storage=directory/"recovery"/"groups";
    for(std::size_t index=0;index<CrashPaths.size();++index)Put(project/CrashPaths[index],CrashSource(index));
    const auto baseline=CrashDocuments(stage,1);std::string error;
    if(!EditorSessionStore::WriteRecoveryGroup(directory,project,baseline,error))
    {
        const auto longestCandidate=storage/".candidate-00000000000000000000-4294967295-0000000000"/"manifest.json";
        Put(output/"setup-failure.json",Json{{"stage",stage},{"error",error},{"storage",PathText(storage)},
            {"maximum_candidate_path_utf16",longestCandidate.native().size()},{"state",CrashState(project,directory)}}.dump(2));
        throw std::runtime_error("prepare crash baseline recovery group: "+error);
    }
    const auto groups=EditorSessionStore::ListRecoveryGroups(directory,project,error);Check(error.empty()&&groups.size()==1,"prepare exactly one baseline group");
    const auto baselineId=groups.front().id;
    Put(output/"fixture.json",Json{{"stage",stage},{"baseline_id",baselineId},{"baseline_documents",DocumentEvidence(baseline)},
        {"new_documents",DocumentEvidence(CrashDocuments(stage,2))}}.dump(2));
    Put(output/"before.json",CrashState(project,directory).dump(2));
    const auto prefix=L"Local\\SelfEngineRecovery-"+std::to_wstring(GetCurrentProcessId())+L"-"+output.filename().wstring();
    const auto readyName=prefix+L"-ready",goName=prefix+L"-go",committedName=prefix+L"-committed";
    TestHandle ready(CreateEventW(nullptr,TRUE,FALSE,readyName.c_str()));TestHandle go(CreateEventW(nullptr,TRUE,FALSE,goName.c_str()));
    TestHandle committed(CreateEventW(nullptr,TRUE,FALSE,committedName.c_str()));
    Check(ready.value&&go.value&&committed.value,"create parent synchronization events");
    TestChild child;Json evidence{{"stage",stage},{"passed",false},{"notifications",Json::array()}};
    try
    {
        RecoveryWatch watch(storage);
        StartTestChild(child,output,{L"--crash-child",output.wstring(),fs::path(stage).wstring(),readyName,goName,committedName});
        evidence["child_pid"]=child.id;WaitChildEvent(ready.value,child,"child must write crash marker before parent proceeds");
        bool committedObserved=false;
        if(stage!="before-write")
        {
            Check(SetEvent(go.value)!=FALSE,"allow child to call real recovery writer");
            if(stage=="after-manifest")
            {WaitChildEvent(committed.value,child,"wait for actual recovery publication");committedObserved=true;}
            else
            {
                const auto deadline=GetTickCount64()+10000;bool observed=false;
                while(true)
                {
                    const auto now=GetTickCount64();if(now>=deadline)break;
                    const HANDLE handles[]{watch.event.value,committed.value,child.process.value};
                    const auto wait=WaitForMultipleObjects(3,handles,FALSE,static_cast<DWORD>(deadline-now));
                    if(wait==WAIT_OBJECT_0)
                    {
                        watch.Consume(evidence["notifications"]);
                        for(const auto& candidate:CandidateDirectories(storage))
                            if(fs::exists(candidate/"0.snapshot")){observed=true;break;}
                        if(observed)break;
                    }
                    else if(wait==WAIT_OBJECT_0+1){committedObserved=true;break;}
                    else if(wait==WAIT_OBJECT_0+2)throw std::runtime_error("candidate writer exited before forced termination");
                    else if(wait==WAIT_TIMEOUT)break;
                    else throw std::runtime_error("candidate directory wait failed");
                }
                evidence["candidate_observed"]=observed;
            }
        }
        evidence["committed_event_observed"]=committedObserved;
        evidence["requested_exit_code"]=CrashExitCode;evidence["exit_code"]=StopTestChild(child,CrashExitCode);
        Put(output/"after-kill.json",CrashState(project,directory).dump(2));
        const auto candidates=CandidateDirectories(storage);std::size_t published=0;
        for(const auto& entry:fs::directory_iterator(storage))
            if(entry.is_directory()&&entry.path().filename().wstring().rfind(L".candidate-",0)!=0)++published;
        bool matched=fs::is_directory(storage/baselineId)&&evidence.at("exit_code")==CrashExitCode;
        if(stage=="before-write")matched=matched&&candidates.empty()&&published==1&&!fs::exists(output/"child-write-result.json");
        else if(stage=="candidate")matched=matched&&candidates.size()==1&&published==1&&fs::is_regular_file(candidates[0]/"0.snapshot")
            &&!fs::exists(candidates[0]/"manifest.json")&&!committedObserved&&!fs::exists(output/"child-write-result.json");
        else matched=matched&&candidates.empty()&&published==2&&committedObserved;
        evidence["stage_matched"]=matched;Put(output/"process.json",evidence.dump(2));
        if(!matched)
        {
            if(stage=="candidate")return false;
            throw std::runtime_error("forced termination did not match requested recovery boundary");
        }
        TestChild verifier;StartTestChild(verifier,output,{L"--crash-verify",output.wstring()});
        evidence["verifier_pid"]=verifier.id;
        Check(WaitForSingleObject(verifier.process.value,10000)==WAIT_OBJECT_0,"fresh process verification must finish");
        DWORD verifierExit=1;Check(GetExitCodeProcess(verifier.process.value,&verifierExit)!=FALSE,"read fresh verifier exit code");
        evidence["verifier_exit_code"]=verifierExit;Check(verifierExit==0,"fresh process recovery assertions failed; see verification.json");
        evidence["passed"]=true;Put(output/"process.json",evidence.dump(2));return true;
    }
    catch(const std::exception& failure)
    {
        if(child.process.value&&WaitForSingleObject(child.process.value,0)==WAIT_TIMEOUT)
            evidence["failure_cleanup_exit_code"]=StopTestChild(child,FailedChildExitCode);
        if(child.process.value)
        {DWORD exitCode=0;if(GetExitCodeProcess(child.process.value,&exitCode))evidence["exit_code"]=exitCode;}
        evidence["error"]=failure.what();Put(output/"process.json",evidence.dump(2));
        Put(output/"after-failure.json",CrashState(project,directory).dump(2));throw;
    }
}
void RecoveryCrashProcesses(const fs::path& output)
{
    // 顶层 run 已经唯一；阶段不再重复 PID/时间戳，避免真实 Win32 候选文件路径超过 MAX_PATH。
    fs::create_directories(output);
    const auto createCase=[&](const std::string& name){const auto path=output/name;
        Check(fs::create_directory(path),"create fresh crash evidence phase without overwriting");return path;};
    const auto before=createCase("before-write");Check(RunCrashCase(before,"before-write"),"before-write crash boundary");
    bool candidateHit=false;
    for(unsigned attempt=0;attempt<3&&!candidateHit;++attempt)
    {
        const auto candidate=createCase("candidate-"+std::to_string(attempt));candidateHit=RunCrashCase(candidate,"candidate");
        if(!candidateHit)std::cout<<"Candidate boundary was missed; evidence retained at "<<PathText(candidate)<<'\n';
    }
    Check(candidateHit,"candidate interruption was not observed in three real attempts; no completed write is counted as an interrupted candidate");
    const auto after=createCase("after-manifest");Check(RunCrashCase(after,"after-manifest"),"after-manifest crash boundary");
    std::cout<<"Real recovery crash-process evidence: "<<PathText(output)<<'\n';
}
void Junction(const fs::path& link,const fs::path& target)
{
    fs::create_directories(link);fs::create_directories(target);
    const auto print=fs::canonical(target).wstring(),substitute=std::wstring(L"\\??\\")+print;
    const auto substituteBytes=substitute.size()*sizeof(wchar_t),printBytes=print.size()*sizeof(wchar_t);
    const auto pathBytes=substituteBytes+sizeof(wchar_t)+printBytes+sizeof(wchar_t);
    struct Header{DWORD tag;WORD length,reserved,substituteOffset,substituteLength,printOffset,printLength;};
    static_assert(sizeof(Header)==16);
    const Header header{IO_REPARSE_TAG_MOUNT_POINT,static_cast<WORD>(8+pathBytes),0,0,static_cast<WORD>(substituteBytes),
        static_cast<WORD>(substituteBytes+sizeof(wchar_t)),static_cast<WORD>(printBytes)};
    std::vector<std::uint8_t> buffer(sizeof(header)+pathBytes);
    std::memcpy(buffer.data(),&header,sizeof(header));
    std::memcpy(buffer.data()+sizeof(header),substitute.data(),substituteBytes);
    std::memcpy(buffer.data()+sizeof(header)+substituteBytes+sizeof(wchar_t),print.data(),printBytes);
    HANDLE file=CreateFileW(link.c_str(),GENERIC_WRITE,0,nullptr,OPEN_EXISTING,FILE_FLAG_OPEN_REPARSE_POINT|FILE_FLAG_BACKUP_SEMANTICS,nullptr);
    Check(file!=INVALID_HANDLE_VALUE,"open junction fixture");DWORD returned=0;
    const bool created=DeviceIoControl(file,FSCTL_SET_REPARSE_POINT,buffer.data(),static_cast<DWORD>(buffer.size()),nullptr,0,&returned,nullptr)!=FALSE;
    CloseHandle(file);Check(created,"create junction fixture without symlink privilege");
}
void AbandonedCandidates(const fs::path& output)
{
    const auto project=output/"project",directory=output/"user";
    fs::create_directories(project);std::string error;
    std::vector<RecoveryDocument> documents{{"scene","scene","Scenes/Main.json",{},std::string(2048,'a'),1}};
    Check(EditorSessionStore::WriteRecoveryGroup(directory,project,documents,error),"candidate cleanup base group");
    auto groups=EditorSessionStore::ListRecoveryGroups(directory,project,error);Check(groups.size()==1,"cleanup published fixture");
    const auto id=groups[0].id;const auto storage=directory/"recovery"/"groups",published=storage/id,candidate=storage/(".candidate-"+id);
    Put(storage/"unrelated"/"keep.txt","unknown");
    Put(storage/".candidate-not-ours"/"keep.txt","unknown token");
    Put(storage/".candidate-00000000000000000001-1-9999999999"/"keep.txt","overflow token");
    const auto maximum=Bytes(directory/"recovery")+128;
    fs::copy(published,candidate,fs::copy_options::recursive);
    documents[0].revision=2;
    Check(!EditorSessionStore::WriteRecoveryGroup(directory,project,documents,error,maximum)&&error=="Editor.RecoveryBudgetBlocked",
        "abandoned candidate alone can exhaust global recovery budget");
    EditorSessionStore::CrashMarker active{GetCurrentProcessId(),EditorSessionStore::CurrentProcessCreation(),"active"};
    Check(EditorSessionStore::WriteCrashMarker(directory,active,error),"active marker fixture");
    Check(!EditorSessionStore::CleanupAbandonedCandidates(directory,error)&&error=="Editor.RecoveryCleanupActive"
        &&fs::exists(candidate)&&fs::exists(published),"active writer prevents candidate cleanup");
    auto stale=active;stale.processId=0xfffffff0;stale.processCreation=1;
    Check(EditorSessionStore::WriteCrashMarker(directory,stale,error)&&EditorSessionStore::CleanupAbandonedCandidates(directory,error),"stale marker allows abandoned candidate cleanup");
    Check(!fs::exists(candidate)&&fs::exists(published)&&Read(storage/"unrelated"/"keep.txt")=="unknown"
        &&Read(storage/".candidate-not-ours"/"keep.txt")=="unknown token"
        &&Read(storage/".candidate-00000000000000000001-1-9999999999"/"keep.txt")=="overflow token",
        "cleanup preserves published groups, unknown directories and malformed tokens");
    EditorSessionStore::CrashMarker preserved;
    Check(EditorSessionStore::ReadCrashMarker(directory,preserved,error)&&preserved.processId==stale.processId,"cleanup does not rewrite process marker");
    Check(EditorSessionStore::WriteRecoveryGroup(directory,project,documents,error,maximum)
        &&Bytes(directory/"recovery")<=maximum,"recovery can publish after abandoned space is reclaimed");
    Put(candidate,"unknown regular file");
    Check(EditorSessionStore::CleanupAbandonedCandidates(directory,error)&&Read(candidate)=="unknown regular file","candidate-shaped regular files are preserved");
    fs::remove(candidate);

    const auto outside=output/"outside-recovery";Put(outside/"keep.txt","outside preserved");
    Junction(candidate,outside);
    Check(!EditorSessionStore::CleanupAbandonedCandidates(directory,error)&&error=="Editor.RecoveryInvalidPath"
        &&Read(outside/"keep.txt")=="outside preserved","candidate directory junction is rejected");
    Check(RemoveDirectoryW(candidate.c_str())!=FALSE,"remove only junction fixture");
    Junction(candidate/"nested",outside);
    Check(!EditorSessionStore::CleanupAbandonedCandidates(directory,error)&&error=="Editor.RecoveryInvalidPath"
        &&fs::exists(candidate)&&Read(outside/"keep.txt")=="outside preserved","nested junction prevents recursive candidate cleanup");
    Check(RemoveDirectoryW((candidate/"nested").c_str())!=FALSE,"remove only nested junction fixture");
    Put(directory/"process.json","{ corrupt");
    Check(!EditorSessionStore::CleanupAbandonedCandidates(directory,error)&&error=="Editor.StorageCorrupt"&&fs::exists(candidate),"unknown marker state does not authorize cleanup");
    Check(EditorSessionStore::WriteCrashMarker(directory,stale,error)&&EditorSessionStore::CleanupAbandonedCandidates(directory,error)
        &&!fs::exists(candidate)&&Read(outside/"keep.txt")=="outside preserved","validated stale candidate cleanup leaves link target intact");
}
void RecoveryGroups(const fs::path& output)
{
    const auto project=output/L"恢复 项目";Put(project/L"Scenes/主场景.scene.json","saved scene");Put(project/"Ui/Main.ui.json","saved ui");
    const auto directory=EditorSessionStore::ProjectDirectory(project);
    Check(directory.parent_path()==fs::weakly_canonical(project)/"out"/"Editor","session directory belongs to project and user");
    Check(directory!=EditorSessionStore::ProjectDirectory(output/"other-project"),"projects have separate session roots");
    std::string error;
    const auto relative=std::string(reinterpret_cast<const char*>(u8"Scenes/主场景.scene.json"));
    std::vector<RecoveryDocument> documents{{"scene","scene",relative,Sha256("saved scene"),"edited scene",9},
        {"ui","ui","Ui/Main.ui.json",Sha256("saved ui"),"edited ui",9}};
    Check(EditorSessionStore::WriteRecoveryGroup(directory,project,documents,error),"write complete recovery group");
    auto groups=EditorSessionStore::ListRecoveryGroups(directory,project,error);
    Check(error.empty()&&groups.size()==1&&groups[0].documents.size()==2,"list only complete checked group");
    const auto firstId=groups[0].id;RecoveryGroup read;
    Check(EditorSessionStore::ReadRecoveryGroup(directory,project,firstId,read,error)&&read.documents[0].content=="edited scene"
        &&read.documents[0].relativePath==relative&&read.documents[0].sourceDigest==Sha256("saved scene"),"recovery retains content, unicode paths and source identity");
    Check(Read(project/L"Scenes/主场景.scene.json")=="saved scene"&&Read(project/"Ui/Main.ui.json")=="saved ui","snapshots never replace sources");
    Put(project/L"Scenes/主场景.scene.json","external change");
    Check(EditorSessionStore::ReadRecoveryGroup(directory,project,firstId,read,error)
        &&read.documents[0].sourceDigest!=Sha256(Read(project/L"Scenes/主场景.scene.json")),"changed source remains detectable for save-copy decision");
    Check(!EditorSessionStore::ReadRecoveryGroup(directory,output/"other-project",firstId,read,error)
        &&error=="Editor.RecoveryProjectMismatch","recovery rejects another project");
    Check(!EditorSessionStore::ReadRecoveryGroup(directory,project,"../outside",read,error)
        &&!EditorSessionStore::DiscardRecoveryGroup(directory,"../../outside",error),"group path traversal rejected");
    auto invalid=documents;invalid[0].relativePath="../outside.json";
    Check(!EditorSessionStore::WriteRecoveryGroup(directory,project,invalid,error),"document traversal rejected");
    invalid[0].relativePath="C:/outside.json";
    Check(!EditorSessionStore::WriteRecoveryGroup(directory,project,invalid,error),"absolute document path rejected");
    invalid=documents;invalid[0].sourceDigest="not-a-digest";
    Check(!EditorSessionStore::WriteRecoveryGroup(directory,project,invalid,error),"invalid source identity rejected");
    invalid=documents;invalid[1].documentId=invalid[0].documentId;
    Check(!EditorSessionStore::WriteRecoveryGroup(directory,project,invalid,error),"duplicate document identity rejected");

    const auto storage=directory/"recovery"/"groups";
    fs::copy(storage/firstId,storage/".candidate-interrupted",fs::copy_options::recursive);
    Put(storage/"unfinished"/"0.snapshot","interrupted before manifest");
    groups=EditorSessionStore::ListRecoveryGroups(directory,project,error);
    Check(groups.size()==1&&groups[0].id==firstId&&!error.empty(),"interrupted candidates and missing manifests never offered");
    documents[0].revision=10;
    Check(EditorSessionStore::WriteRecoveryGroup(directory,project,documents,error),"second recovery group");
    groups=EditorSessionStore::ListRecoveryGroups(directory,project,error);Check(groups.size()==2,"two complete groups remain");
    const auto secondId=groups[0].id;const auto snapshotPath=storage/secondId/"0.snapshot";
    const auto originalSnapshot=Read(snapshotPath);auto changedSnapshot=originalSnapshot;changedSnapshot[0]='X';Put(snapshotPath,changedSnapshot);
    Check(!EditorSessionStore::ReadRecoveryGroup(directory,project,secondId,read,error)&&error=="Editor.RecoveryCorrupt","same-size corrupt content rejected by hash");
    groups=EditorSessionStore::ListRecoveryGroups(directory,project,error);
    Check(groups.size()==1&&groups[0].id==firstId,"bad latest content preserves last valid group");
    Put(snapshotPath,originalSnapshot);
    const auto manifestPath=storage/secondId/"manifest.json";const auto originalManifest=Read(manifestPath);
    auto manifest=nlohmann::json::parse(originalManifest);manifest["manifest"]["created_utc"]="tampered";Put(manifestPath,manifest.dump());
    Check(!EditorSessionStore::ReadRecoveryGroup(directory,project,secondId,read,error),"manifest digest detects changed metadata");
    manifest=nlohmann::json::parse(originalManifest);manifest["manifest"]["documents"][0]["relative_path"]="../outside.json";
    manifest["sha256"]=Sha256(manifest["manifest"].dump());Put(manifestPath,manifest.dump());
    Check(!EditorSessionStore::ReadRecoveryGroup(directory,project,secondId,read,error)&&error=="Editor.RecoveryInvalidPath","path validated independently from manifest hash");
    Put(manifestPath,originalManifest);
    Check(EditorSessionStore::DiscardRecoveryGroup(directory,secondId,error)&&!fs::exists(storage/secondId),"discard removes selected group");
    Check(EditorSessionStore::ReadRecoveryGroup(directory,project,firstId,read,error),"discard preserves another valid group");

    const auto capped=output/"capped";
    for(std::uint64_t revision=9;revision<16;++revision)
    {documents[0].revision=revision;Check(EditorSessionStore::WriteRecoveryGroup(capped,project,documents,error),"write capped group");}
    groups=EditorSessionStore::ListRecoveryGroups(capped,project,error);
    Check(groups.size()==5&&groups.front().documents[0].revision==15&&groups.back().documents[0].revision==11,"retain newest five complete groups across two-digit revisions");
    const auto lastId=groups.front().id;auto oversized=documents;oversized[0].content=std::string(4096,'x');
    Check(!EditorSessionStore::WriteRecoveryGroup(capped,project,oversized,error,1024)&&error=="Editor.RecoveryTooLarge","oversized group rejected before eviction");
    groups=EditorSessionStore::ListRecoveryGroups(capped,project,error);
    Check(groups.size()==5&&groups.front().id==lastId,"oversized group keeps all previous valid revisions");

    const auto budgeted=output/"budgeted";
    std::vector<RecoveryDocument> one{{"scene","scene","Scenes/One.json",{},std::string(2048,'a'),1}};
    Check(EditorSessionStore::WriteRecoveryGroup(budgeted,project,one,error),"first budget group");
    const auto maximum=Bytes(budgeted/"recovery")+128;
    one[0].documentId="other";one[0].revision=2;
    Check(EditorSessionStore::WriteRecoveryGroup(budgeted,project,one,error,maximum),"budget shared across different documents");
    groups=EditorSessionStore::ListRecoveryGroups(budgeted,project,error);
    Check(groups.size()==1&&groups[0].documents[0].documentId=="other"&&Bytes(budgeted/"recovery")<=maximum,"whole recovery directory stays within budget");
    const auto retainedId=groups[0].id;
    Put(budgeted/"recovery"/"orphan.snapshot",std::string(1024,'o'));
    one[0].revision=3;
    Check(!EditorSessionStore::WriteRecoveryGroup(budgeted,project,one,error,maximum)&&error=="Editor.RecoveryBudgetBlocked","orphan bytes count toward global budget");
    Check(EditorSessionStore::ReadRecoveryGroup(budgeted,project,retainedId,read,error),"blocked budget never deletes final valid group");
    Check(Read(project/L"Scenes/主场景.scene.json")=="external change","all recovery operations leave changed original intact");
}
void ProtectedRecoveryGroups(const fs::path& output)
{
    const auto project=output/"project",retention=output/"retention",budgeted=output/"budgeted";
    fs::create_directories(project);std::string error;
    constexpr std::uint64_t unlimitedForFixture=200ull*1024*1024;
    std::vector<RecoveryDocument> documents{{"scene","scene","Scenes/Main.scene.json",{},std::string(2048,'a'),1}};
    const auto freeze=[](const fs::path& directory){
        Json files=Json::object();
        for(const auto& item:fs::recursive_directory_iterator(directory/"recovery"))
            if(item.is_regular_file())files[PathText(item.path().lexically_relative(directory))]=Read(item.path());
        return files;
    };
    for(std::uint64_t revision=1;revision<=5;++revision)
    {documents[0].revision=revision;documents[0].content.assign(2048,static_cast<char>('a'+revision));
        Check(EditorSessionStore::WriteRecoveryGroup(retention,project,documents,error),"prepare five recovery revisions");}
    auto groups=EditorSessionStore::ListRecoveryGroups(retention,project,error);Check(error.empty()&&groups.size()==5,"five initial protected-test groups");
    const auto protectedId=groups.back().id,secondOldest=groups[3].id;
    const auto protectedPath=retention/"recovery"/"groups"/protectedId;
    const auto protectedManifest=Read(protectedPath/"manifest.json"),protectedContent=Read(protectedPath/"0.snapshot");
    documents[0].revision=6;documents[0].content.assign(2048,'g');
    Check(EditorSessionStore::WriteRecoveryGroup(retention,project,documents,error,unlimitedForFixture,5,
        {protectedId,protectedId,"not-an-existing-group"}),"new revision skips oldest protected group during retention eviction");
    groups=EditorSessionStore::ListRecoveryGroups(retention,project,error);
    Check(error.empty()&&groups.size()==5&&groups.front().documents[0].revision==6
        &&!fs::exists(retention/"recovery"/"groups"/secondOldest),"retention evicts the oldest eligible group instead");
    Check(Read(protectedPath/"manifest.json")==protectedManifest&&Read(protectedPath/"0.snapshot")==protectedContent,
        "retained deferred revision remains byte-identical");
    std::vector<std::string> allProtected;for(const auto& group:groups)allProtected.push_back(group.id);
    const auto beforeCountFailure=freeze(retention);documents[0].revision=7;
    Check(!EditorSessionStore::WriteRecoveryGroup(retention,project,documents,error,unlimitedForFixture,5,allProtected)
        &&error=="Editor.RecoveryBudgetBlocked","five protected groups refuse a sixth without exceeding revision cap");
    Check(freeze(retention)==beforeCountFailure&&CandidateDirectories(retention/"recovery"/"groups").empty(),
        "revision-cap failure keeps every existing byte and creates no candidate");
    Check(!EditorSessionStore::WriteRecoveryGroup(retention,project,documents,error,unlimitedForFixture,2,{protectedId,groups.front().id})
        &&error=="Editor.RecoveryBudgetBlocked"&&freeze(retention)==beforeCountFailure,
        "unsatisfiable cap does not execute planned eviction of other unprotected groups");

    documents[0].revision=1;documents[0].content.assign(2048,'a');
    Check(EditorSessionStore::WriteRecoveryGroup(budgeted,project,documents,error),"prepare protected budget group");
    groups=EditorSessionStore::ListRecoveryGroups(budgeted,project,error);const auto budgetProtected=groups.front().id;
    documents[0].revision=2;documents[0].content.assign(2048,'b');
    Check(EditorSessionStore::WriteRecoveryGroup(budgeted,project,documents,error),"prepare replaceable budget group");
    groups=EditorSessionStore::ListRecoveryGroups(budgeted,project,error);const auto replaceable=groups.front().id;
    const auto byteLimit=Bytes(budgeted/"recovery")+128;
    documents[0].revision=3;documents[0].content.assign(2048,'c');
    Check(EditorSessionStore::WriteRecoveryGroup(budgeted,project,documents,error,byteLimit,5,{budgetProtected}),
        "budget eviction skips protected bytes and replaces an eligible group");
    groups=EditorSessionStore::ListRecoveryGroups(budgeted,project,error);RecoveryGroup preserved;
    Check(error.empty()&&groups.size()==2&&Bytes(budgeted/"recovery")<=byteLimit
        &&EditorSessionStore::ReadRecoveryGroup(budgeted,project,budgetProtected,preserved,error)&&preserved.documents[0].revision==1
        &&!fs::exists(budgeted/"recovery"/"groups"/replaceable),"byte budget preserves deferred content and stays within limit");
    allProtected.clear();for(const auto& group:groups)allProtected.push_back(group.id);
    const auto beforeByteFailure=freeze(budgeted);documents[0].revision=4;
    Check(!EditorSessionStore::WriteRecoveryGroup(budgeted,project,documents,error,byteLimit,5,allProtected)
        &&error=="Editor.RecoveryBudgetBlocked","protected byte budget blocks writing even below revision cap");
    Check(freeze(budgeted)==beforeByteFailure&&CandidateDirectories(budgeted/"recovery"/"groups").empty(),
        "byte-budget failure keeps original deferred and new-session groups intact");
}
}
int wmain(int argc,wchar_t** argv)
{
    try
    {
        // 子模式在任何 fixture 初始化前分流，绝不清理父进程准备的崩溃现场。
        if(argc>1&&std::wstring_view(argv[1])==L"--crash-child")return CrashChild(argc,argv);
        if(argc>1&&std::wstring_view(argv[1])==L"--crash-verify")
        {Check(argc==3,"usage: --crash-verify <case-dir>");return VerifyCrashCase(fs::absolute(argv[2]));}
        Check(argc==2,"usage: EditorSessionStoreTests <output-dir>");
        const auto output=UniqueDirectory(fs::absolute(fs::path(argv[1])),"run");
        std::cout<<"EditorSessionStoreTests evidence: "<<PathText(output)<<std::endl;
        std::string error;
        EditorPreferences preferences;
        Check(EditorSessionStore::LoadPreferences(output,preferences,error)&&preferences.language=="zh-CN"&&preferences.restoreLastProject,"missing preferences use defaults");
        preferences.language="en-US";preferences.uiScale=1.5f;preferences.externalEditor="code";preferences.recentProjects={"a","b"};
        Check(EditorSessionStore::SavePreferences(output,preferences,error),"preferences saved");
        EditorPreferences loaded;
        Check(EditorSessionStore::LoadPreferences(output,loaded,error)&&loaded.language=="en-US"&&loaded.uiScale==1.5f&&loaded.externalEditor=="code"&&loaded.recentProjects.size()==2,"preferences round trip");
        Put(output/"preferences.json","{ broken");
        const auto before=Read(output/"preferences.json");
        EditorPreferences rejected;
        Check(!EditorSessionStore::LoadPreferences(output,rejected,error)&&!error.empty()&&Read(output/"preferences.json")==before,"corrupt preferences preserved");
        EditorSessionSnapshot session;
        Check(EditorSessionStore::LoadSession(output,session,error)&&session.documents.empty()&&session.layoutVersion==1,"missing session uses defaults");
        session.project="proj";session.documents={"Scenes/Main.scene.json","Materials/A.material.json"};session.activeDocument="Scenes/Main.scene.json";
        session.selection={7,9};session.cameraX=1;session.cameraY=2;session.cameraZ=3;session.cameraYaw=0.5f;session.cameraPitch=-0.25f;session.layoutVersion=2;
        Check(EditorSessionStore::SaveSession(output,session,error),"session saved");
        EditorSessionSnapshot restored;
        Check(EditorSessionStore::LoadSession(output,restored,error)&&restored.project=="proj"&&restored.documents.size()==2&&restored.selection.size()==2
            &&restored.cameraZ==3&&restored.layoutVersion==2,"session round trip");
        session.cameraZ=9;Check(EditorSessionStore::SaveSession(output,session,error),"session second save creates valid backup");
        Put(output/"session.json","{ interrupted");
        Check(EditorSessionStore::LoadSession(output,restored,error)&&error=="Editor.StorageBackupUsed"&&restored.cameraZ==3
            &&Read(output/"session.json")=="{ interrupted","corrupt session loads valid backup without touching original");
        const auto validBackup=Read(output/"session.json.bak");
        Check(EditorSessionStore::SaveSession(output,session,error)&&Read(output/"session.json.bak")==validBackup,"repair never puts bad primary into valid backup");
        bool archived=false;
        for(const auto& item:fs::directory_iterator(output))
            if(item.path().filename().string().rfind("session.json.corrupt-",0)==0&&Read(item.path())=="{ interrupted")archived=true;
        Check(archived,"explicit session save archives damaged original bytes");
        Put(output/"session.json","{\"schema\":2}");
        Check(!EditorSessionStore::LoadSession(output,restored,error)&&error=="Editor.StorageUnsupportedSchema","future session schema does not fall back silently");
        Check(!EditorSessionStore::SaveSession(output,session,error)&&Read(output/"session.json")=="{\"schema\":2}","future session schema never overwritten");
        Check(EditorSessionStore::ClearSession(output,error)&&!fs::exists(output/"session.json")&&!fs::exists(output/"session.json.bak"),"session clear removes backup to prevent resurrection");
        auto invalidSession=session;invalidSession.cameraX=(std::numeric_limits<float>::quiet_NaN)();
        Check(!EditorSessionStore::SaveSession(output,invalidSession,error),"invalid camera state rejected");
        for(std::uint32_t revision=0;revision<7;++revision)
            Check(EditorSessionStore::WriteRecovery(output,"doc",std::string("{}"),revision,200ull*1024*1024,error),"write recovery");
        Check(EditorSessionStore::RecoveryCount(output,"doc")==5,"recovery keeps latest five revisions");
        Check(!fs::exists(output/"recovery"/"doc.0.json")&&fs::exists(output/"recovery"/"doc.6.json"),"recovery evicts oldest");
        const auto listed=EditorSessionStore::ListRecovery(output);
        Check(listed.size()==5&&listed[0].documentId=="doc","recovery listing");
        Check(EditorSessionStore::RecoveryBytes(output,"doc")>0,"recovery bytes reported");
        Check(EditorSessionStore::WriteRecovery(output,"doc","{}",10,200ull*1024*1024,error)
            &&EditorSessionStore::ListRecovery(output).back().revision==10,"legacy revisions sort numerically");
        Check(!EditorSessionStore::WriteRecovery(output,"../outside","{}",1,100,error),"legacy recovery identifiers cannot escape directory");
        RecoveryGroups(output/"groups-tests");
        AbandonedCandidates(output/"abandoned-tests");
        ProtectedRecoveryGroups(output/"protected-tests");
        EditorSessionStore::CrashMarker marker;marker.processId=GetCurrentProcessId();marker.startedUtc="now";
        HANDLE current=GetCurrentProcess();FILETIME creation{},exit{},kernel{},user{};GetProcessTimes(current,&creation,&exit,&kernel,&user);
        marker.processCreation=(static_cast<std::uint64_t>(creation.dwHighDateTime)<<32)|creation.dwLowDateTime;
        Check(EditorSessionStore::WriteCrashMarker(output,marker,error),"crash marker saved");
        EditorSessionStore::CrashMarker read;Check(EditorSessionStore::ReadCrashMarker(output,read,error)&&read.processId==marker.processId&&read.processCreation==marker.processCreation,"crash marker round trip");
        Check(EditorSessionStore::IsProcessActive(read),"current process is active");
        auto dead=read;dead.processId=0xFFFFFFF0;dead.processCreation=1;Check(!EditorSessionStore::IsProcessActive(dead),"unknown process is inactive");
        Check(EditorSessionStore::ClearCrashMarker(output,error)&&!fs::exists(output/"process.json"),"crash marker cleared");
        Check(EditorSessionStore::CurrentProcessCreation()>0,"current process creation time available");
        Check(!EditorSessionStore::UserDataDirectory().empty(),"user data directory resolved");
        EditorSessionStore::CrashMarker inspected;
        Check(EditorSessionStore::InspectStartup(output,inspected,error)==EditorSessionStore::StartupState::Clean,"no marker is a clean start");
        Check(EditorSessionStore::WriteCrashMarker(output,marker,error),"marker written for startup inspection");
        Check(EditorSessionStore::InspectStartup(output,inspected,error)==EditorSessionStore::StartupState::ActiveInstance,"active instance detected");
        auto crashed=marker;crashed.processId=0xFFFFFFF0;crashed.processCreation=1;
        Check(EditorSessionStore::WriteCrashMarker(output,crashed,error),"stale marker written");
        Check(EditorSessionStore::InspectStartup(output,inspected,error)==EditorSessionStore::StartupState::UncleanExit,"unclean exit detected");
        Check(EditorSessionStore::ClearCrashMarker(output,error),"marker cleanup");
        Put(output/"process.json","{ broken");
        Check(EditorSessionStore::InspectStartup(output,inspected,error)==EditorSessionStore::StartupState::UncleanExit&&!error.empty(),"corrupt marker is not a clean shutdown");
        EditorSessionStore::RecoveryDebounce debounce(60);
        Check(debounce.Due(0,1),"first recovery is due");
        debounce.Mark(0,1);Check(!debounce.Due(30,2)&&debounce.Due(60,2),"changed revision waits save interval");
        Check(!debounce.Due(60,1)&&!debounce.Due(600,1),"unchanged revision never resaves");
        debounce.Mark(60,2);Check(!debounce.Due(61,3)&&debounce.Due(120,3),"new edit does not bypass rate limit");
        Check(!debounce.Due(20,3),"clock rollback does not bypass rate limit");
        RecoveryCrashProcesses(output/"crash");
        std::cout<<"EditorSessionStoreTests passed (project storage, backup preservation, atomic recovery groups, hashes, paths, protected eviction budgets and real process termination).\n";
        return 0;
    }
    catch(const std::exception& failure)
    {
        std::cerr<<"EditorSessionStoreTests failed: "<<failure.what()<<'\n';return 1;
    }
}
