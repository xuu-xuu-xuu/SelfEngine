#pragma once
#include "EditorSession.h"

namespace se
{
enum class FileActionResult { Done,Waiting,Failed };
enum class UnsavedChoice { Save,Discard,Cancel };
enum class ExternalChoice { Reload,SaveAs,Cancel };
class SceneFileWorkflow
{
public:
    explicit SceneFileWorkflow(std::filesystem::path root={}):root_(std::move(root)){}
    void SetRoot(std::filesystem::path root){root_=std::move(root);}
    FileActionResult RequestOpen(EditorSession&,const std::filesystem::path&);
    FileActionResult RequestClose(EditorSession&,bool otherDocumentDirty=false);
    FileActionResult SaveCurrent(EditorSession&,const std::filesystem::path& untitledPath);
    FileActionResult SaveAs(EditorSession&,const std::filesystem::path&);
    FileActionResult DecideUnsaved(EditorSession&,UnsavedChoice,const std::filesystem::path& untitledPath);
    FileActionResult DecideOverwrite(EditorSession&,bool approve);
    // 保存前发现磁盘内容与最近读取/保存基线不同：重新加载、另存或取消，不静默覆盖。
    FileActionResult DecideExternal(EditorSession&,ExternalChoice);
    bool NeedsSaveDecision() const noexcept{return askSave_;}
    bool NeedsOverwriteDecision() const noexcept{return askOverwrite_;}
    bool NeedsExternalDecision() const noexcept{return askExternal_;}
    bool TakeExternalSaveAsRequest() noexcept{const bool value=externalSaveAs_;externalSaveAs_=false;return value;}
    bool Closing() const noexcept{return action_==Action::Close;}
    bool CloseApproved() const noexcept{return closeApproved_;}
    bool Pending() const noexcept{return action_!=Action::None||askOverwrite_||askExternal_;}
    const std::filesystem::path& SaveTarget() const noexcept{return saveTarget_;}
    const std::filesystem::path& ExternalTarget() const noexcept{return externalTarget_;}
    const std::string& LastError() const noexcept{return error_;}
private:
    enum class Action {None,Open,Close};
    FileActionResult Fail(const char* key){error_=key;return FileActionResult::Failed;}
    std::filesystem::path Resolve(const std::filesystem::path&) const;
    FileActionResult Store(EditorSession&,const std::filesystem::path&,bool continuation,bool overwrite);
    FileActionResult Finish(EditorSession&);
    void Cancel();
    void RecordStamp(const std::filesystem::path&);
    bool Changed(const std::filesystem::path&) const;
    std::filesystem::path root_,openTarget_,saveTarget_,externalTarget_;
    std::string error_;
    Action action_=Action::None;
    std::uint64_t stampSize_=0;
    std::filesystem::file_time_type stampTime_{};
    bool stampValid_=false,askSave_=false,askOverwrite_=false,askExternal_=false,externalSaveAs_=false,afterSave_=false,closeApproved_=false;
};
}
