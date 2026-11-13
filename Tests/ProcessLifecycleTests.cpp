#include "ToolLayer/Editor/EditorProcessRunnerInternal.h"
#include "ResourceLayer/Assets/AssetIO.h"
#include "CoreLayer/Log.h"
#include <wrl/client.h>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <stdexcept>

namespace fs=std::filesystem;
namespace
{
HANDLE observedJob=nullptr;
struct ExternalProbe
{
    HANDLE stop=nullptr,process=nullptr;
    ~ExternalProbe(){if(stop)SetEvent(stop);if(process){WaitForSingleObject(process,3000);CloseHandle(process);}if(stop)CloseHandle(stop);}
};
void Check(bool value,const char* message){if(!value)throw std::runtime_error(message);}
DWORD Pid(const fs::path& file){DWORD value=0;std::ifstream(file)>>value;return value;}
bool Gone(DWORD pid){HANDLE process=OpenProcess(SYNCHRONIZE,FALSE,pid);if(!process)return true;const bool gone=WaitForSingleObject(process,0)==WAIT_OBJECT_0;CloseHandle(process);return gone;}
HANDLE WINAPI ObserveJob(LPSECURITY_ATTRIBUTES security,LPCWSTR name)
{auto job=CreateJobObjectW(security,name);if(job)DuplicateHandle(GetCurrentProcess(),job,GetCurrentProcess(),&observedJob,0,FALSE,DUPLICATE_SAME_ACCESS);return job;}
}
int wmain(int argc,wchar_t** argv)
{
    try
    {
        Check(argc==3,"supply probe executable and output directory");const auto probe=fs::absolute(argv[1]);const auto output=fs::absolute(argv[2])/("run-"+se::asset_io::NewId())/L"含空格 space";fs::create_directories(output);
        Check(StartFileLog(output/"trace.log"),"open process test trace");std::cout<<"Evidence: "<<se::asset_io::Utf8(output)<<std::endl;
        // 四个初始化故障：创建/配置/归属/恢复，均不得执行孩子代码。
        for(int failure=0;failure<4;++failure)
        {
            auto api=se::detail::ProcessJobApi{};
            if(failure==0)api.createJob=[](LPSECURITY_ATTRIBUTES,LPCWSTR)->HANDLE{SetLastError(ERROR_ACCESS_DENIED);return nullptr;};
            if(failure==1)api.setLimits=[](HANDLE,JOBOBJECTINFOCLASS,LPVOID,DWORD)->BOOL{SetLastError(ERROR_ACCESS_DENIED);return FALSE;};
            if(failure==2)api.assign=[](HANDLE,HANDLE)->BOOL{SetLastError(ERROR_ACCESS_DENIED);return FALSE;};
            if(failure==3)api.resume=[](HANDLE)->DWORD{SetLastError(ERROR_ACCESS_DENIED);return DWORD(-1);};
            const auto root=output/("failure-"+std::to_string(failure));fs::create_directories(root);se::ProcessRunResult result;
            Check(!se::detail::RunEditorProcessWithApi(probe.wstring(),{L"--mark",root.wstring()},root,root/"process.log",nullptr,result,api),"job setup failure rejects child");
            Check(!result.started&&result.exitCode==-1&&!fs::exists(root/"executed.pid"),"rejected suspended child never executes");
            Check(!result.processId||(result.exited&&Gone(result.processId)),"rejected child is confirmed reaped");
        }
        {
            const auto root=output/"tree";fs::create_directories(root);std::atomic<bool> cancel=false;se::ProcessRunResult result;
            Check(se::RunEditorProcess(probe.wstring(),{L"--tree",root.wstring()},root,root/"process.log",&cancel,result,[&]{if(fs::exists(root/"leaf.pid"))cancel=true;}),"cancel process tree");
            Check(result.cancelled&&result.exited&&result.treeExited&&result.forced,"forced cancellation has confirmed tree exit");
            for(const auto* name:{L"root.pid",L"child.pid",L"leaf.pid"})Check(Pid(root/name)&&Gone(Pid(root/name)),"no root, child or grandchild remains");
        }
        {
            const auto root=output/"graceful";fs::create_directories(root);std::atomic<bool> cancel=false;se::ProcessRunResult result;
            Check(se::RunEditorProcess(probe.wstring(),{L"--window",root.wstring()},root,root/"process.log",&cancel,result,[&]{if(fs::exists(root/"ready.pid"))cancel=true;}),"graceful window cancellation");
            Check(result.cancelled&&result.exited&&result.treeExited&&!result.forced&&fs::exists(root/"clean.txt"),"WM_CLOSE runs clean shutdown without termination");
        }
        {
            const auto root=output/"parent-ended";fs::create_directories(root);se::ProcessRunResult result;
            Check(se::RunEditorProcess(probe.wstring(),{L"--orphan-root",root.wstring()},root,root/"process.log",nullptr,result),"root finishes with a live descendant");
            Check(result.exitCode==0&&result.treeExited&&Gone(Pid(root/"leaf.pid")),"normal root exit also confirms descendant cleanup");
        }
        {
            const auto root=output/"external";fs::create_directories(root);const auto eventName=L"Local\\SelfEngineCpuExternal-"+fs::path(se::asset_io::NewId()).wstring();
            ExternalProbe external;external.stop=CreateEventW(nullptr,TRUE,FALSE,eventName.c_str());Check(external.stop!=nullptr,"create owned external shutdown event");
            auto api=se::detail::ProcessJobApi{};api.createJob=ObserveJob;se::ProcessRunResult result;
            const bool ran=se::detail::RunEditorProcessWithApi(probe.wstring(),{L"--external-host",root.wstring(),eventName},root,root/"process.log",nullptr,result,api);
            Check(ran&&result.exitCode==0&&result.treeExited,"broker host exits cleanly");
            const auto deadline=GetTickCount64()+3000;while(!fs::exists(root/"external.pid")&&GetTickCount64()<deadline)Sleep(5);
            const auto pid=Pid(root/"external.pid");external.process=OpenProcess(SYNCHRONIZE|PROCESS_QUERY_LIMITED_INFORMATION,FALSE,pid);Check(external.process!=nullptr,"external application remains alive after host exits");
            BOOL belongs=TRUE;Check(IsProcessInJob(external.process,observedJob,&belongs)&&!belongs,"explicit application is outside internal task job");
            CloseHandle(observedJob);observedJob=nullptr;Check(WaitForSingleObject(external.process,0)==WAIT_TIMEOUT,"closing internal job does not kill external application");
            SetEvent(external.stop);Check(WaitForSingleObject(external.process,3000)==WAIT_OBJECT_0,"owned external probe exits cooperatively");
        }
        for(const auto& entry:fs::recursive_directory_iterator(output))
            Check(!entry.path().filename().wstring().starts_with(L"external-launch-"),"broker request directories are cleaned");
        std::cout<<"Process lifecycle passed: four setup failures, three generation cleanup, graceful close, orphan cleanup, external broker, Unicode/quotes and IPC cleanup; no GPU.\n";return 0;
    }
    catch(const std::exception& failure){if(observedJob)CloseHandle(observedJob);std::cerr<<failure.what()<<'\n';return 1;}
}
