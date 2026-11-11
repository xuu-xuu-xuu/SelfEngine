#pragma once
#include "EditorCommand.h"
#include <filesystem>
#include <memory>
#include <string>

namespace se
{
class ImGuiEditor;
class EditorSession;
// 自动化驱动正式命令/导出入口；工作流模式仅修改显式隔离夹具，不冒充鼠标或真人操作。
// RunEditorApp 同时承载 native 与 managed，因此烟测也只在该层建立一次。
class EditorSmokeDriver
{
public:
    static EditorSmokeDriver FromEnvironment(const std::filesystem::path& projectRoot);
    // 只接受隔离项目 out/Editor/workflow-smoke/<case>/workflow.json；有限阶段及补丁均留在该目录。
    static bool ValidateWorkflow(const std::filesystem::path& projectRoot,const std::filesystem::path& config,
        std::size_t& stageCount,std::string& detail);
    ~EditorSmokeDriver();
    EditorSmokeDriver(EditorSmokeDriver&&) noexcept;
    EditorSmokeDriver& operator=(EditorSmokeDriver&&) noexcept;
    EditorSmokeDriver(const EditorSmokeDriver&)=delete;
    EditorSmokeDriver& operator=(const EditorSmokeDriver&)=delete;
    bool Enabled() const noexcept;
    void OperationFrame(ImGuiEditor& frontend,EditorSession& session,
        std::function<EditorCommandOutcome(std::string_view)> execute={},
        std::function<void()> requestClose={});
    // 调用方把真实宿主退出码传回；未完成烟测/报表写入失败不能报告成功。
    int FinalResult(int hostExit);
private:
    struct State;
    explicit EditorSmokeDriver(std::unique_ptr<State> state);
    std::unique_ptr<State> state_;
};
}
