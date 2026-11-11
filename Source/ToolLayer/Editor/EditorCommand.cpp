#include "EditorCommand.h"
#include <algorithm>
#include <fstream>

namespace se
{
bool EditorCommandRouter::Register(std::string id,Handler handler,EnableCheck check)
{
    if(id.empty()||!handler)return false;
    entries_.insert_or_assign(std::move(id),Entry{std::move(handler),std::move(check)});
    return true;
}
bool EditorCommandRouter::Registered(std::string_view id) const
{
    return entries_.find(std::string(id))!=entries_.end();
}
bool EditorCommandRouter::Enabled(std::string_view id,std::string& reasonKey) const
{
    reasonKey.clear();
    const auto found=entries_.find(std::string(id));
    if(found==entries_.end()){reasonKey="Editor.CommandUnknown";return false;}
    if(found->second.check&&!found->second.check(reasonKey))return false;
    return true;
}
EditorCommandOutcome EditorCommandRouter::Execute(std::string_view id,const EditorCommandContext& context)
{
    const auto found=entries_.find(std::string(id));
    if(found==entries_.end())return EditorCommandOutcome::Rejected("Editor.CommandUnknown",std::string(id));
    if(generation_&&context.workspaceGeneration!=generation_)
        return EditorCommandOutcome::Rejected("Editor.CommandStaleWorkspace",std::string(id));
    std::string reason;
    if(found->second.check&&!found->second.check(reason))
        return EditorCommandOutcome::Rejected(reason.empty()?"Editor.CommandDisabled":reason,std::string(id));
    return found->second.handler(context);
}

bool EditorDocumentRegistry::ReadFileStamp(const std::filesystem::path& path,std::uint64_t& size,std::filesystem::file_time_type& time)
{
    std::error_code error;
    if(!std::filesystem::is_regular_file(path,error))return false;
    size=std::filesystem::file_size(path,error);
    if(error)return false;
    time=std::filesystem::last_write_time(path,error);
    return !error;
}
bool EditorDocumentRegistry::Register(std::string id,std::string type,const std::filesystem::path& path,std::string& error)
{
    error.clear();
    if(id.empty()||type.empty()||path.empty()){error="invalid document";return false;}
    EditorDocumentEntry entry;
    entry.id=std::move(id);entry.type=std::move(type);entry.path=path;
    entry.hasStamp=ReadFileStamp(entry.path,entry.fileSize,entry.fileTime);
    documents_.insert_or_assign(entry.id,std::move(entry));
    return true;
}
bool EditorDocumentRegistry::Registered(std::string_view id) const
{
    return documents_.find(std::string(id))!=documents_.end();
}
const EditorDocumentEntry* EditorDocumentRegistry::Find(std::string_view id) const
{
    const auto found=documents_.find(std::string(id));
    return found==documents_.end()?nullptr:&found->second;
}
bool EditorDocumentRegistry::MarkEdited(std::string_view id)
{
    const auto found=documents_.find(std::string(id));
    if(found==documents_.end())return false;
    ++found->second.editRevision;
    return true;
}
bool EditorDocumentRegistry::MarkSaved(std::string_view id,std::string& error)
{
    error.clear();
    const auto found=documents_.find(std::string(id));
    if(found==documents_.end()){error="unknown document";return false;}
    found->second.hasStamp=ReadFileStamp(found->second.path,found->second.fileSize,found->second.fileTime);
    if(!found->second.hasStamp){error="saved document cannot be read back";return false;}
    found->second.savedRevision=found->second.editRevision;
    return true;
}
bool EditorDocumentRegistry::RevertTo(std::string_view id,std::uint64_t revision)
{
    const auto found=documents_.find(std::string(id));
    if(found==documents_.end()||revision>found->second.editRevision)return false;
    found->second.editRevision=revision;
    return true;
}
bool EditorDocumentRegistry::Dirty(std::string_view id) const
{
    const auto found=documents_.find(std::string(id));
    return found!=documents_.end()&&found->second.editRevision!=found->second.savedRevision;
}
std::size_t EditorDocumentRegistry::DirtyCount() const
{
    return static_cast<std::size_t>(std::count_if(documents_.begin(),documents_.end(),
        [](const auto& item){return item.second.editRevision!=item.second.savedRevision;}));
}
std::vector<std::string> EditorDocumentRegistry::DirtyDocuments() const
{
    std::vector<std::string> result;
    for(const auto& [id,entry]:documents_)if(entry.editRevision!=entry.savedRevision)result.push_back(id);
    std::sort(result.begin(),result.end());
    return result;
}
bool EditorDocumentRegistry::HasExternalChange(std::string_view id) const
{
    const auto found=documents_.find(std::string(id));
    if(found==documents_.end())return false;
    if(!found->second.hasStamp)return false;
    std::uint64_t size=0;std::filesystem::file_time_type time{};
    if(!ReadFileStamp(found->second.path,size,time))return true;
    return size!=found->second.fileSize||time!=found->second.fileTime;
}
bool EditorDocumentRegistry::MarkReloaded(std::string_view id,std::string& error)
{
    error.clear();
    const auto found=documents_.find(std::string(id));
    if(found==documents_.end()){error="unknown document";return false;}
    found->second.hasStamp=ReadFileStamp(found->second.path,found->second.fileSize,found->second.fileTime);
    if(!found->second.hasStamp){error="document cannot be read back";return false;}
    found->second.editRevision=found->second.savedRevision;
    return true;
}
bool EditorDocumentRegistry::AcceptExternal(std::string_view id,std::string& error)
{
    error.clear();
    const auto found=documents_.find(std::string(id));
    if(found==documents_.end()){error="unknown document";return false;}
    found->second.hasStamp=ReadFileStamp(found->second.path,found->second.fileSize,found->second.fileTime);
    if(!found->second.hasStamp){error="document cannot be read back";return false;}
    return true;
}
bool EditorDocumentRegistry::SaveAs(std::string_view id,const std::filesystem::path& path,std::string& error)
{
    error.clear();
    const auto found=documents_.find(std::string(id));
    if(found==documents_.end()){error="unknown document";return false;}
    if(path.empty()){error="empty save path";return false;}
    found->second.path=path;
    found->second.hasStamp=ReadFileStamp(found->second.path,found->second.fileSize,found->second.fileTime);
    if(!found->second.hasStamp){error="saved document cannot be read back";return false;}
    found->second.savedRevision=found->second.editRevision;
    return true;
}
std::vector<EditorDocumentSaveResult> EditorDocumentRegistry::SaveAll(const std::function<bool(const EditorDocumentEntry&,std::string&,std::string&)>& save)
{
    std::vector<EditorDocumentSaveResult> results;
    auto ordered=DirtyDocuments();
    const auto rank=[](const std::string& id){return id=="material"?0:id=="ui"?1:id=="script-bindings"?2:id=="scene"?3:id=="project-settings"?4:id=="build-settings"?5:6;};
    std::stable_sort(ordered.begin(),ordered.end(),[&](const auto& left,const auto& right){return rank(left)<rank(right);});
    for(const auto& id:ordered)
    {
        auto& entry=documents_.at(id);
        if(entry.editRevision==entry.savedRevision)continue;
        EditorDocumentSaveResult result;result.id=id;
        if(HasExternalChange(id))
        {
            result.diagnosticKey="Editor.ExternalChange";
            result.detail=entry.path.string();
        }
        else if(save(entry,result.diagnosticKey,result.detail))
        {
            std::string error;
            if(MarkSaved(id,error))result.saved=true;
            else result.diagnosticKey=error;
        }
        results.push_back(std::move(result));
    }
    std::sort(results.begin(),results.end(),[](const auto& left,const auto& right){return left.id<right.id;});
    return results;
}
}
