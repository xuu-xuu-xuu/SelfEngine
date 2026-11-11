#pragma once
#include "RuntimeHandles.h"
#include "CoreLayer/TaskQueue.h"
#include "ResourceLayer/Scenes/SceneSerializer.h"
#include <map>

namespace se
{
class EventBus;
struct SceneHandle
{
    std::uint64_t value=0; // 全局不回绕代数；不同世界和已卸载记录的身份不复用。
    explicit operator bool()const noexcept{return value!=0;}
    bool operator==(const SceneHandle&)const=default;
};
enum class SceneState:std::uint32_t {Unloaded,Preparing,ReadyToActivate,Active,Unloading,Failed,Cancelled};
enum class SceneLoadMode:std::uint32_t {Replace,Additive};
struct SceneSnapshot
{
    SceneHandle handle;SceneState state=SceneState::Unloaded;bool active=false;
    std::uint64_t request=0,revision=0,scope=0; // 记录句柄可重载；消息作用域每次激活使用新的代数。
    std::string name,source,requestedSource,error;
    std::vector<EntityHandle> members;
    SceneEnvironmentSettings environment;
};
struct SceneStatistics
{
    std::uint64_t requested=0,activated=0,unloaded=0,failed=0,cancelled=0,staleResults=0,callbackFaults=0,notificationFailures=0;
    std::size_t pendingJobs=0,records=0,members=0;
};
class SceneManager
{
    struct Record;struct Command;struct AsyncState;
public:
    using Reader=std::function<bool(const std::filesystem::path&,SceneDocument&,std::string&,const CancellationToken&)>;
    using Validator=std::function<bool(const World&,std::string&)>;
    using UnloadHook=std::function<void(SceneHandle,std::span<const EntityHandle>,std::span<const StableEntityId>)>;
    using EnvironmentValidator=std::function<bool(const SceneEnvironmentSettings&,std::shared_ptr<const TODProfile>&,std::string&)>;
    SceneManager(World& world,EventBus& events);
    ~SceneManager();
    SceneManager(const SceneManager&)=delete;SceneManager& operator=(const SceneManager&)=delete;
    bool BindWorld(World& world);
    void ResetWorld(World& world);
    void Close(); // 只停止接入；World 仍唯一拥有成员。静止宿主可从终结器关闭。
    SceneHandle Create(std::string name);
    std::uint64_t RequestDocument(SceneHandle scene,SceneDocument document,SceneLoadMode mode=SceneLoadMode::Replace,SceneHandle replacing={});
    std::uint64_t RequestFile(SceneHandle scene,std::filesystem::path path,SceneLoadMode mode=SceneLoadMode::Replace,SceneHandle replacing={},Reader reader={});
    bool Cancel(SceneHandle scene);
    bool Unload(SceneHandle scene);
    bool ActivateIdle(SceneHandle scene,const SceneDocument& document,SceneLoadMode mode=SceneLoadMode::Replace,SceneHandle replacing={},const Validator& finalValidation={},std::string source={});
    bool SynchronizeIdle(); // 编辑器属性/撤销快照发布后更新成员，不推进模拟。
    bool ExchangeCreationScope(SceneHandle scene,SceneHandle& previous); // 宿主回调借用期间使用，允许退出已停用场景的清理作用域。
    bool FrameBegin();
    SceneSnapshot Inspect(SceneHandle scene)const;
    SceneHandle ActiveScene()const noexcept;
    SceneHandle SceneOf(EntityHandle entity)const;
    EntityHandle CreateEntity(SceneHandle scene,std::string name,StableEntityId stableId=0);
    void SetValidator(Validator validator){validator_=std::move(validator);}
    void SetUnloadHook(UnloadHook hook){unloadHook_=std::move(hook);}
    void SetEnvironmentValidator(EnvironmentValidator validator){environmentValidator_=std::move(validator);}
    bool ResolveActiveEnvironment(std::shared_ptr<const TODProfile>& profile);
    bool ValidateEnvironmentProfiles(const std::function<bool(const TODProfile&,std::string&)>& validate);
    SceneStatistics Statistics()const;
    const std::string& LastError()const noexcept{return error_;}
private:
    bool OwnerThread()const;
    bool Fail(const char* key);
    Record* Find(SceneHandle scene);
    const Record* Find(SceneHandle scene)const;
    std::uint64_t StartRequest(Record& scene,SceneLoadMode mode,SceneHandle replacing,std::string source);
    bool Apply(Command command,const Validator& finalValidation={});
    void Failed(Record& scene,std::string error);
    void Retire(Record& scene,std::span<const StableEntityId> invalidated)noexcept;
    void Notify(std::uint64_t type,const SceneSnapshot& scene);
    void SynchronizeMembers();
    SceneHandle OwnerOf(EntityHandle entity)const;
    bool OnCreated(EntityHandle entity);
    void InstallRules();
    World* world_;EventBus* events_;
    std::map<std::uint64_t,std::unique_ptr<Record>> records_;
    std::unordered_map<StableEntityId,SceneHandle> ownership_;
    std::unordered_map<std::uint64_t,std::uint64_t> replacementClaims_;
    std::vector<Command> commands_;
    std::shared_ptr<AsyncState> asynchronous_;
    std::unique_ptr<TaskQueue> jobs_;
    SceneHandle primary_,creationScope_;
    Validator validator_;UnloadHook unloadHook_;
    EnvironmentValidator environmentValidator_;
    SceneStatistics statistics_;
    std::thread::id thread_=std::this_thread::get_id();
    bool closed_=false,applying_=false;
    std::string error_;
};
}
