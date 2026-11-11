#pragma once
#include "EditorSessionStore.h"
#include <functional>

namespace se
{
struct EditorExternalDocument
{
    RecoveryDocument document;
    bool dirty=false;
};
// 领域宿主提供自有序列化、校验和保存；Tool 统一显示状态、关闭保护及完整恢复组。
struct EditorExternalDocuments
{
    std::function<bool(std::vector<EditorExternalDocument>&,std::string&)> snapshot;
    std::function<bool(const std::string&,std::string&)> save;
    // apply=false 只校验候选，不能改变当前文档；apply=true 仅恢复内存，不写原文件。
    std::function<bool(const RecoveryDocument&,bool apply,std::string&)> restore;
};
}
