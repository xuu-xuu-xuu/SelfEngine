#pragma once
#include <Windows.h>
#include <string>

namespace se
{
enum class StartupStage : LONG
{
    Project,Scripts,RestoreHost,PublishHost,VerifyRuntime,RestoreScripts,BuildScripts,ValidateScripts,
    Runtime,Assets,Scene,Interface,Renderer,FirstFrame,Ready
};
struct StartupSnapshot {StartupStage stage=StartupStage::Project;int percent=0;bool cancelled=false;};
const char* StartupStageKey(StartupStage stage) noexcept;
// 百分比是已到达的工程阶段；首帧成功之前不得报告 Ready。
bool ReportStartup(StartupStage stage) noexcept;
bool StartupCancelled() noexcept;
bool StartupProgressActive() noexcept;

// 每个 Worker 使用唯一共享映射，只把名字传给受控子进程；不写用户项目文件。
class StartupProgressChannel
{
public:
    StartupProgressChannel();
    ~StartupProgressChannel();
    StartupProgressChannel(const StartupProgressChannel&)=delete;
    StartupProgressChannel& operator=(const StartupProgressChannel&)=delete;
    void Reset() noexcept;
    void Cancel() noexcept;
    StartupSnapshot Read() const noexcept;
    bool Valid() const noexcept {return data_!=nullptr;}
private:
    HANDLE mapping_=nullptr;
    void* data_=nullptr;
    std::wstring name_,previous_;
};
}
