#include "ExternalChangeMonitor.h"

#include <system_error>

namespace se
{
void ExternalChangeMonitor::Reset(const std::filesystem::path& path)
{
    path_ = path;
    modified_ = {};
    size_ = 0;
    valid_ = false;
    if (path_.empty())
    {
        return;
    }
    std::error_code error;
    if (!std::filesystem::is_regular_file(path_, error) || error)
    {
        return;
    }
    modified_ = std::filesystem::last_write_time(path_, error);
    if (error)
    {
        return;
    }
    size_ = std::filesystem::file_size(path_, error);
    valid_ = !error;
}

bool ExternalChangeMonitor::Poll() const
{
    if (!valid_ || path_.empty())
    {
        return false;
    }
    std::error_code error;
    if (!std::filesystem::is_regular_file(path_, error) || error)
    {
        return false;
    }
    const auto modified = std::filesystem::last_write_time(path_, error);
    if (error)
    {
        return false;
    }
    const auto size = std::filesystem::file_size(path_, error);
    if (error)
    {
        return false;
    }
    return modified != modified_ || size != size_;
}

void ExternalChangeMonitor::Consume()
{
    if (path_.empty())
    {
        return;
    }
    std::error_code error;
    if (!std::filesystem::is_regular_file(path_, error) || error)
    {
        valid_ = false;
        return;
    }
    modified_ = std::filesystem::last_write_time(path_, error);
    if (error)
    {
        valid_ = false;
        return;
    }
    size_ = std::filesystem::file_size(path_, error);
    valid_ = !error;
}
} // namespace se
