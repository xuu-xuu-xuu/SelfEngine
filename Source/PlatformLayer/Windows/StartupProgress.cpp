#include "StartupProgress.h"
#include "CoreLayer/Log.h"
#include <algorithm>
#include <atomic>

namespace se
{
namespace
{
constexpr wchar_t Environment[]=L"SELFENGINE_EDITOR_STARTUP_CHANNEL";
struct SharedProgress {volatile LONG percent,stage,cancelled;};
constexpr int percentages[]={5,10,15,25,32,38,45,55,60,65,72,80,90,97,100};
constexpr const char* keys[]={"Editor.Loading.Project","Editor.Loading.Scripts","Editor.Loading.RestoreHost",
    "Editor.Loading.PublishHost","Editor.Loading.VerifyRuntime","Editor.Loading.RestoreScripts",
    "Editor.Loading.BuildScripts","Editor.Loading.ValidateScripts","Editor.Loading.Runtime","Editor.Loading.Assets",
    "Editor.Loading.Scene","Editor.Loading.Interface","Editor.Loading.Renderer","Editor.Loading.FirstFrame","Editor.Loading.Ready"};
std::wstring ChannelName()
{
    const DWORD size=GetEnvironmentVariableW(Environment,nullptr,0);if(!size||size>256)return {};
    std::wstring value(size,L'\0');const DWORD written=GetEnvironmentVariableW(Environment,value.data(),size);
    if(!written||written>=size)return {};value.resize(written);return value;
}
struct Borrowed
{
    HANDLE mapping=nullptr;SharedProgress* data=nullptr;
    Borrowed()
    {
        const auto name=ChannelName();if(name.empty())return;
        mapping=OpenFileMappingW(FILE_MAP_ALL_ACCESS,FALSE,name.c_str());
        if(mapping)data=static_cast<SharedProgress*>(MapViewOfFile(mapping,FILE_MAP_ALL_ACCESS,0,0,sizeof(SharedProgress)));
    }
    ~Borrowed(){if(data)UnmapViewOfFile(data);if(mapping)CloseHandle(mapping);}
};
}
const char* StartupStageKey(StartupStage stage) noexcept
{
    const auto index=static_cast<LONG>(stage);
    return index>=0&&index<=static_cast<LONG>(StartupStage::Ready)?keys[index]:keys[0];
}
bool ReportStartup(StartupStage stage) noexcept
{
    try
    {
        const auto index=static_cast<LONG>(stage);if(index<0||index>static_cast<LONG>(StartupStage::Ready))return false;
        Borrowed shared;if(!shared.data)return true;
        if(InterlockedCompareExchange(&shared.data->cancelled,0,0))return false;
        // 编译回执可能重复到达；不让较旧阶段覆盖实际宿主已报告的进度。
        LONG previous=InterlockedCompareExchange(&shared.data->stage,0,0);
        bool advanced=false;
        while(index>previous)
        {
            const LONG observed=InterlockedCompareExchange(&shared.data->stage,index,previous);
            if(observed==previous){advanced=true;break;}previous=observed;
        }
        LONG percent=InterlockedCompareExchange(&shared.data->percent,0,0);
        while(percentages[index]>percent)
        {
            const LONG observed=InterlockedCompareExchange(&shared.data->percent,percentages[index],percent);
            if(observed==percent)break;percent=observed;
        }
        if(advanced)LogCheckpoint("Startup.Progress",std::string(keys[index])+" percent="+std::to_string(percentages[index]));
        return true;
    }
    catch(...){return true;} // 诊断 UI 不让原本可用的启动因映射分配失败退出。
}
bool StartupCancelled() noexcept
{
    try{Borrowed shared;return shared.data&&InterlockedCompareExchange(&shared.data->cancelled,0,0)!=0;}
    catch(...){return false;}
}
bool StartupProgressActive() noexcept
{
    try{Borrowed shared;return shared.data&&InterlockedCompareExchange(&shared.data->stage,0,0)<static_cast<LONG>(StartupStage::Ready);}
    catch(...){return false;}
}
StartupProgressChannel::StartupProgressChannel()
{
    static std::atomic<unsigned> next=0;
    previous_=ChannelName();
    name_=L"Local\\FlareStartup-"+std::to_wstring(GetCurrentProcessId())+L"-"+std::to_wstring(GetTickCount64())+L"-"+std::to_wstring(++next);
    mapping_=CreateFileMappingW(INVALID_HANDLE_VALUE,nullptr,PAGE_READWRITE,0,sizeof(SharedProgress),name_.c_str());
    if(mapping_)data_=MapViewOfFile(mapping_,FILE_MAP_ALL_ACCESS,0,0,sizeof(SharedProgress));
    if(data_){Reset();SetEnvironmentVariableW(Environment,name_.c_str());}
}
StartupProgressChannel::~StartupProgressChannel()
{
    if(ChannelName()==name_)SetEnvironmentVariableW(Environment,previous_.empty()?nullptr:previous_.c_str());
    if(data_)UnmapViewOfFile(data_);if(mapping_)CloseHandle(mapping_);
}
void StartupProgressChannel::Reset() noexcept
{
    if(!data_)return;auto& data=*static_cast<SharedProgress*>(data_);
    InterlockedExchange(&data.cancelled,0);InterlockedExchange(&data.percent,5);InterlockedExchange(&data.stage,0);
}
void StartupProgressChannel::Cancel() noexcept
{if(data_)InterlockedExchange(&static_cast<SharedProgress*>(data_)->cancelled,1);}
StartupSnapshot StartupProgressChannel::Read() const noexcept
{
    if(!data_)return {};auto& data=*static_cast<SharedProgress*>(data_);
    const LONG stage=std::clamp(InterlockedCompareExchange(&data.stage,0,0),0L,static_cast<LONG>(StartupStage::Ready));
    const LONG percent=InterlockedCompareExchange(&data.percent,0,0);
    return {static_cast<StartupStage>(stage),static_cast<int>(std::max(percent,static_cast<LONG>(percentages[stage]))),InterlockedCompareExchange(&data.cancelled,0,0)!=0};
}
}
