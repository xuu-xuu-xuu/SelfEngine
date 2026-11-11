#pragma once
#include <cstdint>
#include <filesystem>
#include <string>

namespace se
{
// 编辑工作进程：拥有唯一项目的编辑世界与编辑会话。
// 正式启动器 SelfEngineEditor 与 EditorSandbox 共用本入口，不复制第二套编辑流程。
struct EditorWorkerOptions
{
    std::filesystem::path projectRoot;
    std::filesystem::path scene;
    std::filesystem::path capture;
    std::string locale="zh-CN";
    std::string editorUi="toolkit";
    std::uint64_t frames=0;
    bool hidden=false;
    bool demo=false;
    // 自动化自检：从真实编辑器入口触发一次导出并写回结果，用于 U08 可复现验收。
    std::filesystem::path smokeExportReport;
    std::filesystem::path smokeWorkflow;
    std::string smokeExportName;
    std::uint64_t smokeCancelMilliseconds=0;
    bool smokePlayback=false;
};
int RunEditorWorker(const EditorWorkerOptions& options);
}
