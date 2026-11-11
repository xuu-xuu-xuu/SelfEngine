#include "BuildRestartRequest.h"

#include <fstream>

namespace se
{
bool WriteBuildRestartRequest(const std::filesystem::path& path, std::string& error)
{
    error.clear();
    if (path.empty())
    {
        error = "Editor.BuildControlInvalid";
        return false;
    }
    std::error_code fileError;
    if (!path.parent_path().empty())
    {
        std::filesystem::create_directories(path.parent_path(), fileError);
        if (fileError)
        {
            error = "Editor.BuildControlInvalid";
            return false;
        }
    }
    std::ofstream output(path, std::ios::binary | std::ios::trunc);
    if (!output)
    {
        error = "Editor.BuildControlInvalid";
        return false;
    }
    output << "{\"schemaVersion\":1,\"action\":\"compile\"}\n";
    if (!output)
    {
        error = "Editor.BuildControlInvalid";
        return false;
    }
    return true;
}
} // namespace se
