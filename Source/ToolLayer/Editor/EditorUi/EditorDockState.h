#pragma once

#include "ToolLayer/Editor/EditorUi/EditorDock.h"

#include <filesystem>
#include <string>
#include <string_view>

namespace se::ui::editor
{

class EditorDockState
{
public:
    static bool Save(const EditorDock& workspace,const std::filesystem::path& path,std::string& error);
    static bool Load(const std::filesystem::path& path,EditorDock& workspace,std::string& error);
    [[nodiscard]] static std::string Serialize(const EditorDock& workspace);
    static bool Deserialize(std::string_view json,EditorDock& workspace,std::string& error);
    static bool Save(const DockNode& layout, const std::filesystem::path& path, std::string& error);
    static bool Load(const std::filesystem::path& path, DockNode& layout, std::string& error);
    [[nodiscard]] static std::string Serialize(const DockNode& layout);
    static bool Deserialize(std::string_view json, DockNode& layout, std::string& error);
};

} // namespace se::ui::editor
