#pragma once

#include <cstddef>
#include <filesystem>
#include <string>
#include <string_view>

namespace se
{
// editor-dock.ini 的磁盘契约。布局语义由 ImGui docking 解释，本类只负责
// 有界读取、格式门禁、原子写入和备份，避免坏文件被当作有效布局覆盖。
class EditorDockLayout
{
public:
    static constexpr std::size_t MaximumBytes=1024*1024;
    static constexpr std::string_view DockingSection="[Docking][Data]";
    static bool Validate(std::string_view text,std::string& errorKey);
    static bool Load(const std::filesystem::path& path,std::string& text,std::string& errorKey);
    static bool Save(const std::filesystem::path& path,std::string_view text,std::string& errorKey);
    static std::filesystem::path BackupPath(std::filesystem::path path);
};
}
