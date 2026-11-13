#pragma once
#include "EditorProcessRunner.h"
#include <Windows.h>

namespace se::detail
{
// 窄 Win32 接缝用于确定性覆盖 Job 失败，不通过环境变量或用户参数削弱正式路径。
struct ProcessJobApi
{
    decltype(&CreateJobObjectW) createJob=&CreateJobObjectW;
    decltype(&SetInformationJobObject) setLimits=&SetInformationJobObject;
    decltype(&AssignProcessToJobObject) assign=&AssignProcessToJobObject;
    decltype(&ResumeThread) resume=&ResumeThread;
};
bool RunEditorProcessWithApi(const std::wstring& executable,const std::vector<std::wstring>& arguments,
    const std::filesystem::path& workingDirectory,const std::filesystem::path& logFile,
    const std::atomic<bool>* cancel,ProcessRunResult& result,const ProcessJobApi& api,
    const std::function<void()>& tick={},bool showWindow=false);
}
