#pragma once

#include "ToolLayer/Editor/EditorUi/EditorCommand.h"
#include "ToolLayer/Editor/EditorUi/EditorCommandRouter.h"

#include <string>
#include <vector>

namespace se::ui::editor
{

struct EditorMenuItem
{
    std::string menu;
    std::string label;
    EditorCommandId command = EditorCommandId::Save;
    std::string argument;
};

struct EditorToolbarItem
{
    std::string label;
    EditorCommandId command = EditorCommandId::Save;
};

class EditorChrome
{
public:
    EditorChrome(EditorCommandRouter& router, EditorCommandBus& bus);

    void AddMenu(std::string menu, std::string label, EditorCommandId command, std::string argument = {});
    void AddToolbar(std::string label, EditorCommandId command);
    bool ActivateMenu(std::size_t index);
    bool ActivateToolbar(std::size_t index);
    void SetStatus(std::string status);
    [[nodiscard]] const std::string& Status() const noexcept;
    [[nodiscard]] const std::vector<EditorMenuItem>& Menus() const noexcept;
    [[nodiscard]] const std::vector<EditorToolbarItem>& Toolbar() const noexcept;

private:
    EditorCommandRouter* router_;
    EditorCommandBus* bus_;
    std::vector<EditorMenuItem> menus_;
    std::vector<EditorToolbarItem> toolbar_;
    std::string status_;
};

} // namespace se::ui::editor
