#include "EditorProcessRunner.h"
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
bool RunEditorProcess(const std::wstring& executable,const std::vector<std::wstring>& arguments,
    const std::filesystem::path& workingDirectory,const std::filesystem::path& logFile,
    const std::atomic<bool>* cancel,ProcessRunResult& result,
    const std::function<void()>& tick,bool showWindow)
{
    result={};
    std::error_code fileError;
    if(!logFile.parent_path().empty())std::filesystem::create_directories(logFile.parent_path(),fileError);
    HANDLE log=OpenInheritable(logFile.c_str(),FILE_APPEND_DATA,OPEN_ALWAYS);
    if(log==INVALID_HANDLE_VALUE){result.output="Editor.ProcessLogFailed";return false;}
    HANDLE nullInput=OpenInheritable(L"NUL",GENERIC_READ,OPEN_EXISTING);
    if(nullInput==INVALID_HANDLE_VALUE)nullInput=OpenInheritable(L"NUL",GENERIC_READ,OPEN_ALWAYS);
    STARTUPINFOW startup{};
    startup.cb=sizeof(startup);
    startup.dwFlags=STARTF_USESTDHANDLES|STARTF_USESHOWWINDOW;
    // 交互式编辑器宿主必须用 SW_SHOWNORMAL：Windows 会用这里的值覆盖子进程第一次 ShowWindow，
    // 固定 SW_HIDE 会把引擎创建的主窗口强制隐藏，用户只能看到后台进程而没有可见界面。
    startup.wShowWindow=showWindow?SW_SHOWNORMAL:SW_HIDE;
    startup.hStdInput=nullInput==INVALID_HANDLE_VALUE?nullptr:nullInput;
    startup.hStdOutput=log;
    startup.hStdError=log;
    PROCESS_INFORMATION information{};
    auto command=BuildProcessCommandLine(executable,arguments);
    const auto directory=workingDirectory.empty()?std::filesystem::current_path():workingDirectory;
    const auto flags=CREATE_NO_WINDOW|CREATE_UNICODE_ENVIRONMENT|CREATE_SUSPENDED;
    if(!CreateProcessW(executable.c_str(),command.data(),nullptr,nullptr,TRUE,flags,nullptr,directory.c_str(),&startup,&information))
    {
        result.output="Editor.ProcessStartFailed";
        if(log!=INVALID_HANDLE_VALUE)CloseHandle(log);
        if(nullInput!=INVALID_HANDLE_VALUE)CloseHandle(nullInput);
        return false;
    }
    HANDLE job=CreateJobObjectW(nullptr,nullptr);
    bool jobOwnsProcess=false;
    if(job)
    {
        JOBOBJECT_EXTENDED_LIMIT_INFORMATION limits{};
        limits.BasicLimitInformation.LimitFlags=JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE;
        if(SetInformationJobObject(job,JobObjectExtendedLimitInformation,&limits,sizeof(limits)))
            jobOwnsProcess=AssignProcessToJobObject(job,information.hProcess)!=FALSE;
    }
    ResumeThread(information.hThread);
    bool cancelRequested=false;
    for(;;)
    {
        const auto wait=WaitForSingleObject(information.hProcess,100);
        if(wait==WAIT_OBJECT_0)break;
        if(wait==WAIT_FAILED)break;
        if(cancel&&cancel->load())
        {
            cancelRequested=true;
            if(jobOwnsProcess)TerminateJobObject(job,1);
            else TerminateProcess(information.hProcess,1);
            WaitForSingleObject(information.hProcess,5000);
            break;
        }
        if(tick)
        {
            try{tick();}catch(...){}
        }
    }
    DWORD exitCode=1;
    GetExitCodeProcess(information.hProcess,&exitCode);
    CloseHandle(information.hThread);
    CloseHandle(information.hProcess);
    if(job)CloseHandle(job);
    if(nullInput!=INVALID_HANDLE_VALUE)CloseHandle(nullInput);
    if(log!=INVALID_HANDLE_VALUE)CloseHandle(log);
    result.started=true;
    result.cancelled=cancelRequested;
    result.exitCode=static_cast<int>(exitCode);
    result.output=ReadLogTail(logFile,16384);
    return true;
}
bool StartEditorProcess(const std::wstring& executable,const std::vector<std::wstring>& arguments,
    const std::filesystem::path& workingDirectory,std::string& error)
{
    error.clear();
    STARTUPINFOW startup{};
    startup.cb=sizeof(startup);
    startup.dwFlags=STARTF_USESHOWWINDOW;
    startup.wShowWindow=SW_HIDE;
    PROCESS_INFORMATION information{};
    auto command=BuildProcessCommandLine(executable,arguments);
    const auto directory=workingDirectory.empty()?std::filesystem::current_path():workingDirectory;
    if(!CreateProcessW(executable.c_str(),command.data(),nullptr,nullptr,FALSE,CREATE_NO_WINDOW|CREATE_UNICODE_ENVIRONMENT,
        nullptr,directory.c_str(),&startup,&information))
    {
        error="Editor.ProcessStartFailed";
        return false;
    }
    CloseHandle(information.hThread);
    CloseHandle(information.hProcess);
    return true;
}
}
