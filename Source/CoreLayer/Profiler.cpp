#include "CoreLayer/Profiler.h"
#include <algorithm>
#include <cmath>
#include <iomanip>
#include <limits>
#include <sstream>
#include <stdexcept>
#include <Windows.h>

namespace se
{
namespace
{
std::atomic<std::uint64_t> nextExport{1};
std::string Quote(std::string_view value)
{std::string result="\"";for(char c:value){if(c=='\"')result+='\"';result+=c;}return result+'\"';}
bool WriteCsv(const std::filesystem::path& path,std::string_view text,std::string& error)
{
    error.clear();if(path.empty()||text.size()>MAXDWORD){error="Profiler.InvalidPath";return false;}
    auto temporary=path;temporary+=L".tmp."+std::to_wstring(GetCurrentProcessId())+L"."+std::to_wstring(nextExport.fetch_add(1));
    const HANDLE file=CreateFileW(temporary.c_str(),GENERIC_WRITE,0,nullptr,CREATE_NEW,FILE_ATTRIBUTE_NORMAL,nullptr);
    if(file==INVALID_HANDLE_VALUE){error="Profiler.WriteFailed";return false;}
    DWORD written=0;const bool success=WriteFile(file,text.data(),static_cast<DWORD>(text.size()),&written,nullptr)
        &&written==text.size()&&FlushFileBuffers(file);const bool closed=CloseHandle(file)!=FALSE;
    if(!success||!closed){DeleteFileW(temporary.c_str());error="Profiler.WriteFailed";return false;}
    if(!MoveFileExW(temporary.c_str(),path.c_str(),MOVEFILE_REPLACE_EXISTING|MOVEFILE_WRITE_THROUGH))
    {DeleteFileW(temporary.c_str());error="Profiler.WriteFailed";return false;}
    return true;
}
double Percentile(const std::vector<double>& sorted,double percentile)
{return sorted[static_cast<std::size_t>(std::ceil(percentile*static_cast<double>(sorted.size())))-1];}
}
Profiler::Profiler(std::size_t historyLimit):limit_(historyLimit)
{if(historyLimit==0)throw std::invalid_argument("Profiler.InvalidLimit");}
bool WriteProfileCsv(const std::filesystem::path& path,std::string_view text,std::string& error)
{return WriteCsv(path,text,error);}
void Profiler::SetEnabled(bool enabled)
{
    std::lock_guard lock(mutex_);enabled_=enabled;
    if(!enabled){active_=false;current_={};++token_;}
}
bool Profiler::BeginFrame(std::uint64_t frame)
{
    if(!Enabled())return false;std::lock_guard lock(mutex_);
    if(!Enabled()||active_||(!frames_.empty()&&frame<=frames_.back().frame))return false;
    current_={};current_.frame=frame;active_=true;++token_;started_=std::chrono::steady_clock::now();return true;
}
bool Profiler::EndFrame()
{
    std::lock_guard lock(mutex_);if(!active_)return false;
    const double elapsed=std::chrono::duration<double,std::milli>(std::chrono::steady_clock::now()-started_).count();
    current_.scopes["CPU.Frame"]={1,elapsed};frames_.push_back(std::move(current_));active_=false;++token_;
    if(frames_.size()>limit_){frames_.pop_front();++dropped_;}return true;
}
void Profiler::Clear()
{std::lock_guard lock(mutex_);active_=false;current_={};frames_.clear();dropped_=0;++token_;}
std::uint64_t Profiler::Token() const
{if(!Enabled())return 0;std::lock_guard lock(mutex_);return active_&&Enabled()?token_:0;}
bool Profiler::Record(std::uint64_t token,std::string_view scope,double milliseconds,std::uint64_t calls)
{
    if(!token||scope.empty()||scope.size()>1024||scope=="CPU.Frame"||!std::isfinite(milliseconds)||milliseconds<0||calls==0)return false;
    std::lock_guard lock(mutex_);if(!active_||token!=token_||!Enabled())return false;
    auto& sample=current_.scopes[std::string(scope)];
    if(calls>(std::numeric_limits<std::uint64_t>::max)()-sample.calls||!std::isfinite(sample.milliseconds+milliseconds))return false;
    sample.calls+=calls;sample.milliseconds+=milliseconds;return true;
}
bool Profiler::RecordCpuDuration(std::string_view scope,double milliseconds,std::uint64_t calls)
{return Record(Token(),scope,milliseconds,calls);}
std::vector<CpuFrame> Profiler::Frames() const
{std::lock_guard lock(mutex_);return {frames_.begin(),frames_.end()};}
std::vector<CpuFrame> Profiler::FramesSince(std::uint64_t frame) const
{
    std::lock_guard lock(mutex_);const auto first=std::upper_bound(frames_.begin(),frames_.end(),frame,[](auto cursor,const CpuFrame& value){return cursor<value.frame;});
    return {first,frames_.end()};
}
std::uint64_t Profiler::DroppedFrames() const
{std::lock_guard lock(mutex_);return dropped_;}
std::vector<CpuStatistics> Profiler::Statistics() const
{
    const auto frames=Frames();std::map<std::string,std::vector<double>> values;std::map<std::string,std::uint64_t> calls;
    for(const auto& frame:frames)for(const auto& [scope,sample]:frame.scopes)
    {values[scope].push_back(sample.milliseconds);calls[scope]+=sample.calls;}
    std::vector<CpuStatistics> result;
    for(auto& [scope,samples]:values)
    {
        std::sort(samples.begin(),samples.end());double average=0;
        for(double value:samples)average+=value/static_cast<double>(samples.size());
        result.push_back({scope,samples.size(),calls[scope],average,Percentile(samples,0.5),Percentile(samples,0.95),Percentile(samples,0.99)});
    }
    return result;
}
bool Profiler::ExportFramesCsv(const std::filesystem::path& path,std::string& error) const
{
    std::ostringstream text;text.imbue(std::locale::classic());text<<std::setprecision(17)<<"frame,scope,calls,cpu_ms\n";
    for(const auto& frame:Frames())for(const auto& [scope,sample]:frame.scopes)
        text<<frame.frame<<','<<Quote(scope)<<','<<sample.calls<<','<<sample.milliseconds<<'\n';
    return WriteCsv(path,text.str(),error);
}
bool Profiler::ExportSummaryCsv(const std::filesystem::path& path,std::string& error) const
{
    std::ostringstream text;text.imbue(std::locale::classic());text<<std::setprecision(17)<<"scope,sampled_frames,calls,mean_cpu_ms,p50_cpu_ms,p95_cpu_ms,p99_cpu_ms\n";
    for(const auto& sample:Statistics())text<<Quote(sample.scope)<<','<<sample.frames<<','<<sample.calls<<','<<sample.averageMilliseconds
        <<','<<sample.p50Milliseconds<<','<<sample.p95Milliseconds<<','<<sample.p99Milliseconds<<'\n';
    return WriteCsv(path,text.str(),error);
}
CpuScope::CpuScope(Profiler& profiler,std::string_view name):CpuScope(&profiler,name){}
CpuScope::CpuScope(Profiler* profiler,std::string_view name):profiler_(profiler),token_(profiler?profiler->Token():0)
{if(token_){name_=name;start_=std::chrono::steady_clock::now();}}
CpuScope::~CpuScope() noexcept
{
    if(!token_)return;
    try {profiler_->Record(token_,name_,std::chrono::duration<double,std::milli>(std::chrono::steady_clock::now()-start_).count(),1);}
    catch(...) {} // 诊断工具不得因析构中的内存分配失败终止应用。
}
}
