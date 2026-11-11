#include "ToolLayer/Editor/EditorProcessRunner.h"
#include <atomic>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <iterator>
#include <stdexcept>
#include <thread>

namespace fs=std::filesystem;
using namespace se;
namespace
{
void Check(bool value,const char* message){if(!value)throw std::runtime_error(message);}
std::string Read(const fs::path& path)
{
    std::ifstream input(path,std::ios::binary);return {std::istreambuf_iterator<char>(input),{}};
}
}
int wmain(int argc,wchar_t** argv)
{
    try
    {
        Check(argc==2,"usage: EditorProcessRunnerTests <output-dir>");
        auto output=fs::absolute(fs::path(argv[1]));
        std::error_code cleanup;fs::remove_all(output,cleanup);fs::create_directories(output);
        const auto powershell=WindowsPowerShellPath();
        Check(fs::is_regular_file(powershell),"powershell available");
        ProcessRunResult result;
        const auto log=output/"echo.log";
        Check(RunEditorProcess(powershell,{L"-NoProfile",L"-Command",L"Write-Output process-runner-probe"},output,log,nullptr,result),"echo process starts");
        Check(result.started&&!result.cancelled&&result.exitCode==0,"echo process exits zero");
        Check(Read(log).find("process-runner-probe")!=std::string::npos,"child output redirected to log");
        Check(RunEditorProcess(powershell,{L"-NoProfile",L"-Command",L"exit 3"},output,output/"fail.log",nullptr,result),"failing process starts");
        Check(result.exitCode==3,"failing process exit code reported");
        Check(!RunEditorProcess((output/"missing.exe").wstring(),{},output,output/"missing.log",nullptr,result),"missing executable rejected");
        Check(!result.started,"missing executable did not start");
        std::atomic<bool> cancel{false};
        std::thread canceller([&]{std::this_thread::sleep_for(std::chrono::milliseconds(300));cancel.store(true);});
        const auto start=std::chrono::steady_clock::now();
        Check(RunEditorProcess(powershell,{L"-NoProfile",L"-Command",L"Start-Sleep -Seconds 5"},output,output/"cancel.log",&cancel,result),"cancellable process starts");
        const auto elapsed=std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now()-start).count();
        canceller.join();
        Check(result.cancelled,"cancel request observed");
        Check(elapsed<4500,"cancel terminates child before natural completion");
        const auto line=BuildProcessCommandLine(L"C:/tool.exe",{L"plain",L"with space",L"quote\"inside"});
        Check(line.find(L"\"with space\"")!=std::wstring::npos,"arguments with spaces quoted");
        Check(line.find(L"quote\\\"inside")!=std::wstring::npos,"embedded quotes escaped");
        std::cout<<"EditorProcessRunnerTests passed (log redirection, exit code, missing executable, cancellation, quoting).\n";
        return 0;
    }
    catch(const std::exception& failure)
    {
        std::cerr<<"EditorProcessRunnerTests failed: "<<failure.what()<<'\n';return 1;
    }
}
