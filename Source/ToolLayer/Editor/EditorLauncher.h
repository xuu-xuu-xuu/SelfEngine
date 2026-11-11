#pragma once
#include <cstdint>
#include <filesystem>
#include <string>

namespace se
{
// 正式编辑器入口参数。--worker/--smoke-* 是内部参数，不属于用户工作流。
struct EditorLaunchOptions
{
    std::filesystem::path project;
    std::filesystem::path scene;
    std::filesystem::path capture;
    std::filesystem::path captureLauncher;
    std::filesystem::path smokeOpenProject;
    std::filesystem::path smokeCreateParent;
    std::filesystem::path smokeExportReport;
    std::filesystem::path smokeWorkflow;
    std::string smokeExportName;
    std::uint64_t smokeCancelMilliseconds=0;
    bool smokePlayback=false;
    std::string locale="zh-CN";
    std::string editorUi="toolkit";
    std::uint64_t frames=0;
    std::uint64_t smokeLauncherMs=0;
    bool safeMode=false;
    bool worker=false;
    bool hidden=false;
    bool demo=false;
    bool uiErrors=true;
};
// 解析失败时写入 ASCII error，调用方负责显示 usage。
bool ParseEditorLaunchOptions(int argc,wchar_t** argv,EditorLaunchOptions& options,std::string& error);
// 无项目参数时显示启动页；显式项目直接建立编辑工作进程；内部自检模式运行后自动退出。
int RunEditorLauncher(const EditorLaunchOptions& options);
}
