#pragma once
#include <cstdint>
#include <filesystem>

namespace se
{
// 只观察显式文档路径的 mtime/size；不轮询目录、不监听整个项目。
class ExternalChangeMonitor
{
public:
    void Reset(const std::filesystem::path& path);
    // 文件存在且 mtime/size 与基线不同返回 true；缺失文件不自动当成变更。
    [[nodiscard]] bool Poll() const;
    // 调用方处理后把基线更新到当前文件。
    void Consume();
    [[nodiscard]] const std::filesystem::path& Path() const noexcept { return path_; }

private:
    std::filesystem::path path_;
    std::filesystem::file_time_type modified_{};
    std::uintmax_t size_ = 0;
    bool valid_ = false;
};
} // namespace se
