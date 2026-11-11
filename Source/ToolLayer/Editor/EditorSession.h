#pragma once
#include "FunctionLayer/Runtime/Environment.h"
#include "FunctionLayer/Runtime/SceneManager.h"
#include "FunctionLayer/Runtime/EventBus.h"
#include "CoreLayer/PropertyMetadata.h"
#include <filesystem>
#include <functional>
#include <memory>
#include <string>
#include <vector>

namespace se
{
struct EditorHierarchyItem { StableEntityId id=0,parent=0;std::size_t depth=0;bool hasChildren=false; };
enum class ReparentMode { KeepLocal,KeepWorld };
std::vector<EditorHierarchyItem> BuildEditorHierarchy(const World& world);
// 编辑器使用稳定 ID 跨越撤销、加载和 Play 副本；运行时 handle 只在所属 World 内有效。
class EditorSession
{
public:
    EditorSession(){editEnvironment_.SetPaused(true);}
    ~EditorSession() { StopPlay(); }
    const World& EditWorld() const noexcept { return edit_; }
    const SceneManager& EditScenes()const noexcept{return editScenes_;}
    const World* PlayWorld() const noexcept { return play_?&play_->GetWorld():nullptr; }
    World* PlayWorld() noexcept { return play_?&play_->GetWorld():nullptr; }
    Environment& EditEnvironment()noexcept{return editEnvironment_;}
    const Environment& EditEnvironment()const noexcept{return editEnvironment_;}
    Environment* PlayEnvironment()noexcept{return play_.get();}
    const Environment* PlayEnvironment()const noexcept{return play_.get();}
    bool SetTODResolver(Environment::TODResolver resolver);
    void CompletePlayFrame();
    StableEntityId Selected() const noexcept { return selected_; }
    bool Select(StableEntityId id);
    bool Apply(std::string label, const std::function<bool(World&)>& operation);
    StableEntityId Create(std::string name, StableEntityId parent = 0);
    bool Delete(StableEntityId id);
    StableEntityId Duplicate(StableEntityId id);
    bool Rename(StableEntityId id, std::string name);
    bool SetTransform(StableEntityId id, const Transform& transform);
    bool SetParent(StableEntityId id,StableEntityId parent,ReparentMode mode=ReparentMode::KeepLocal);
    bool SetMesh(StableEntityId id, const MeshComponent& component);
    bool SetProperty(StableEntityId id,std::string_view key,const PropertyValue& value);
    bool BeginEdit(std::string label);
    bool CommitEdit();
    bool CancelEdit();
    bool Undo();
    bool Redo();
    bool Open(const std::filesystem::path& path, bool discardUnsaved = false);
    // 缺失/空入口场景的内存默认场景；不写文件，saved_ 保持空以标记未保存。
    bool NewDefaultScene(std::string& errorKey);
    bool RestoreUnsavedDocument(std::string_view content,const std::filesystem::path& path,const std::string& baseDigest={});
    bool Save(const std::filesystem::path& path,bool replaceExisting=true);
    bool Dirty() const;
    bool StartPlay();
    // Play 前检查编辑世界；失败返回稳定文本键，不创建运行上下文。
    bool ValidatePlay(std::string& reasonKey) const;
    bool SetPlayPaused(bool paused);
    bool StepPlayFrame();
    double PlayFixedStep() const noexcept;
    bool PlayPaused() const noexcept;
    // UI 线程登记单步；实际执行在运行环境推进点，避免同一显示帧重复执行阶段。
    void RequestPlayStep() noexcept{playStepRequested_=true;}
    bool ConsumePlayStep() noexcept;
    bool SetPlayCallbacks(std::function<bool(World&)> start, std::function<void(World&)> stopping);
    void StopPlay() noexcept;
    bool IsPlaying() const noexcept { return play_ != nullptr; }
    const std::string& LastError() const noexcept { return error_; }
    const std::filesystem::path& Path() const noexcept { return path_; }
    const std::string& SourceDigest() const noexcept{return sourceDigest_;}
    std::size_t UndoCount() const noexcept { return undo_.size(); }
    std::size_t RedoCount() const noexcept { return redo_.size(); }
private:
    struct Snapshot { std::string scene; StableEntityId selected = 0; };
    struct Command { std::string label; Snapshot before, after; };
    bool Capture(Snapshot& snapshot) const;
    bool Restore(const Snapshot& snapshot);
    void Push(Command command);
    bool Fail(const char* key) const;
    Environment editEnvironment_;
    World& edit_=editEnvironment_.GetWorld();
    SceneManager& editScenes_=editEnvironment_.Scenes();
    std::unique_ptr<Environment> play_;
    Environment::TODResolver todResolver_;
    std::function<bool(World&)> playStarted_;
    std::function<void(World&)> playStopping_;
    bool startingPlay_=false,stoppingPlay_=false,stopPlayRequested_=false,playStepRequested_=false;
    StableEntityId selected_ = 0;
    std::filesystem::path path_;
    std::string saved_;
    std::string sourceDigest_;
    std::vector<Command> undo_, redo_;
    std::unique_ptr<Command> transaction_;
    mutable std::string error_;
};
}
