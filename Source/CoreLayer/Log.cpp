#include "CoreLayer/Log.h"
#include <atomic>
#include <chrono>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <mutex>
#include <cstdlib>

namespace
{
std::atomic<bool> debugLoggingEnabled = false;
struct Sink
{
    std::mutex mutex;
    std::ofstream file;
    std::filesystem::path path;
    std::chrono::steady_clock::time_point started=std::chrono::steady_clock::now();
};
Sink& FileSink(){static Sink sink;return sink;}
void WriteFile(Sink& sink,const char* level,const std::string& message)
{
    if(!sink.file.is_open())return;
    const auto now=std::chrono::system_clock::now();
    const auto stamp=std::chrono::system_clock::to_time_t(now);std::tm utc{};
    gmtime_s(&utc,&stamp);
    const auto ms=std::chrono::duration_cast<std::chrono::milliseconds>(now.time_since_epoch()).count()%1000;
    sink.file<<std::put_time(&utc,"%Y-%m-%dT%H:%M:%S")<<'.'<<std::setw(3)<<std::setfill('0')<<ms
        <<"Z elapsed_ms="<<std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now()-sink.started).count()
        <<" ["<<level<<"] "<<message<<'\n';
    sink.file.flush();
}
}
void SetDebugLoggingEnabled(bool enabled){debugLoggingEnabled=enabled;}
bool IsDebugLoggingEnabled(){return debugLoggingEnabled.load();}
bool StartFileLog(const std::filesystem::path& path) noexcept
{
    try
    {
        auto& sink=FileSink();std::lock_guard lock(sink.mutex);
        if(sink.file.is_open())return true;
        if(!path.parent_path().empty())std::filesystem::create_directories(path.parent_path());
        sink.file.open(path,std::ios::binary|std::ios::app);
        if(!sink.file)return false;
        sink.path=path;WriteFile(sink,"Checkpoint","stage=Process.Log.Open");return true;
    }
    catch(...){return false;}
}
std::filesystem::path CurrentLogPath()
{auto& sink=FileSink();std::lock_guard lock(sink.mutex);return sink.path;}
bool SampleDiagnosticFrame(std::uint64_t frame) noexcept
{
    // 验证进程可增加开头的有限帧采样，上限 120；正常进程只记录前三帧和心跳。
    static const auto initial=[]() noexcept{const auto* value=std::getenv("SELFENGINE_DIAGNOSTIC_FRAMES");
        if(!value)return 3ul;char* end=nullptr;const auto parsed=std::strtoul(value,&end,10);
        return end!=value&&*end=='\0'&&parsed>=3&&parsed<=120?parsed:3ul;}();
    return frame<=initial||frame%600==0;
}
void LogCheckpoint(const char* stage,const std::string& detail,std::uint64_t frame) noexcept
{
    try
    {
        auto& sink=FileSink();std::lock_guard lock(sink.mutex);
        WriteFile(sink,"Checkpoint",std::string("stage=")+stage+" frame="+std::to_string(frame)+(detail.empty()?"":" ")+detail);
    }
    catch(...){}
}
void Log(LogLevel level,const std::string& message)
{
    if(level==LogLevel::Debug&&!IsDebugLoggingEnabled())return;
    const char* label=level==LogLevel::Debug?"Debug":level==LogLevel::Info?"Info":level==LogLevel::Warning?"Warning":"Error";
    auto& sink=FileSink();std::lock_guard lock(sink.mutex);
    auto& output=(level==LogLevel::Warning||level==LogLevel::Error)?std::cerr:std::cout;
    output<<'['<<label<<"] "<<message<<'\n';
    try{WriteFile(sink,label,message);}catch(...){}
}
