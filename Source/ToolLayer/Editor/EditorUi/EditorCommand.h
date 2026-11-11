#pragma once

#include <cstdint>
#include <functional>
#include <string>
#include <string_view>
#include <vector>

namespace se::ui::editor
{

enum class EditorCommandId : std::uint32_t
{
    Save = 0,
    SaveAll,
    Open,
    SaveAs,
    NewScene,
    Undo,
    Redo,
    Play,
    Pause,
    Step,
    Stop,
    Build,
    Import,
    Export,
    Delete,
    Duplicate,
    Rename,
    TogglePanel,
    FocusViewport,
    OpenSettings,
    SetProperty,
    CreateEntity,
    ReparentEntity,
};

struct EditorCommand
{
    EditorCommandId id = EditorCommandId::Save;
    std::string argument;
};

using EditorCommandHandler = std::function<bool(const EditorCommand&)>;

// 面板只发命令；具体保存/撤销/Play/导入/导出由 Tool 层处理器执行。
class EditorCommandBus
{
public:
    void Register(EditorCommandId id, EditorCommandHandler handler);
    bool Execute(const EditorCommand& command);
    [[nodiscard]] bool CanExecute(EditorCommandId id) const noexcept;
    void Clear();

private:
    struct Entry
    {
        EditorCommandHandler handler;
    };
    std::vector<Entry> entries_;
};

[[nodiscard]] const char* EditorCommandName(EditorCommandId id) noexcept;

} // namespace se::ui::editor
