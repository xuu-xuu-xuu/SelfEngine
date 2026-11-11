#include "ToolLayer/Editor/EditorUi/EditorCommand.h"

#include <utility>
#include <vector>

namespace se::ui::editor
{

void EditorCommandBus::Register(EditorCommandId id, EditorCommandHandler handler)
{
    const std::size_t index = static_cast<std::size_t>(id);
    if (entries_.size() <= index)
    {
        entries_.resize(index + 1);
    }
    entries_[index].handler = std::move(handler);
}

bool EditorCommandBus::Execute(const EditorCommand& command)
{
    const std::size_t index = static_cast<std::size_t>(command.id);
    if (index >= entries_.size() || !entries_[index].handler)
    {
        return false;
    }
    return entries_[index].handler(command);
}

bool EditorCommandBus::CanExecute(EditorCommandId id) const noexcept
{
    const std::size_t index = static_cast<std::size_t>(id);
    return index < entries_.size() && static_cast<bool>(entries_[index].handler);
}

void EditorCommandBus::Clear()
{
    entries_.clear();
}

const char* EditorCommandName(EditorCommandId id) noexcept
{
    switch (id)
    {
    case EditorCommandId::Save: return "save";
    case EditorCommandId::SaveAll: return "save-all";
    case EditorCommandId::Open: return "open";
    case EditorCommandId::SaveAs: return "save-as";
    case EditorCommandId::NewScene: return "new-scene";
    case EditorCommandId::Undo: return "undo";
    case EditorCommandId::Redo: return "redo";
    case EditorCommandId::Play: return "play";
    case EditorCommandId::Pause: return "pause";
    case EditorCommandId::Step: return "step";
    case EditorCommandId::Stop: return "stop";
    case EditorCommandId::Build: return "build";
    case EditorCommandId::Import: return "import";
    case EditorCommandId::Export: return "export";
    case EditorCommandId::Delete: return "delete";
    case EditorCommandId::Duplicate: return "duplicate";
    case EditorCommandId::Rename: return "rename";
    case EditorCommandId::TogglePanel: return "toggle-panel";
    case EditorCommandId::FocusViewport: return "focus-viewport";
    case EditorCommandId::OpenSettings: return "open-settings";
    case EditorCommandId::SetProperty: return "set-property";
    case EditorCommandId::CreateEntity: return "create-entity";
    case EditorCommandId::ReparentEntity: return "reparent-entity";
    }
    return "unknown";
}

} // namespace se::ui::editor
