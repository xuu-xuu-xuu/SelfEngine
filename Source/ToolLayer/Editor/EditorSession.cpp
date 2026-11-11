#include "EditorSession.h"
#include "FunctionLayer/Runtime/ComponentMetadata.h"
#include "CoreLayer/Math/TransformDecomposition.h"
#include "ResourceLayer/Scenes/SceneSerializer.h"
#include "SceneDefaults.h"
#include "ResourceLayer/Assets/AssetIO.h"
#include <algorithm>
#include <cmath>
#include <unordered_map>

namespace se
{
std::vector<EditorHierarchyItem> BuildEditorHierarchy(const World& world)
{
    std::unordered_map<StableEntityId,std::vector<StableEntityId>> children;
    for(const auto entity:world.Entities())
    {
        const auto* record=world.Get(entity);
        const auto parent=record->parent?world.Get(record->parent)->stableId:0;
        children[parent].push_back(record->stableId);
    }
    for(auto& [id,list]:children){(void)id;std::sort(list.begin(),list.end());}
    std::vector<EditorHierarchyItem> pending,result;result.reserve(world.Size());
    const auto push=[&](StableEntityId parent,std::size_t depth){
        if(const auto found=children.find(parent);found!=children.end())
            for(auto it=found->second.rbegin();it!=found->second.rend();++it)pending.push_back({*it,parent,depth,false});
    };
    push(0,0);
    while(!pending.empty())
    {
        auto item=pending.back();pending.pop_back();item.hasChildren=children.contains(item.id);
        result.push_back(item);push(item.id,item.depth+1);
    }
    return result;
}
bool EditorSession::Fail(const char* key) const { error_ = key; return false; }
bool EditorSession::Select(StableEntityId id)
{
    if (id && !edit_.FindByStableId(id)) return Fail("Editor.InvalidSelection");
    selected_ = id; return true;
}
bool EditorSession::Capture(Snapshot& snapshot) const
{
    snapshot.selected = selected_;
    return SerializeScene(edit_, snapshot.scene, error_);
}
bool EditorSession::Restore(const Snapshot& snapshot)
{
    World restored;if(!DeserializeScene(snapshot.scene,restored,error_))return false;
    // 撤销恢复逻辑对象；已发出的身份不随历史回滚，否则后来创建可能误接旧引用。
    for(const auto id:edit_.KnownStableIds())if(!restored.ReserveStableId(id))return Fail("Editor.OperationFailed");
    edit_=std::move(restored);
    if(!editScenes_.SynchronizeIdle()){error_=editScenes_.LastError();return false;}
    selected_ = edit_.FindByStableId(snapshot.selected) ? snapshot.selected : 0;
    return true;
}
void EditorSession::Push(Command command)
{
    if (command.before.scene == command.after.scene && command.before.selected == command.after.selected) return;
    if (undo_.size() == 100) undo_.erase(undo_.begin());
    undo_.push_back(std::move(command)); redo_.clear();
}
bool EditorSession::Apply(std::string label, const std::function<bool(World&)>& operation)
{
    if (!operation || play_) return Fail("Editor.EditWhilePlaying");
    Command command; command.label = std::move(label);
    if (!Capture(command.before)) return false;
    World candidate(edit_);
    try
    {
        if (!operation(candidate) || !candidate.FlushCommands())
        { error_ = candidate.LastError().empty() ? "Editor.OperationFailed" : candidate.LastError(); return false; }
        if (!SerializeScene(candidate, command.after.scene, error_)) return false;
    }
    catch (...) { return Fail("Editor.OperationFailed"); }
    edit_ = std::move(candidate);
    if(!editScenes_.SynchronizeIdle()){error_=editScenes_.LastError();return false;}
    if (!edit_.FindByStableId(selected_)) selected_ = 0;
    command.after.selected = selected_;
    if (!transaction_) Push(std::move(command));
    error_.clear(); return true;
}
StableEntityId EditorSession::Create(std::string name, StableEntityId parent)
{
    StableEntityId created = 0;
    if (!Apply("Editor.Create", [&](World& world) {
        const auto p = parent ? world.FindByStableId(parent) : EntityHandle{};
        if (parent && !p) return false;
        const auto entity = world.Create(name);
        if (!entity || (parent && !world.SetParent(entity,p))) return false;
        created = world.Get(entity)->stableId; return true;
    })) return 0;
    selected_ = created;
    if (!transaction_ && !undo_.empty()) undo_.back().after.selected = created;
    return created;
}
bool EditorSession::Delete(StableEntityId id){
    const auto handle=edit_.FindByStableId(id);
    if(!handle)return Fail("Editor.InvalidSelection");
    bool savedBefore=false;
    if(!saved_.empty())
    {
        World base;std::string ignored;
        if(DeserializeScene(saved_,base,ignored))savedBefore=base.FindByStableId(id)!=EntityHandle{};
    }
    const bool result=Apply("Editor.Delete",[=](World& world){return world.Destroy(world.FindByStableId(id));});
    // 新建后删除、且从未保存的实体不应留下持久 retiredIds，否则一次无残留的编辑会永久变脏。
    if(result&&!savedBefore&&!edit_.FindByStableId(id)&&edit_.ForgetStableId(id))
    {
        if(transaction_)SerializeScene(edit_,transaction_->after.scene,error_);
        else if(!undo_.empty())SerializeScene(edit_,undo_.back().after.scene,error_);
    }
    return result;
}
bool EditorSession::Rename(StableEntityId id, std::string name)
{ return Apply("Editor.Rename", [&](World& world) { return world.Rename(world.FindByStableId(id),name); }); }
bool EditorSession::SetTransform(StableEntityId id, const Transform& transform)
{ return Apply("Editor.Transform", [&](World& world) { return world.SetTransform(world.FindByStableId(id),transform); }); }
bool EditorSession::SetParent(StableEntityId id,StableEntityId parent,ReparentMode mode)
{
    if(mode!=ReparentMode::KeepLocal&&mode!=ReparentMode::KeepWorld)return Fail("Editor.OperationFailed");
    bool unrepresentable=false;
    const bool result=Apply("Editor.Reparent", [&](World& world) {
        const auto entity=world.FindByStableId(id);if(!entity)return false;
        auto p = parent ? world.FindByStableId(parent) : EntityHandle{};
        if(parent&&!p)return false;
        const auto matrix=world.GetWorldMatrix(entity);
        if(!world.SetParent(entity,p))return false;
        if(mode==ReparentMode::KeepLocal)return true;
        const auto inverse=p?Inverse(world.GetWorldMatrix(p)):std::optional<Mat4>(Mat4{});Transform local;
        if(!inverse||!TryDecomposeTrs(*inverse*matrix,local)){unrepresentable=true;return false;}
        return world.SetTransform(entity,local);
    });
    if(!result&&unrepresentable)error_="Editor.TransformNotRepresentable";return result;
}
bool EditorSession::SetMesh(StableEntityId id, const MeshComponent& component)
{ return Apply("Editor.Mesh", [&](World& world) { return world.SetMesh(world.FindByStableId(id),component); }); }
bool EditorSession::SetProperty(StableEntityId id,std::string_view key,const PropertyValue& value)
{
    const auto* field=FindProperty(key);if(!field)return Fail("Metadata.UnknownProperty");
    std::string propertyError;
    const bool result=Apply(std::string(field->labelKey),[&](World& world){return WriteProperty(world,world.FindByStableId(id),key,value,propertyError);});
    if(!result&&!propertyError.empty())error_=propertyError;return result;
}
StableEntityId EditorSession::Duplicate(StableEntityId id)
{
    StableEntityId duplicate = 0;
    const bool result = Apply("Editor.Duplicate", [&](World& world) {
        auto root = world.FindByStableId(id); if (!root) return false;
        const auto all = world.Entities();
        std::vector<EntityHandle> subtree;
        for (auto entity : all)
            for (auto cursor = entity; cursor; cursor = world.Get(cursor)->parent)
                if (cursor == root) { subtree.push_back(entity); break; }
        std::unordered_map<std::uint64_t, EntityHandle> replacements;
        for (auto source : subtree)
        {
            const auto record = *world.Get(source);
            const auto target = world.Create(record.name + " Copy");
            if (!target || !world.SetEnabled(target,record.enabled)) return false;
            replacements[source.Pack()] = target;
            if (source == root) duplicate = world.Get(target)->stableId;
        }
        for (auto source : subtree)
        {
            auto parent = world.Get(source)->parent;
            if (auto it = replacements.find(parent.Pack()); it != replacements.end()) parent = it->second;
            if (!world.SetParent(replacements.at(source.Pack()),parent)) return false;
        }
        std::unordered_map<StableEntityId,StableEntityId> references;
        for(auto source:subtree)references.emplace(world.Get(source)->stableId,world.Get(replacements.at(source.Pack()))->stableId);
        for(auto source:subtree)
        {std::string error;if(!CloneComponentProperties(world,source,world,replacements.at(source.Pack()),error,references))return false;}
        return true;
    });
    if (!result) return 0;
    selected_ = duplicate;
    if (!transaction_ && !undo_.empty()) undo_.back().after.selected = duplicate;
    return duplicate;
}
bool EditorSession::BeginEdit(std::string label)
{
    if (transaction_ || play_) return Fail("Editor.TransactionActive");
    auto command = std::make_unique<Command>(); command->label = std::move(label);
    if (!Capture(command->before)) return false;
    transaction_ = std::move(command); return true;
}
bool EditorSession::CommitEdit()
{
    if (!transaction_) return Fail("Editor.NoTransaction");
    if (!Capture(transaction_->after)) return false;
    auto command = std::move(*transaction_); transaction_.reset(); Push(std::move(command)); return true;
}
bool EditorSession::CancelEdit()
{
    if (!transaction_) return Fail("Editor.NoTransaction");
    if (!Restore(transaction_->before)) return false;
    transaction_.reset(); return true;
}
bool EditorSession::Undo()
{
    if (transaction_ || play_ || undo_.empty()) return Fail("Editor.CannotUndo");
    if (!Restore(undo_.back().before)) return false;
    redo_.push_back(std::move(undo_.back())); undo_.pop_back(); return true;
}
bool EditorSession::Redo()
{
    if (transaction_ || play_ || redo_.empty()) return Fail("Editor.CannotRedo");
    if (!Restore(redo_.back().after)) return false;
    undo_.push_back(std::move(redo_.back())); redo_.pop_back(); return true;
}
bool EditorSession::Dirty() const
{
    if (saved_.empty()) return edit_.Size() != 0;
    std::string current; return !SerializeScene(edit_,current,error_) || current != saved_;
}
bool EditorSession::RestoreUnsavedDocument(std::string_view content,const std::filesystem::path& path,const std::string& baseDigest)
{
    if(play_||transaction_)return Fail("Editor.TransactionActive");
    World restored;if(!DeserializeScene(content,restored,error_))return false;
    std::string saved,digest;World base;
    if(!path.empty()&&std::filesystem::is_regular_file(path))
    {
        std::vector<std::uint8_t> bytes;if(!asset_io::Read(path,bytes))return Fail("Scene.ReadFailed");
        digest=asset_io::Hash(bytes);if(digest!=baseDigest)return Fail("Editor.RecoverySourceChanged");
        if(!DeserializeScene({reinterpret_cast<const char*>(bytes.data()),bytes.size()},base,error_)||!SerializeScene(base,saved,error_))return false;
    }
    else if(!baseDigest.empty())return Fail("Editor.RecoverySourceChanged");
    edit_=std::move(restored);if(!editScenes_.SynchronizeIdle()){error_=editScenes_.LastError();return false;}
    path_=path;saved_=std::move(saved);sourceDigest_=std::move(digest);selected_=0;undo_.clear();redo_.clear();error_.clear();return true;
}
bool EditorSession::NewDefaultScene(std::string& errorKey)
{
    errorKey.clear();
    if(play_||transaction_){errorKey="Editor.TransactionActive";return false;}
    World world;if(!CreateDefaultScene(world,errorKey))return false;
    edit_=std::move(world);if(!editScenes_.SynchronizeIdle()){errorKey=editScenes_.LastError();return false;}
    path_.clear();saved_.clear();sourceDigest_.clear();selected_=0;undo_.clear();redo_.clear();error_.clear();
    return true;
}
bool EditorSession::Open(const std::filesystem::path& path, bool discardUnsaved)
{    if (transaction_ || play_) return Fail("Editor.TransactionActive");
    if (!discardUnsaved && Dirty()) return Fail("Editor.UnsavedChanges");
    std::vector<std::uint8_t> bytes;if(!asset_io::Read(path,bytes))return Fail("Scene.ReadFailed");
    SceneDocument document;if(!ParseSceneDocument({reinterpret_cast<const char*>(bytes.data()),bytes.size()},document,error_))return false;
    auto scene=editScenes_.ActiveScene();if(!scene)scene=editScenes_.Create("EditScene");
    if(!scene){error_=editScenes_.LastError();return false;}
    auto nextPath=path;const auto source=path.generic_u8string();std::string saved;
    if(!editScenes_.ActivateIdle(scene,document,SceneLoadMode::Replace,{},[&](const World& candidate,std::string& error){return SerializeScene(candidate,saved,error);},std::string(source.begin(),source.end())))
    {error_=editScenes_.LastError();return false;}
    saved_ = std::move(saved); path_ = std::move(nextPath);
    sourceDigest_=asset_io::Hash(bytes);
    selected_ = 0; undo_.clear(); redo_.clear(); return true;
}
bool EditorSession::Save(const std::filesystem::path& path,bool replaceExisting)
{
    if (transaction_) return Fail("Editor.TransactionActive");
    if (!SaveScene(path,edit_,error_,replaceExisting)) return false;
    path_ = path;if(!SerializeScene(edit_,saved_,error_))return false;
    sourceDigest_=asset_io::Hash({reinterpret_cast<const std::uint8_t*>(saved_.data()),saved_.size()});return true;
}
bool EditorSession::ValidatePlay(std::string& reasonKey) const
{
    reasonKey.clear();
    if(play_||transaction_||startingPlay_||stoppingPlay_){reasonKey="Editor.TransactionActive";return false;}
    bool hasCamera=false;
    for(const auto entity:edit_.Entities())
    {
        if(!edit_.GetCamera(entity))continue;
        bool enabled=true;
        for(auto current=entity;current;)
        {
            const auto* record=edit_.Get(current);
            if(!record||!record->enabled){enabled=false;break;}
            current=record->parent;
        }
        if(!enabled)continue;
        const auto matrix=edit_.GetWorldMatrix(entity);
        bool finite=true;
        for(const auto value:matrix.values)if(!std::isfinite(value)){finite=false;break;}
        if(!finite){reasonKey="Editor.PlayInvalidCamera";return false;}
        hasCamera=true;
    }
    if(!hasCamera){reasonKey="Editor.PlayNoCamera";return false;}
    return true;
}
bool EditorSession::StartPlay()
{
    if(play_||transaction_||startingPlay_||stoppingPlay_)return Fail("Editor.TransactionActive");
    bool callbackEntered=false;
    try
    {
        auto candidate=std::make_unique<Environment>();candidate->SetTODResolver(todResolver_);candidate->GetWorld()=edit_;
        if(!candidate->Scenes().SynchronizeIdle()||!candidate->SynchronizeTOD())return Fail("Editor.PlayStartFailed");
        play_=std::move(candidate);startingPlay_=true;stopPlayRequested_=false;playStepRequested_=false;callbackEntered=true;
        const bool started=!playStarted_||playStarted_(play_->GetWorld());startingPlay_=false;
        if(!started||stopPlayRequested_){StopPlay();return Fail(started?"Editor.PlayStartCancelled":"Editor.PlayStartFailed");}
    }
    catch(...){startingPlay_=false;if(callbackEntered)StopPlay();else play_.reset();return Fail("Editor.PlayStartFailed");}
    error_.clear();return true;
}
bool EditorSession::SetPlayPaused(bool paused)
{
    if(!play_)return Fail("Editor.PlaybackInvalidState");
    play_->SetFramePaused(paused);
    error_.clear();return true;
}
bool EditorSession::StepPlayFrame()
{
    if(!play_||!play_->FramePaused())return Fail("Editor.PlaybackInvalidState");
    if(!play_->StepFrame()){error_=play_->LastError();return false;}
    error_.clear();return true;
}
double EditorSession::PlayFixedStep() const noexcept
{
    return play_?play_->Clock().Step():1.0/60.0;
}
bool EditorSession::PlayPaused() const noexcept
{
    return play_&&play_->FramePaused();
}
bool EditorSession::ConsumePlayStep() noexcept
{
    if(!playStepRequested_)return false;
    playStepRequested_=false;
    return play_&&play_->FramePaused();
}
bool EditorSession::SetTODResolver(Environment::TODResolver resolver)
{if(play_)return Fail("Editor.EditWhilePlaying");todResolver_=std::move(resolver);editEnvironment_.SetTODResolver(todResolver_);return true;}
void EditorSession::CompletePlayFrame()
{if(stopPlayRequested_&&play_&&!play_->IsRunningFrame())StopPlay();}
bool EditorSession::SetPlayCallbacks(std::function<bool(World&)> start,std::function<void(World&)> stopping)
{
    if(play_)return Fail("Editor.EditWhilePlaying");
    playStarted_=std::move(start);playStopping_=std::move(stopping);return true;
}
void EditorSession::StopPlay() noexcept
{
    if(!play_||stoppingPlay_)return;
    // 启动回调仍借用此 World，返回后才能执行停止回调与释放。
    if(startingPlay_||play_->IsRunningFrame()){stopPlayRequested_=true;return;}
    stopPlayRequested_=false;
    stoppingPlay_=true;
    play_->BeginShutdown();
    // 回调借用的 PlayWorld 必须保持存活直到 OnDestroy/订阅清理全部结束。
    try { if(playStopping_)playStopping_(play_->GetWorld()); } catch (...) { error_="Editor.PlayStopFailed"; }
    play_.reset();
    stoppingPlay_=false;
}
}
