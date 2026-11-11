#include <Windows.h>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <stdexcept>
#include <string>
#include <string_view>

namespace fs=std::filesystem;
namespace
{
void Check(bool value,const char* message){if(!value)throw std::runtime_error(message);}
DWORD Run(const fs::path& executable,std::wstring command,const fs::path& log,DWORD flags)
{
    SECURITY_ATTRIBUTES attributes{sizeof(attributes),nullptr,TRUE};
    const auto file=CreateFileW(log.c_str(),GENERIC_WRITE,FILE_SHARE_READ,&attributes,CREATE_NEW,FILE_ATTRIBUTE_NORMAL,nullptr);
    Check(file!=INVALID_HANDLE_VALUE,"create probe log");
    struct FileGuard{HANDLE file;~FileGuard(){CloseHandle(file);}} guardFile{file};
    STARTUPINFOW startup{};startup.cb=sizeof(startup);startup.dwFlags=STARTF_USESTDHANDLES|STARTF_USESHOWWINDOW;
    startup.wShowWindow=SW_HIDE;startup.hStdOutput=file;startup.hStdError=file;
    PROCESS_INFORMATION process{};Check(CreateProcessW(executable.c_str(),command.data(),nullptr,nullptr,TRUE,flags,nullptr,nullptr,&startup,&process)!=FALSE,"start isolated hidden probe");
    struct Guard{PROCESS_INFORMATION& value;~Guard(){CloseHandle(value.hThread);CloseHandle(value.hProcess);}} guard{process};
    if(WaitForSingleObject(process.hProcess,30000)!=WAIT_OBJECT_0)
    {TerminateProcess(process.hProcess,1);WaitForSingleObject(process.hProcess,30000);Check(false,"own console probe timeout");}
    DWORD code=0;Check(GetExitCodeProcess(process.hProcess,&code)!=FALSE,"probe child exit");return code;
}
}
int wmain(int argc,wchar_t** argv)
{
    try
    {
        if(argc==5&&std::wstring_view(argv[1])==L"--child")
        {
            const UINT page=static_cast<UINT>(std::stoul(argv[2]));Check(SetConsoleOutputCP(page)!=FALSE,"set isolated console codepage");
            const fs::path cooker=fs::absolute(argv[3]),log=fs::absolute(argv[4]);
            // 子工具使用管道/文件输出，但仍附着于同一测试控制台；修改其代码页会影响并行cl。
            const auto result=Run(cooker,L"\""+cooker.wstring()+L"\"",log,0);const auto after=GetConsoleOutputCP();
            std::cout<<"before="<<page<<" after="<<after<<" tool_exit="<<result<<'\n';
            Check(result==2,"actual cooker usage exit");Check(after==page,"redirected cooker changed shared console codepage");return 0;
        }
        Check(argc==3,"supply cooker and output directory");const auto cooker=fs::absolute(argv[1]);
        const auto output=fs::absolute(argv[2])/(std::to_wstring(GetCurrentProcessId())+L"-"+std::to_wstring(GetTickCount64()));
        Check(fs::create_directories(output),"fresh probe directory");const auto before=GetConsoleOutputCP();
        wchar_t selfBuffer[32768]{};Check(GetModuleFileNameW(nullptr,selfBuffer,32768)>0,"self executable");const fs::path self=selfBuffer;
        bool success=true;for(const UINT page:{GetACP(),UINT(CP_UTF8)})
        {
            auto command=L"\""+self.wstring()+L"\" --child "+std::to_wstring(page)+L" \""+cooker.wstring()+L"\" \""+(output/(std::to_wstring(page)+L"-tool.log")).wstring()+L"\"";
            const auto log=output/(std::to_wstring(page)+L"-probe.log");const auto result=Run(self,command,log,CREATE_NEW_CONSOLE);
            std::ifstream input(log);std::cout<<input.rdbuf();success&=result==0;
        }
        Check(GetConsoleOutputCP()==before,"outer caller codepage preserved");Check(success,"tool console isolation regression");
        std::ofstream verification(output/"verification.json");verification<<"{\"success\":true,\"caller_codepage\":"<<before<<",\"tested_codepages\":["<<GetACP()<<",65001]}\n";
        Check(bool(verification),"write console isolation evidence");
        std::cout<<"Tool console isolation passed.\n";return 0;
    }
    catch(const std::exception& error){std::cerr<<error.what()<<'\n';return 1;}
}
