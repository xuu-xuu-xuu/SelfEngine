#pragma once
#include <cstdint>
#include <filesystem>
#include <functional>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

namespace se
{
enum class EditorCommandResult { Succeeded, Failed, Rejected, Deferred };
struct EditorCommandOutcome
{
    EditorCommandResult state=EditorCommandResult::Failed;
    std::string diagnosticKey,detail;
    static EditorCommandOutcome Succeeded(){return {EditorCommandResult::Succeeded,{}, {}};}
    static EditorCommandOutcome Failed(std::string key,std::string detail={}){return {EditorCommandResult::Failed,std::move(key),std::move(detail)};}
    static EditorCommandOutcome Rejected(std::string key,std::string detail={}){return {EditorCommandResult::Rejected,std::move(key),std::move(detail)};}
};
struct EditorCommandContext
{
    std::uint64_t requestId=0,workspaceGeneration=0;
};
// 菜单、工具栏、快捷键和自动化共用同一命令路由；不依赖 ImGui。
class EditorCommandRouter
{
public:
    using Handler=std::function<EditorCommandOutcome(const EditorCommandContext&)>;
    using EnableCheck=std::function<bool(std::string& reasonKey)>;
    bool Register(std::string id,Handler handler,EnableCheck check={});
    bool Registered(std::string_view id) const;
    bool Enabled(std::string_view id,std::string& reasonKey) const;
    EditorCommandOutcome Execute(std::string_view id,const EditorCommandContext& context);
    std::size_t Count() const noexcept{return entries_.size();}
    void SetWorkspaceGeneration(std::uint64_t generation){generation_=generation;}
    std::uint64_t WorkspaceGeneration() const noexcept{return generation_;}
private:
    struct Entry{Handler handler;EnableCheck check;};
    std::unordered_map<std::string,Entry> entries_;
    std::uint64_t generation_=0;
};

struct EditorDocumentEntry
{
    std::string id,type;
    std::filesystem::path path;
    std::uint64_t savedRevision=0,editRevision=0;
    std::uint64_t fileSize=0;
    std::filesystem::file_time_type fileTime{};
    bool hasStamp=false;
};
struct EditorDocumentSaveResult
{
    std::string id,diagnosticKey,detail;
    bool saved=false;
};
// 文档身份、修订与脏状态；领域模型的撤销仍由各自会话拥有。
class EditorDocumentRegistry
{
public:
    bool Register(std::string id,std::string type,const std::filesystem::path& path,std::string& error);
    bool Registered(std::string_view id) const;
    const EditorDocumentEntry* Find(std::string_view id) const;
    std::size_t Size() const noexcept{return documents_.size();}
    bool MarkEdited(std::string_view id);
    bool MarkSaved(std::string_view id,std::string& error);
    // 撤销回保存点或更早时调用；revision <= savedRevision 即干净。
    bool RevertTo(std::string_view id,std::uint64_t revision);
    bool Dirty(std::string_view id) const;
    std::size_t DirtyCount() const;
    std::vector<std::string> DirtyDocuments() const;
    bool HasExternalChange(std::string_view id) const;
    // 外部变更处理：重新加载丢弃编辑修订；确认覆盖采纳当前磁盘内容为基线；另存为新路径。
    bool MarkReloaded(std::string_view id,std::string& error);
    bool AcceptExternal(std::string_view id,std::string& error);
    bool SaveAs(std::string_view id,const std::filesystem::path& path,std::string& error);
    // 保存全部：逐个调用，成功者变干净，失败者保持脏；跨文件不是事务。
    std::vector<EditorDocumentSaveResult> SaveAll(const std::function<bool(const EditorDocumentEntry&,std::string&,std::string&)>& save);
private:
    static bool ReadFileStamp(const std::filesystem::path& path,std::uint64_t& size,std::filesystem::file_time_type& time);
    std::unordered_map<std::string,EditorDocumentEntry> documents_;
};
}
