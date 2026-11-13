#include "EditorProcessRunner.h"
#include "EditorProcessRunnerInternal.h"
#include "ExternalProcessBroker.h"
#include "CoreLayer/Log.h"
#include <Windows.h>
#include <algorithm>
#include <fstream>
#include <iterator>

namespace se
{
namespace
{
std::wstring QuoteArgument(const std::wstring& value)
{
    if(value.empty())return L"\"\"";
    if(value.find_first_of(L" \t\n\v\"")==std::wstring::npos)return value;
    std::wstring quoted=L"\"";
    for(auto iterator=value.begin();;++iterator)
    {
        std::size_t backslashes=0;
        while(iterator!=value.end()&&*iterator==L'\\'){++backslashes;++iterator;}
        if(iterator==value.end()){quoted.append(backslashes*2,L'\\');break;}
        if(*iterator==L'"'){quoted.append(backslashes*2+1,L'\\');quoted.push_back(L'"');}
        else{quoted.append(backslashes,L'\\');quoted.push_back(*iterator);}
    }
    quoted.push_back(L'"');
    return quoted;
}
std::string ReadLogTail(const std::filesystem::path& path,std::size_t maximumBytes)
{
    std::ifstream input(path,std::ios::binary);
    if(!input)return {};
    input.seekg(0,std::ios::end);
    const auto size=static_cast<std::size_t>(input.tellg());
    const auto offset=size>maximumBytes?size-maximumBytes:std::size_t{0};
    input.seekg(static_cast<std::streamoff>(offset),std::ios::beg);
    std::string tail{std::istreambuf_iterator<char>(input),{}};
    while(!tail.empty()&&(tail.back()=='\0'||tail.back()=='\r'))tail.pop_back();
    return tail;
}
HANDLE OpenInheritable(const wchar_t* path,DWORD access,DWORD creation)
{
    SECURITY_ATTRIBUTES security{sizeof(SECURITY_ATTRIBUTES),nullptr,TRUE};
    return CreateFileW(path,access,FILE_SHARE_READ|FILE_SHARE_WRITE,&security,creation,FILE_ATTRIBUTE_NORMAL,nullptr);
}
struct Handle
{
    HANDLE value=nullptr;
    explicit Handle(HANDLE handle=nullptr):value(handle){}
    ~Handle(){if(value&&value!=INVALID_HANDLE_VALUE)CloseHandle(value);}
    Handle(const Handle&)=delete;Handle& operator=(const Handle&)=delete;
};
bool JobEmpty(HANDLE job)
{JOBOBJECT_BASIC_ACCOUNTING_INFORMATION info{};return QueryInformationJobObject(job,JobObjectBasicAccountingInformation,&info,sizeof(info),nullptr)&&!info.ActiveProcesses;}
bool WaitTree(HANDLE job,DWORD milliseconds)
{const auto deadline=GetTickCount64()+milliseconds;do{if(JobEmpty(job))return true;Sleep(10);}while(GetTickCount64()<deadline);return JobEmpty(job);}
struct CloseWindows{HANDLE job;bool posted=false;};
BOOL CALLBACK CloseJobWindow(HWND window,LPARAM context)
{
    auto& data=*reinterpret_cast<CloseWindows*>(context);DWORD pid=0;GetWindowThreadProcessId(window,&pid);
    Handle process(OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION,FALSE,pid));BOOL belongs=FALSE;
    if(process.value&&IsProcessInJob(process.value,data.job,&belongs)&&belongs&&PostMessageW(window,WM_CLOSE,0,0))data.posted=true;
    return TRUE;
}
}
std::wstring WindowsPowerShellPath()
{
    wchar_t systemDirectory[MAX_PATH]{};
    const auto length=GetSystemDirectoryW(systemDirectory,MAX_PATH);
    if(length>0&&length<MAX_PATH)
    {
        const auto candidate=std::filesystem::path(systemDirectory)/L"WindowsPowerShell"/L"v1.0"/L"powershell.exe";
        std::error_code error;
        if(std::filesystem::is_regular_file(candidate,error))return candidate.wstring();
    }
    return L"powershell.exe";
}
std::wstring BuildProcessCommandLine(const std::wstring& executable,const std::vector<std::wstring>& arguments)
{
    // 首参数始终加引号，避免 cmd.exe 把带正斜杠的可执行路径当作自身开关解析。
    std::wstring line=L"\""+executable+L"\"";
    for(const auto& argument:arguments){line.push_back(L' ');line+=QuoteArgument(argument);}
    return line;
}
bool detail::RunEditorProcessWithApi(const std::wstring& executable,const std::vector<std::wstring>& arguments,
    const std::filesystem::path& workingDirectory,const std::filesystem::path& logFile,
    const std::atomic<bool>* cancel,ProcessRunResult& result,const ProcessJobApi& api,
    const std::function<void()>& tick,bool showWindow)
{
    result={};
    std::error_code fileError;
    if(!logFile.parent_path().empty())std::filesystem::create_directories(logFile.parent_path(),fileError);
    Handle log(OpenInheritable(logFile.c_str(),FILE_APPEND_DATA,OPEN_ALWAYS));
    if(log.value==INVALID_HANDLE_VALUE){result.systemError=GetLastError();result.output="Editor.ProcessLogFailed";return false;}
    Handle nullInput(OpenInheritable(L"NUL",GENERIC_READ,OPEN_EXISTING));
    const auto fail=[&](const char* key,DWORD error){result.exitCode=-1;result.systemError=error;result.output=std::string(key)+" win32="+std::to_string(error);LogCheckpoint("Process.Failed",result.output+" pid="+std::to_string(result.processId));return false;};
    Handle job(api.createJob(nullptr,nullptr));
    if(!job.value)return fail("Editor.ProcessJobFailed",GetLastError());
    JOBOBJECT_EXTENDED_LIMIT_INFORMATION limits{};limits.BasicLimitInformation.LimitFlags=JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE;
    if(!api.setLimits(job.value,JobObjectExtendedLimitInformation,&limits,sizeof(limits)))return fail("Editor.ProcessJobFailed",GetLastError());
    ExternalProcessBroker broker(logFile.parent_path().empty()?std::filesystem::current_path():logFile.parent_path());
    if(!broker.Ready())return fail("Editor.ExternalLaunchBrokerUnavailable",ERROR_PATH_NOT_FOUND);
    auto environment=broker.Environment();if(environment.empty())return fail("Editor.ExternalLaunchBrokerUnavailable",GetLastError());
    STARTUPINFOW startup{};
    startup.cb=sizeof(startup);
    startup.dwFlags=STARTF_USESTDHANDLES|STARTF_USESHOWWINDOW;
    // 交互式编辑器宿主必须用 SW_SHOWNORMAL：Windows 会用这里的值覆盖子进程第一次 ShowWindow，
    // 固定 SW_HIDE 会把引擎创建的主窗口强制隐藏，用户只能看到后台进程而没有可见界面。
    startup.wShowWindow=showWindow?SW_SHOWNORMAL:SW_HIDE;
    startup.hStdInput=nullInput.value==INVALID_HANDLE_VALUE?nullptr:nullInput.value;
    startup.hStdOutput=log.value;
    startup.hStdError=log.value;
    PROCESS_INFORMATION information{};
    auto command=BuildProcessCommandLine(executable,arguments);
    const auto directory=workingDirectory.empty()?std::filesystem::current_path():workingDirectory;
    const auto flags=CREATE_NO_WINDOW|CREATE_UNICODE_ENVIRONMENT|CREATE_SUSPENDED;
    if(!CreateProcessW(executable.c_str(),command.data(),nullptr,nullptr,TRUE,flags,environment.data(),directory.c_str(),&startup,&information))
    {
        return fail("Editor.ProcessStartFailed",GetLastError());
    }
    Handle process(information.hProcess),thread(information.hThread);result.processId=information.dwProcessId;
    const auto rejectSuspended=[&](const char* key,DWORD error){
        const auto terminated=TerminateProcess(process.value,1);result.exited=WaitForSingleObject(process.value,5000)==WAIT_OBJECT_0;
        result.treeExited=result.exited;LogCheckpoint("Process.RejectedSuspended","pid="+std::to_string(result.processId)+" terminated="+std::to_string(terminated)+" exited="+std::to_string(result.exited));
        return fail(key,error);};
    if(!api.assign(job.value,process.value))return rejectSuspended("Editor.ProcessJobFailed",GetLastError());
    if(api.resume(thread.value)==DWORD(-1))return rejectSuspended("Editor.ProcessResumeFailed",GetLastError());
    result.started=true;
    LogCheckpoint("Process.Start","pid="+std::to_string(result.processId)+" job=owned");
    bool waitFailed=false;
    for(;;)
    {
        const auto wait=WaitForSingleObject(process.value,100);
        if(wait==WAIT_OBJECT_0){result.exited=true;break;}
        if(wait==WAIT_FAILED){result.systemError=GetLastError();waitFailed=true;break;}
        if(cancel&&cancel->load())
        {
            result.cancelled=true;LogCheckpoint("Process.Cancel.Begin","pid="+std::to_string(result.processId));
            CloseWindows closing{job.value};EnumWindows(CloseJobWindow,reinterpret_cast<LPARAM>(&closing));
            // 有窗口的宿主先走 WM_CLOSE，让渲染器正常排空；无窗口内部任务直接收拢 Job。
            if(closing.posted)result.exited=WaitForSingleObject(process.value,1500)==WAIT_OBJECT_0;
            break;
        }
        broker.Pump();
        if(tick)
        {
            try{tick();}catch(...){}
        }
    }
    // 根进程结束不等于内部进程树结束；强制回收仅针对归属已确认的内部任务。
    if(result.exited)WaitTree(job.value,100); // 让内核进程计数完成更新，避免把刚正常结束误报为强杀。
    if(!JobEmpty(job.value))
    {
        result.forced=true;
        LogCheckpoint("Process.Tree.Terminate","pid="+std::to_string(result.processId));
        if(!TerminateJobObject(job.value,1))result.systemError=GetLastError();
    }
    const auto cleanupDeadline=GetTickCount64()+5000;
    result.exited=WaitForSingleObject(process.value,result.exited?0:5000)==WAIT_OBJECT_0;
    const auto now=GetTickCount64();
    result.treeExited=WaitTree(job.value,now<cleanupDeadline?static_cast<DWORD>(cleanupDeadline-now):0);
    if(!result.exited||!result.treeExited)return fail("Editor.ProcessCleanupFailed",result.systemError?result.systemError:ERROR_TIMEOUT);
    DWORD exitCode=0;
    if(!GetExitCodeProcess(process.value,&exitCode))return fail("Editor.ProcessWaitFailed",GetLastError());
    result.exitCode=static_cast<int>(exitCode);
    LogCheckpoint("Process.Exit","pid="+std::to_string(result.processId)+" code="+std::to_string(exitCode)+" tree_exited=1 forced="+std::to_string(result.forced));
    if(waitFailed)return fail("Editor.ProcessWaitFailed",result.systemError);
    result.output=ReadLogTail(logFile,16384);
    return true;
}
bool RunEditorProcess(const std::wstring& executable,const std::vector<std::wstring>& arguments,
    const std::filesystem::path& workingDirectory,const std::filesystem::path& logFile,
    const std::atomic<bool>* cancel,ProcessRunResult& result,const std::function<void()>& tick,bool showWindow)
{return detail::RunEditorProcessWithApi(executable,arguments,workingDirectory,logFile,cancel,result,detail::ProcessJobApi{},tick,showWindow);}
bool StartEditorProcess(const std::wstring& executable,const std::vector<std::wstring>& arguments,
    const std::filesystem::path& workingDirectory,std::string& error,bool showWindow)
{
    return LaunchExternalApplication(executable,arguments,workingDirectory,showWindow,false,error);
}
bool StartEditorAssociatedFile(const std::filesystem::path& file,const std::filesystem::path& workingDirectory,std::string& error)
{return LaunchExternalApplication(file.wstring(),{},workingDirectory,true,true,error);}
}
