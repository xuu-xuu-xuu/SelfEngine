#pragma once
#include "EditorSessionStore.h"
#include "ProjectSettingsService.h"

namespace se
{
enum class EditorSettingsKind { Project,Build };

// 设置面板的文档状态：完整 JSON 留存未知字段，编辑缓冲与最后读盘字节分别保存。
class EditorSettingsDocument
{
public:
    explicit EditorSettingsDocument(EditorSettingsKind kind):kind_(kind){}
    // path 必须为项目根中的 project.json / build.json；缺少 build.json 时载入空的新文档。
    // 脏文档默认拒绝重载；只有调用方已取得丢弃决定时才传 discardUnsaved=true。
    bool Load(const std::filesystem::path& path,std::string& error,bool discardUnsaved=false);
    bool Loaded()const noexcept{return loaded_;}
    bool SourceExists()const noexcept{return sourceExists_;}
    bool Dirty()const noexcept{return loaded_&&content_!=savedContent_;}
    EditorSettingsKind Kind()const noexcept{return kind_;}
    const std::filesystem::path& Path()const noexcept{return path_;}
    const ProjectSettings& Project()const noexcept{return project_;}
    const BuildSettings& Build()const noexcept{return build_;}
    const std::string& SourceDigest()const noexcept{return sourceDigest_;}
    std::uint64_t Revision()const noexcept{return revision_;}
    // Set 保留尚未合法的用户输入；引用与字段合法性在 Save 时统一检查。
    bool Set(const ProjectSettings& settings,std::string& error);
    bool Set(const BuildSettings& settings,std::string& error);
    // 显式 Save 可创建尚不存在的 build.json；既有文件发生外部变化时拒绝覆盖。
    bool Save(std::string& error,std::string& detail);
    bool Snapshot(RecoveryDocument& document,std::string& error)const;
    // apply=false 只校验完整 JSON、文档身份、路径及源摘要；apply=true 仅替换内存。
    bool Restore(const RecoveryDocument& document,bool apply,std::string& error);
private:
    bool CheckSource(std::string& error)const;
    EditorSettingsKind kind_;
    bool loaded_=false,sourceExists_=false;
    std::filesystem::path path_;
    ProjectSettings project_;
    BuildSettings build_;
    std::string sourceDigest_,content_,savedContent_;
    std::uint64_t revision_=0;
};
}
