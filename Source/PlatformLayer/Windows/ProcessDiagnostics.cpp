#include "ProcessDiagnostics.h"
#include "ExecutablePath.h"
#include "CoreLayer/Log.h"
#include <Windows.h>
#include <mutex>

namespace
{
struct ProcessExit
{
    ~ProcessExit(){LogCheckpoint("Process.Exit");}
};
}
void InitializeProcessDiagnostics(const char* role) noexcept
{
    try
    {
        static std::once_flag once;
        std::call_once(once,[role]{
            std::filesystem::path directory;
            const auto length=GetEnvironmentVariableW(L"SELFENGINE_LOG_DIRECTORY",nullptr,0);
            if(length&&length<32768)
            {
                std::wstring value(length,L'\0');const auto read=GetEnvironmentVariableW(L"SELFENGINE_LOG_DIRECTORY",value.data(),length);
                if(read&&read<length){value.resize(read);directory=value;}
            }
            if(directory.empty())directory=GetExecutableDirectory()/"Logs";
            SYSTEMTIME time{};GetSystemTime(&time);wchar_t stamp[80]{};
            swprintf_s(stamp,L"%04u%02u%02u-%02u%02u%02u%03u-%lu",time.wYear,time.wMonth,time.wDay,time.wHour,time.wMinute,time.wSecond,time.wMilliseconds,GetCurrentProcessId());
            const auto file=directory/(std::string(role)+"-"+std::filesystem::path(stamp).string()+".log");
            if(!StartFileLog(file))
            {Log(LogLevel::Warning,"Diagnostics.FileLogUnavailable");return;} // 语言表前的启动回退。
            static ProcessExit exit;
            LogCheckpoint("Process.Start",std::string("role=")+role+" pid="+std::to_string(GetCurrentProcessId()));
        });
    }
    catch(...){} // 诊断路径不可写时保留控制台回退。
}
