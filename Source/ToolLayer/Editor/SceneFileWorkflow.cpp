#include "SceneFileWorkflow.h"
#include "ResourceLayer/Assets/AssetIO.h"

namespace se
{
namespace
{
bool Same(const std::filesystem::path& a,const std::filesystem::path& b)
{return !a.empty()&&!b.empty()&&asset_io::Within(a,b)&&asset_io::Within(b,a);}
}
std::filesystem::path SceneFileWorkflow::Resolve(const std::filesystem::path& path) const
{
    if(path.empty())return {};
    std::error_code error;const auto resolved=std::filesystem::weakly_canonical(path.is_absolute()?path:(root_.empty()?std::filesystem::current_path():root_)/path,error);
    return error?std::filesystem::path{}:resolved;
}
void SceneFileWorkflow::Cancel()
{action_=Action::None;askSave_=askOverwrite_=askExternal_=externalSaveAs_=afterSave_=false;openTarget_.clear();saveTarget_.clear();externalTarget_.clear();error_.clear();}
void SceneFileWorkflow::RecordStamp(const std::filesystem::path& path)
{
    std::error_code error;
    const auto size=std::filesystem::file_size(path,error);
    const auto time=std::filesystem::last_write_time(path,error);
    stampValid_=!error;stampSize_=stampValid_?size:0;stampTime_=stampValid_?time:std::filesystem::file_time_type{};
}
bool SceneFileWorkflow::Changed(const std::filesystem::path& path) const
{
    std::error_code error;
    const auto size=std::filesystem::file_size(path,error);
    if(error)return true;
    const auto time=std::filesystem::last_write_time(path,error);
    if(error)return true;
    return size!=stampSize_||time!=stampTime_;
}
FileActionResult SceneFileWorkflow::Finish(EditorSession& editor)
{
    if(action_==Action::Open)
    {
        if(!editor.Open(openTarget_,true)){const auto error=editor.LastError();Cancel();error_=error;return FileActionResult::Failed;}
        RecordStamp(editor.Path());
        Cancel();return FileActionResult::Done;
    }
    if(action_==Action::Close){editor.StopPlay();Cancel();closeApproved_=true;return FileActionResult::Done;}
    Cancel();return FileActionResult::Done;
}
FileActionResult SceneFileWorkflow::RequestOpen(EditorSession& editor,const std::filesystem::path& path)
{
    if(editor.IsPlaying())return Fail("Editor.EditWhilePlaying");
    const auto resolved=Resolve(path);if(resolved.empty())return Fail("Scene.InvalidPath");
    Cancel();closeApproved_=false;action_=Action::Open;openTarget_=resolved;
    if(editor.Dirty()){askSave_=true;return FileActionResult::Waiting;}return Finish(editor);
}
FileActionResult SceneFileWorkflow::RequestClose(EditorSession& editor,bool otherDirty)
{
    Cancel();closeApproved_=false;action_=Action::Close;
    if(editor.Dirty()||otherDirty){askSave_=true;return FileActionResult::Waiting;}return Finish(editor);
}
FileActionResult SceneFileWorkflow::Store(EditorSession& editor,const std::filesystem::path& requested,bool continuation,bool overwrite)
{
    if(editor.IsPlaying()&&!(continuation&&action_==Action::Close))return Fail("Editor.EditWhilePlaying");
    const auto path=Resolve(requested);if(path.empty()||path.filename().empty())return Fail("Scene.InvalidPath");
    std::error_code error;const bool exists=std::filesystem::exists(path,error);if(error)return Fail("Scene.InvalidPath");
    if(!overwrite&&Same(path,editor.Path())&&!editor.SourceDigest().empty())
    {
        std::vector<std::uint8_t> bytes;
        if(!asset_io::Read(path,bytes)||asset_io::Hash(bytes)!=editor.SourceDigest())
        {askExternal_=true;externalTarget_=path;return FileActionResult::Waiting;}
    }
    if(!overwrite&&Same(path,editor.Path())&&stampValid_&&Changed(path))
    {askExternal_=true;externalTarget_=path;return FileActionResult::Waiting;}
    if(!overwrite&&!Same(path,editor.Path())&&exists)
    {saveTarget_=path;askOverwrite_=true;afterSave_=continuation;return FileActionResult::Waiting;}
    if(!path.parent_path().empty())std::filesystem::create_directories(path.parent_path(),error);
    if(error)return Fail("Scene.WriteFailed");
    const bool replace=overwrite||Same(path,editor.Path());
    if(!editor.Save(path,replace)){error_=editor.LastError();return FileActionResult::Failed;}
    RecordStamp(path);
    askOverwrite_=false;saveTarget_.clear();error_.clear();return continuation?Finish(editor):FileActionResult::Done;
}
FileActionResult SceneFileWorkflow::SaveCurrent(EditorSession& editor,const std::filesystem::path& untitled)
{return Store(editor,editor.Path().empty()?untitled:editor.Path(),false,false);}
FileActionResult SceneFileWorkflow::SaveAs(EditorSession& editor,const std::filesystem::path& path)
{return Store(editor,path,false,false);}
FileActionResult SceneFileWorkflow::DecideUnsaved(EditorSession& editor,UnsavedChoice choice,const std::filesystem::path& untitled)
{
    if(!askSave_)return Fail("Editor.OperationFailed");
    if(choice==UnsavedChoice::Cancel){Cancel();return FileActionResult::Done;}
    if(choice==UnsavedChoice::Discard)return Finish(editor);
    if(choice!=UnsavedChoice::Save)return Fail("Editor.OperationFailed");
    if(!editor.Dirty())return Finish(editor);
    const auto target=editor.Path().empty()?Resolve(untitled):editor.Path();
    if(editor.Path().empty()&&action_==Action::Open&&Same(target,openTarget_))return Fail("Editor.SavePathConflictsOpen");
    return Store(editor,target,true,false);
}
FileActionResult SceneFileWorkflow::DecideOverwrite(EditorSession& editor,bool approve)
{
    if(!askOverwrite_)return Fail("Editor.OperationFailed");
    if(!approve){askOverwrite_=false;saveTarget_.clear();error_.clear();return FileActionResult::Done;}
    const auto path=saveTarget_;const bool continuation=afterSave_;return Store(editor,path,continuation,true);
}
FileActionResult SceneFileWorkflow::DecideExternal(EditorSession& editor,ExternalChoice choice)
{
    if(!askExternal_)return Fail("Editor.OperationFailed");
    if(choice==ExternalChoice::Cancel)
    {
        askExternal_=false;externalTarget_.clear();error_.clear();return FileActionResult::Done;
    }
    if(choice==ExternalChoice::SaveAs)
    {
        askExternal_=false;externalTarget_.clear();externalSaveAs_=true;return FileActionResult::Waiting;
    }
    if(choice==ExternalChoice::Reload)
    {
        if(!editor.Open(externalTarget_,true)){error_=editor.LastError();return FileActionResult::Failed;}
        RecordStamp(editor.Path());askExternal_=false;externalTarget_.clear();error_.clear();return FileActionResult::Done;
    }
    return Fail("Editor.OperationFailed");
}
}
