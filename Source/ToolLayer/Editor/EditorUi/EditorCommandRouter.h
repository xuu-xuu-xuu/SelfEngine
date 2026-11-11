#pragma once

#include "FunctionLayer/UI/Core/UiInput.h"
#include "ToolLayer/Editor/EditorUi/EditorCommand.h"

#include <vector>

namespace se::ui::editor
{

struct EditorShortcut
{
    UiKeyCode key = 0;
    bool control = false;
    bool shift = false;
    bool alt = false;
    EditorCommandId command = EditorCommandId::Save;
    std::string argument;
};

class EditorCommandRouter
{
public:
    explicit EditorCommandRouter(EditorCommandBus& bus);

    void RegisterShortcut(UiKeyCode key, bool control, bool shift, bool alt,
                          EditorCommandId command, std::string argument = {});
    // 文本框聚焦时让位；返回是否命中并执行。
    bool RouteKey(const UiInputEvent& event, bool textFieldFocused);
    bool Trigger(EditorCommandId command, std::string argument = {});
    [[nodiscard]] bool HasShortcut(UiKeyCode key, bool control, bool shift, bool alt) const noexcept;
    [[nodiscard]] const std::vector<EditorShortcut>& Shortcuts() const noexcept;

private:
    EditorCommandBus* bus_;
    std::vector<EditorShortcut> shortcuts_;
};

} // namespace se::ui::editor
