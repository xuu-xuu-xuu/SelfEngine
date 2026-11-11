#include "ToolLayer/Editor/EditorUi/EditorChrome.h"

#include <utility>

namespace se::ui::editor
{

EditorChrome::EditorChrome(EditorCommandRouter& router, EditorCommandBus& bus)
    : router_(&router), bus_(&bus)
{
}

void EditorChrome::AddMenu(std::string menu, std::string label, EditorCommandId command, std::string argument)
{
    menus_.push_back({std::move(menu), std::move(label), command, std::move(argument)});
}

void EditorChrome::AddToolbar(std::string label, EditorCommandId command)
{
    toolbar_.push_back({std::move(label), command});
}

bool EditorChrome::ActivateMenu(std::size_t index)
{
    if (index >= menus_.size() || !router_ || !bus_ || !bus_->CanExecute(menus_[index].command))
    {
        return false;
    }
    return router_->Trigger(menus_[index].command, menus_[index].argument);
}

bool EditorChrome::ActivateToolbar(std::size_t index)
{
    if (index >= toolbar_.size() || !router_ || !bus_ || !bus_->CanExecute(toolbar_[index].command))
    {
        return false;
    }
    return router_->Trigger(toolbar_[index].command);
}

void EditorChrome::SetStatus(std::string status)
{
    status_ = std::move(status);
}

const std::string& EditorChrome::Status() const noexcept
{
    return status_;
}

const std::vector<EditorMenuItem>& EditorChrome::Menus() const noexcept
{
    return menus_;
}

const std::vector<EditorToolbarItem>& EditorChrome::Toolbar() const noexcept
{
    return toolbar_;
}

} // namespace se::ui::editor
