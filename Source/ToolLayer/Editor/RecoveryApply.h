#pragma once
#include "EditorSession.h"
#include "EditorSessionStore.h"
#include <filesystem>
#include <string>

namespace se
{
// 把恢复组里最近的 .scene.json 快照写到 directory/recovered.scene.json 并打开。
// 不删除恢复组；调用方决定 Discard。
bool ApplyRecoveryScene(const RecoveryGroup& group, const std::filesystem::path& directory,
    EditorSession& session, std::string& error);
} // namespace se
