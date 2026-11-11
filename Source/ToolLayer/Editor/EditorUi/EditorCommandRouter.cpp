#include "ToolLayer/Editor/EditorUi/EditorCommandRouter.h"

#include <utility>

namespace se::ui::editor
{

EditorCommandRouter::EditorCommandRouter(EditorCommandBus& bus)
    : bus_(&bus)
{
}

void EditorCommandRouter::RegisterShortcut(UiKeyCode key, bool control, bool shift, bool alt,
                                           EditorCommandId command, std::string argument)
{
    shortcuts_.push_back({key, control, shift, alt, command, std::move(argument)});
}

bool EditorCommandRouter::RouteKey(const UiInputEvent& event, bool textFieldFocused)
{
    if (textFieldFocused || event.type != UiEventType::KeyDown)
    {
        return false;
    }
    for (const EditorShortcut& shortcut : shortcuts_)
    {
        if (shortcut.key == event.key && shortcut.control == event.control &&
            shortcut.shift == event.shift && shortcut.alt == event.alt)
        {
            return Trigger(shortcut.command, shortcut.argument);
        }
    }
    return false;
}

bool EditorCommandRouter::Trigger(EditorCommandId command, std::string argument)
{
    return bus_ && bus_->Execute({command, std::move(argument)});
}

bool EditorCommandRouter::HasShortcut(UiKeyCode key, bool control, bool shift, bool alt) const noexcept
{
    for (const EditorShortcut& shortcut : shortcuts_)
    {
        if (shortcut.key == key && shortcut.control == control && shortcut.shift == shift &&
            shortcut.alt == alt)
        {
            return true;
        }
    }
    return false;
}

const std::vector<EditorShortcut>& EditorCommandRouter::Shortcuts() const noexcept
{
    return shortcuts_;
}

} // namespace se::ui::editor
