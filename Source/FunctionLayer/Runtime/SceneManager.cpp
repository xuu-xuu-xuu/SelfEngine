#include "SceneManager.h"
#include "EventBus.h"
#include "ComponentMetadata.h"
#include <algorithm>
#include <atomic>
#include <mutex>
#include <stdexcept>

namespace se
{
namespace
{
std::atomic<std::uint64_t> nextScene{1},nextSceneRequest{1};
std::uint64_t Identity(std::atomic<std::uint64_t>& counter)
{auto value=counter.load();while(value){const auto next=value==UINT64_MAX?0:value+1;if(counter.compare_exchange_weak(value,next))return value;}return 0;}
void Write64(std::vector<std::uint8_t>& bytes,std::size_t offset,std::uint64_t value){for(unsigned i=0;i<8;++i)bytes[offset+i]=static_cast<std::uint8_t>(value>>(i*8));}
void Write32(std::vector<std::uint8_t>& bytes,std::size_t offset,std::uint32_t value){for(unsigned i=0;i<4;++i)bytes[offset+i]=static_cast<std::uint8_t>(value>>(i*8));}
}
struct SceneManager::Record:SceneSnapshot
{
    std::vector<StableEntityId> identities;
    CancellationToken token;
    SceneHandle replacing;std::uint64_t replacingRevision=0;
    SceneLoadMode mode=SceneLoadMode::Replace;
    bool makePrimary=false,teardownDone=false;
    std::shared_ptr<const TODProfile> profile;
};
struct SceneManager::Command {SceneHandle scene;std::uint64_t request=0;SceneDocument document;std::string error;bool unload=false;};
struct SceneManager::AsyncState
{
    std::atomic<bool> open{true};std::mutex mutex;std::vector<Command> ready;
    void Complete(Command command){if(!open.load())return;std::lock_guard lock(mutex);if(open.load())ready.push_back(std::move(command));}
};
SceneManager::SceneManager(World& world,EventBus& events):world_(&world),events_(&events),asynchronous_(std::make_shared<AsyncState>()){InstallRules();SynchronizeMembers();}
SceneManager::~SceneManager(){Close();}
bool SceneManager::OwnerThread()const{return std::this_thread::get_id()==thread_;}
bool SceneManager::Fail(const char* key){error_=key;return false;}
SceneManager::Record* SceneManager::Find(SceneHandle scene){const auto it=records_.find(scene.value);return it==records_.end()?nullptr:it->second.get();}
const SceneManager::Record* SceneManager::Find(SceneHandle scene)const{const auto it=records_.find(scene.value);return it==records_.end()?nullptr:it->second.get();}
void SceneManager::InstallRules()
{
    if(world_->sceneCreated_||world_->sceneParent_)throw std::logic_error("Scene.ContextMismatch");
    for(const auto& descriptor:std::initializer_list<EventTypeDescriptor>{{16,"engine.scene_activated",1,4096},{17,"engine.scene_unloaded",1,4096},{18,"engine.scene_load_failed",1,4096}})
        if(!events_->RegisterType(descriptor))throw std::logic_error(events_->LastError());
    world_->sceneCreated_=[this](EntityHandle entity){return OnCreated(entity);};
    world_->sceneParent_=[this](EntityHandle entity,EntityHandle parent){return !parent||OwnerOf(entity)==OwnerOf(parent);};
    events_->SetSceneResolver([this](std::uint64_t scope,EntityHandle owner){const auto* record=Find(OwnerOf(owner));return record&&record->active&&record->scope==scope;});
}
void SceneManager::Close()
{
    if(closed_)return;
    closed_=true;if(asynchronous_)asynchronous_->open=false;
    for(auto& [_,record]:records_)record->token.Cancel();jobs_.reset();commands_.clear();
    if(world_){world_->sceneCreated_={};world_->sceneParent_={};}
    if(OwnerThread())events_->SetSceneResolver({});
}
void SceneManager::ResetWorld(World& world)
{
    if(!OwnerThread())throw std::logic_error("Scene.WrongThread");
    if(applying_){Fail("Scene.CommitInProgress");return;}
    Close();records_.clear();ownership_.clear();replacementClaims_.clear();primary_={};creationScope_={};statistics_={};
    world_=&world;asynchronous_=std::make_shared<AsyncState>();closed_=false;InstallRules();SynchronizeMembers();error_.clear();
}
bool SceneManager::BindWorld(World& world)
{
    if(!OwnerThread())return Fail("Scene.WrongThread");if(world_==&world)return !closed_;
    if(applying_||world_->IsInFrame()||world.IsInFrame())return Fail("Scene.ContextMismatch");ResetWorld(world);return !closed_;
}
SceneHandle SceneManager::Create(std::string name)
{
    if(!OwnerThread()){Fail("Scene.WrongThread");return {};}
    if(closed_||applying_){Fail(closed_?"Scene.Closed":"Scene.CommitInProgress");return {};}
    if(creationScope_&&!Find(creationScope_)->active){Fail("Scene.Inactive");return {};}
    if(name.empty()||name.size()>256||name.find('\0')!=std::string::npos){Fail("Scene.InvalidName");return {};}
    if(records_.size()>=1024){Fail("Scene.SceneBudget");return {};}
    const auto id=Identity(nextScene);if(!id){Fail("Scene.IdentityExhausted");return {};}
    auto record=std::make_unique<Record>();record->handle={id};record->name=std::move(name);records_.emplace(id,std::move(record));error_.clear();return {id};
}
SceneHandle SceneManager::OwnerOf(EntityHandle entity)const
{
    if(!world_->IsKnown(entity))return {};const auto it=ownership_.find(world_->slots_[entity.index].record.stableId);
    return it==ownership_.end()?SceneHandle{}:it->second;
}
SceneHandle SceneManager::SceneOf(EntityHandle entity)const
{if(!OwnerThread())return {};const auto scene=OwnerOf(entity);const auto* record=Find(scene);return record&&record->active?scene:SceneHandle{};}
bool SceneManager::OnCreated(EntityHandle entity)
{
    if(closed_||applying_)return false;
    auto scene=creationScope_;if(!scene)scene=OwnerOf(world_->callbackOwner_);if(!scene)scene=primary_;
    if(!scene)
    {
        scene=Create("Startup");auto* initial=Find(scene);if(!initial)return false;
        initial->scope=Identity(nextScene);if(!initial->scope)return false;initial->active=true;initial->state=SceneState::Active;initial->revision=1;primary_=scene;Notify(16,*initial);
    }
    auto* record=Find(scene);if(!record||!record->active||record->state==SceneState::Unloading)return false;
    if(record->identities.size()>=100000)return false;
    const auto id=world_->slots_[entity.index].record.stableId;
    record->members.reserve(record->members.size()+1);record->identities.reserve(record->identities.size()+1);
    ownership_.emplace(id,scene);record->members.push_back(entity);record->identities.push_back(id);return true;
}
EntityHandle SceneManager::CreateEntity(SceneHandle scene,std::string name,StableEntityId stableId)
{
    if(!OwnerThread()||closed_||applying_){Fail("Scene.CommitNotSafe");return {};}
    if(creationScope_&&!Find(creationScope_)->active){Fail("Scene.Inactive");return {};}
    const auto* record=Find(scene);if(!record||!record->active){Fail("Scene.Inactive");return {};}
    const auto previous=creationScope_;creationScope_=scene;
    struct Restore{SceneHandle& value;SceneHandle previous;~Restore(){value=previous;}} restore{creationScope_,previous};
    return world_->Create(std::move(name),stableId);
}
void SceneManager::SynchronizeMembers()
{
    bool hasObjects=false;for(std::size_t i=0;i<world_->slots_.size();++i)if(world_->IsKnown({static_cast<std::uint32_t>(i),world_->slots_[i].generation})){hasObjects=true;break;}
    if(!primary_&&(hasObjects||!world_->EnvironmentSettings().todProfile.empty()))
    {
        primary_=Create("Startup");auto* initial=Find(primary_);if(!initial)throw std::runtime_error("Scene.SceneBudget");
        initial->scope=Identity(nextScene);if(!initial->scope)throw std::runtime_error("Scene.IdentityExhausted");initial->active=true;initial->state=SceneState::Active;initial->revision=1;Notify(16,*initial);
        initial->environment=world_->EnvironmentSettings();
    }
    for(auto& [_,record]:records_)record->members.clear();
    for(std::size_t i=0;i<world_->slots_.size();++i)
    {
        const EntityHandle entity{static_cast<std::uint32_t>(i),world_->slots_[i].generation};if(!world_->IsKnown(entity))continue;
        const auto id=world_->slots_[i].record.stableId;auto owner=OwnerOf(entity);
        if(!owner)
        {
            owner=OwnerOf(world_->slots_[i].record.parent);if(!owner)owner=primary_;if(!owner)continue;
            ownership_.emplace(id,owner);Find(owner)->identities.push_back(id);
        }
        if(auto* record=Find(owner))record->members.push_back(entity);
    }
    if(auto* primary=Find(primary_);primary&&primary->active)
    {
        if(primary->environment!=world_->EnvironmentSettings()){primary->environment=world_->EnvironmentSettings();primary->profile.reset();++primary->revision;}
        for(const auto id:world_->usedStableIds_)if(!ownership_.contains(id)){ownership_.emplace(id,primary_);primary->identities.push_back(id);}
    }
}
bool SceneManager::ResolveActiveEnvironment(std::shared_ptr<const TODProfile>& profile)
{
    if(!OwnerThread())return Fail("Scene.WrongThread");
    auto* record=Find(primary_);if(!record||!record->active){profile.reset();return true;}
    if(!record->profile&&!record->environment.todProfile.empty()&&environmentValidator_)
        if(!environmentValidator_(record->environment,record->profile,error_))return false;
    profile=record->profile;return true;
}
bool SceneManager::ValidateEnvironmentProfiles(const std::function<bool(const TODProfile&,std::string&)>& validate)
{
    if(!OwnerThread())return Fail("Scene.WrongThread");
    for(const auto& [_,record]:records_)if(record->active&&record->profile&&!validate(*record->profile,error_))return false;
    return true;
}
std::uint64_t SceneManager::StartRequest(Record& scene,SceneLoadMode mode,SceneHandle replacing,std::string source)
{
    if(closed_||applying_||static_cast<unsigned>(mode)>1){Fail(closed_?"Scene.Closed":"Scene.CommitNotSafe");return 0;}
    if(creationScope_&&!Find(creationScope_)->active){Fail("Scene.Inactive");return 0;}
    if(!world_->IsIterating())SynchronizeMembers();
    if(mode==SceneLoadMode::Additive&&scene.active){Fail("Scene.AlreadyActive");return 0;}
    if(mode==SceneLoadMode::Replace&&!replacing)replacing=scene.active?scene.handle:primary_;
    if(mode==SceneLoadMode::Additive)replacing={};
    const auto* previous=Find(replacing);
    if(replacing&&(!previous||!previous->active)){Fail("Scene.Inactive");return 0;}
    if(scene.active&&replacing!=scene.handle){Fail("Scene.InvalidRequest");return 0;}
    const auto request=Identity(nextSceneRequest);if(!request){Fail("Scene.IdentityExhausted");return 0;}
    if(mode==SceneLoadMode::Replace)replacementClaims_.insert_or_assign(replacing.value,request);
    scene.token.Cancel();scene.request=request;scene.mode=mode;scene.replacing=replacing;scene.replacingRevision=previous?previous->revision:0;
    scene.makePrimary=mode==SceneLoadMode::Replace&&(!primary_||replacing==primary_);
    scene.requestedSource=std::move(source);scene.error.clear();scene.state=SceneState::Preparing;
    ++statistics_.requested;error_.clear();return request;
}
std::uint64_t SceneManager::RequestDocument(SceneHandle scene,SceneDocument document,SceneLoadMode mode,SceneHandle replacing)
{
    if(!OwnerThread()){Fail("Scene.WrongThread");return 0;}auto* record=Find(scene);if(!record||!document){Fail("Scene.InvalidRequest");return 0;}
    commands_.reserve(commands_.size()+1);const auto request=StartRequest(*record,mode,replacing,{});if(!request)return 0;
    commands_.push_back({scene,request,std::move(document),{},false});record->state=SceneState::ReadyToActivate;return request;
}
std::uint64_t SceneManager::RequestFile(SceneHandle scene,std::filesystem::path path,SceneLoadMode mode,SceneHandle replacing,Reader reader)
{
    if(!OwnerThread()){Fail("Scene.WrongThread");return 0;}
    if(closed_||applying_){Fail("Scene.CommitNotSafe");return 0;}
    if(!OwnerThread()){Fail("Scene.WrongThread");return 0;}auto* record=Find(scene);if(!record||path.empty()){Fail("Scene.InvalidRequest");return 0;}
    path=std::filesystem::absolute(path);
    if(!jobs_)jobs_=std::make_unique<TaskQueue>(2,64);
    const auto source=path.generic_u8string();const auto request=StartRequest(*record,mode,replacing,std::string(source.begin(),source.end()));if(!request)return 0;
    if(!reader)reader=[](const std::filesystem::path& source,SceneDocument& document,std::string& error,const CancellationToken& token){return !token.IsCancelled()&&ReadSceneDocument(source,document,error);};
    const auto weak=std::weak_ptr<AsyncState>(asynchronous_);
    record->token=jobs_->Submit([weak,scene,request,path=std::move(path),reader=std::move(reader)](const CancellationToken& token)->TaskQueue::Completion{
        auto command=std::make_shared<Command>();command->scene=scene;command->request=request;
        if(!reader(path,command->document,command->error,token)&&command->error.empty())command->error="Scene.ReadFailed";
        return [weak,command]{if(auto state=weak.lock())state->Complete(std::move(*command));};
    },[weak,scene,request](const TaskErrors&){if(auto state=weak.lock())state->Complete({scene,request,{},"Scene.WorkerFailed",false});});
    if(!record->token.Accepted()){Failed(*record,"Scene.QueueFull");Fail("Scene.QueueFull");return 0;}return request;
}
bool SceneManager::Cancel(SceneHandle scene)
{
    if(!OwnerThread())return Fail("Scene.WrongThread");auto* record=Find(scene);if(!record||closed_||applying_)return Fail("Scene.InvalidRequest");
    if(creationScope_&&!Find(creationScope_)->active)return Fail("Scene.Inactive");
    if(record->state!=SceneState::Preparing&&record->state!=SceneState::ReadyToActivate)return true;
    const auto request=Identity(nextSceneRequest);if(!request)return Fail("Scene.IdentityExhausted");record->token.Cancel();record->request=request;record->state=SceneState::Cancelled;++statistics_.cancelled;return true;
}
void SceneManager::Retire(Record& record,std::span<const StableEntityId> invalidated)noexcept
{
    if(record.teardownDone)return;record.teardownDone=true;record.active=false;record.state=SceneState::Unloading;++record.revision;record.token.Cancel();
    const auto previous=applying_;applying_=true;
    try
    {
        for(const auto entity:record.members)if(world_->IsKnown(entity)){world_->slots_[entity.index].record.enabled=false;world_->TouchSpatial(entity);}
        if(!world_->Defer([](World& world){return world.RefreshComponents();}))++statistics_.callbackFaults;
        if(unloadHook_)world_->InvokeComponentCallback([&]{unloadHook_(record.handle,record.members,invalidated);});
    }
    catch(...){++statistics_.callbackFaults;}
    applying_=previous;
}
bool SceneManager::Unload(SceneHandle scene)
{
    if(!OwnerThread())return Fail("Scene.WrongThread");auto* record=Find(scene);if(!record||closed_||applying_)return Fail("Scene.InvalidRequest");
    if(creationScope_&&!Find(creationScope_)->active)return Fail("Scene.Inactive");
    if(record->state==SceneState::Unloading)return true;
    if(!record->active){Cancel(scene);record->state=SceneState::Unloaded;return true;}
    commands_.reserve(commands_.size()+1);const auto request=Identity(nextSceneRequest);if(!request)return Fail("Scene.IdentityExhausted");
    record->request=request;Retire(*record,record->identities);commands_.push_back({scene,request,{},{},true});return true;
}
void SceneManager::Failed(Record& scene,std::string error)
{scene.state=SceneState::Failed;scene.error=std::move(error);++statistics_.failed;Notify(18,scene);}
void SceneManager::Notify(std::uint64_t type,const SceneSnapshot& scene)
{
    try
    {
        const auto length=(std::min)(scene.error.size(),std::size_t{4000});std::vector<std::uint8_t> bytes(24+length);
        Write64(bytes,0,scene.handle.value);Write64(bytes,8,scene.request);Write32(bytes,16,static_cast<std::uint32_t>(scene.state));Write32(bytes,20,static_cast<std::uint32_t>(length));
        std::copy_n(scene.error.begin(),length,bytes.begin()+24);
        if(events_->GetPublisher().Post(type,1,bytes,EventPhase::AfterLogic)!=EventPostResult::Accepted)++statistics_.notificationFailures;
    }
    catch(...){++statistics_.notificationFailures;}
}
SceneSnapshot SceneManager::Inspect(SceneHandle scene)const
{if(!OwnerThread())throw std::logic_error("Scene.WrongThread");const auto* record=Find(scene);return record?static_cast<const SceneSnapshot&>(*record):SceneSnapshot{};}
SceneHandle SceneManager::ActiveScene()const noexcept{const auto* record=Find(primary_);return record&&record->active?primary_:SceneHandle{};}
bool SceneManager::SynchronizeIdle()
{if(!OwnerThread()||closed_||applying_||world_->IsInFrame()||world_->IsIterating())return Fail("Scene.CommitNotSafe");SynchronizeMembers();return true;}
bool SceneManager::ExchangeCreationScope(SceneHandle scene,SceneHandle& previous)
{if(!OwnerThread()||(scene&&!Find(scene)))return Fail("Scene.ContextMismatch");previous=creationScope_;creationScope_=scene;return true;}
SceneStatistics SceneManager::Statistics()const
{
    if(!OwnerThread())throw std::logic_error("Scene.WrongThread");auto result=statistics_;result.pendingJobs=jobs_?jobs_->Pending():0;result.records=records_.size();result.members=ownership_.size();return result;
}
bool SceneManager::Apply(Command command,const Validator& finalValidation)
{
    auto* record=Find(command.scene);
    if(!record||record->request!=command.request||closed_){++statistics_.staleResults;return true;}
    if(!command.unload&&!command.error.empty()){Failed(*record,std::move(command.error));return true;}
    auto* old=command.unload?record:Find(record->replacing);
    if((command.unload&&record->state!=SceneState::Unloading)||(!command.unload&&record->mode==SceneLoadMode::Replace
        &&(replacementClaims_[record->replacing.value]!=command.request||(record->replacing&&(!old||!old->active||old->revision!=record->replacingRevision)))))
    {record->state=SceneState::Cancelled;++statistics_.staleResults;++statistics_.cancelled;return true;}
    applying_=true;struct Guard{bool& value;~Guard(){value=false;}} guard{applying_};
    World candidate{World::StagingTag{}};std::string error;
    const auto retiring=old?old->handle:SceneHandle{};
    candidate.externalReference_=[this,retiring](StableEntityId id){const auto entity=world_->FindByStableId(id);const auto owner=SceneOf(entity);return static_cast<bool>(entity)&&static_cast<bool>(owner)&&owner!=retiring;};
    if(!command.unload&&(!InstantiateScene(command.document,candidate,error)||(validator_&&!validator_(candidate,error))||(finalValidation&&!finalValidation(candidate,error))))
    {Failed(*record,error.empty()?"Scene.DependencyFailed":std::move(error));return true;}
    auto settings=candidate.EnvironmentSettings();std::shared_ptr<const TODProfile> profile;
    if(!command.unload&&environmentValidator_&&!environmentValidator_(settings,profile,error))
    {Failed(*record,error.empty()?"TOD.ResolveFailed":std::move(error));return true;}
    auto identities=candidate.KnownStableIds();auto ownership=ownership_;auto publishedSource=record->requestedSource;
    if(old)for(const auto id:old->identities)ownership.erase(id);
    for(const auto id:identities)if(!ownership.emplace(id,record->handle).second){Failed(*record,"Scene.StableIdConflict");return true;}
    std::unordered_set<StableEntityId> removedIds;
    if(old)removedIds.insert(old->identities.begin(),old->identities.end());
    for(const auto entity:candidate.Entities())removedIds.erase(candidate.Get(entity)->stableId);
    std::vector<StableEntityId> invalidated(removedIds.begin(),removedIds.end());std::sort(invalidated.begin(),invalidated.end());
    struct ReferenceEdit {ComponentHandle component;ComponentTypeId type;std::size_t field;};std::vector<ReferenceEdit> references;
    for(const auto entity:world_->Entities())
    {
        if(OwnerOf(entity)==retiring)continue;
        for(const auto component:world_->Components(entity))
        {
            const auto* value=world_->GetComponent(component);const auto* description=FindComponentDescriptor(value->Type());if(!description||!description->read||!description->write)continue;
            for(std::size_t field=0;field<description->properties.size();++field)if(description->properties[field].type==PropertyType::EntityReference)
            {
                PropertyValue property;if(!description->read(*world_,component,field,property,error)){Failed(*record,error);return true;}
                if(const auto* reference=std::get_if<PropertyEntityReference>(&property);reference&&removedIds.contains(reference->stableId))references.push_back({component,value->Type(),field});
            }
        }
    }
    const auto newScope=command.unload?0:Identity(nextScene);if(!command.unload&&!newScope){Failed(*record,"Scene.IdentityExhausted");return true;}
    SceneSnapshot unloaded;if(old){unloaded.handle=old->handle;unloaded.request=command.request;unloaded.state=SceneState::Unloaded;unloaded.scope=old->scope;}
    std::vector<EntityHandle> added;const std::vector<EntityHandle> noMembers;const std::vector<StableEntityId> noIdentities;
    if(!world_->CommitScene(candidate,old?old->members:noMembers,old?old->identities:noIdentities,added,[&]{
        if(old){old->request=command.request;Retire(*old,invalidated);}
        for(const auto& edit:references)
        {
            try{const auto* description=FindComponentDescriptor(edit.type);std::string referenceError;
                if(!description||!description->write||!description->write(*world_,edit.component,edit.field,PropertyEntityReference{},referenceError))++statistics_.callbackFaults;}
            catch(...){++statistics_.callbackFaults;}
        }
    }))
    {Failed(*record,world_->LastError());return true;}
    ownership_.swap(ownership);
    if(old)
    {
        old->members.clear();old->identities.clear();old->active=false;old->state=SceneState::Unloaded;old->error.clear();++statistics_.unloaded;Notify(17,unloaded);
    }
    if(command.unload)
    {
        if(primary_==record->handle){primary_={};for(const auto& [_,other]:records_)if(other->active){primary_=other->handle;break;}}
        world_->sceneEnvironment_=primary_?Find(primary_)->environment:SceneEnvironmentSettings{};
        error_.clear();return true;
    }
    record->members.swap(added);record->identities.swap(identities);record->active=true;record->state=SceneState::Active;record->teardownDone=false;++record->revision;record->scope=newScope;
    record->source=std::move(publishedSource);record->error.clear();if(record->makePrimary||!primary_)primary_=record->handle;
    record->environment=std::move(settings);record->profile=std::move(profile);world_->sceneEnvironment_=Find(primary_)->environment;
    ++statistics_.activated;Notify(16,*record);error_.clear();return true;
}
bool SceneManager::ActivateIdle(SceneHandle scene,const SceneDocument& document,SceneLoadMode mode,SceneHandle replacing,const Validator& finalValidation,std::string source)
{
    if(!OwnerThread()||closed_||applying_||world_->IsInFrame()||world_->IsIterating()||world_->HasPendingCommands())return Fail("Scene.CommitNotSafe");
    auto* record=Find(scene);if(!record||!document)return Fail("Scene.InvalidRequest");const auto request=StartRequest(*record,mode,replacing,std::move(source));if(!request)return false;
    try{Apply({scene,request,document,{},false},finalValidation);}
    catch(...){Failed(*record,"Scene.PrepareFailed");}
    if(record->state!=SceneState::Active){error_=record->error;return false;}
    if(!world_->ActivateStagedComponents())++statistics_.callbackFaults;
    if(!world_->FlushCommands())++statistics_.callbackFaults;
    return true;
}
bool SceneManager::FrameBegin()
{
    if(!OwnerThread()||closed_||applying_||!world_->IsInFrame()||!world_->preparingFrame_)return Fail("Scene.CommitNotSafe");
    SynchronizeMembers();std::vector<Command> snapshot;snapshot.swap(commands_);
    if(jobs_)
    {
        const auto ready=jobs_->ReadyCount();jobs_->Pump(ready);std::vector<Command> completed;
        {std::lock_guard lock(asynchronous_->mutex);completed.swap(asynchronous_->ready);}
        // 本次接收的后台结果不能进入已经固定的提交批次；下一 FrameBegin 再复验代数。
        for(auto& command:completed)
        {
            if(auto* record=Find(command.scene);record&&record->request==command.request&&command.error.empty())record->state=SceneState::ReadyToActivate;
            commands_.push_back(std::move(command));
        }
    }
    for(auto& command:snapshot)
    {
        const auto scene=command.scene;
        try{if(!Apply(std::move(command)))return false;}
        catch(...){if(auto* record=Find(scene))Failed(*record,"Scene.PrepareFailed");}
    }
    if(!world_->ActivateStagedComponents())++statistics_.callbackFaults;
    if(!world_->FlushPhaseCommands())
    {if(world_->LastError()=="World.ComponentCallbackFailed")++statistics_.callbackFaults;else{error_=world_->LastError();return false;}}
    return true;
}
}
