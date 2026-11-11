#include "RecoveryApply.h"

#include <fstream>

namespace se
{
bool ApplyRecoveryScene(const RecoveryGroup& group, const std::filesystem::path& directory,
    EditorSession& session, std::string& error)
{
    error.clear();
    if (directory.empty())
    {
        error = "Editor.RecoveryInvalidPath";
        return false;
    }
    for (const RecoveryDocument& document : group.documents)
    {
        if (!document.relativePath.ends_with(".scene.json"))
        {
            continue;
        }
        const auto path = directory / "recovered.scene.json";
        std::ofstream output(path, std::ios::binary | std::ios::trunc);
        output << document.content;
        output.close();
        if (!output)
        {
            error = "Editor.RecoveryWriteFailed";
            return false;
        }
        if (!session.Open(path, true))
        {
            error = session.LastError();
            return false;
        }
        return true;
    }
    error = "Editor.RecoveryNoScene";
    return false;
}
} // namespace se
