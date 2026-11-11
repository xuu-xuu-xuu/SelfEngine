#pragma once
#include <atomic>
#include <filesystem>
#include <functional>
#include <string>
#include <vector>

namespace se
{
struct ProcessRunResult
{
    bool started=false;
    bool cancelled=false;
    int exitCode=-1;
    std::string output; // 日志尾部，供失败诊断；完整输出留在日志文件。
};
// Windows PowerShell 的固定位置；不存在时回退到 PATH 中的 powershell.exe。
std::wstring WindowsPowerShellPath();
// 生成可传给 CreateProcessW 的参数串：每个参数按 Windows 命令行规则加引号。
std::wstring BuildProcessCommandLine(const std::wstring& executable,const std::vector<std::wstring>& arguments);
// 以隐藏窗口启动子进程，stdout/stderr 直接写入日志文件，不创建管道。
// cancel 置位时只终止本次创建的子进程树；tick 在等待期间周期性调用，用于读取进度报告。
// showWindow 为 true 时让子进程的首次 ShowWindow 正常显示；交互式编辑器宿主必须传 true，
// 否则 STARTUPINFO 的 SW_HIDE 会覆盖引擎窗口的 ShowWindow，编辑器只在后台运行而不显示窗口。
bool RunEditorProcess(const std::wstring& executable,const std::vector<std::wstring>& arguments,
    const std::filesystem::path& workingDirectory,const std::filesystem::path& logFile,
    const std::atomic<bool>* cancel,ProcessRunResult& result,
    const std::function<void()>& tick={},bool showWindow=false);
// 启动后不等待，用于“运行导出游戏”；返回是否成功创建进程。
bool StartEditorProcess(const std::wstring& executable,const std::vector<std::wstring>& arguments,
    const std::filesystem::path& workingDirectory,std::string& error);
}
