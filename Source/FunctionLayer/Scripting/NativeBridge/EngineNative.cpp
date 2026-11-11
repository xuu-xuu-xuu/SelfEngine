#define SELFENGINE_NATIVE_EXPORTS
#include "FunctionLayer/Scripting/NativeBridge/EngineNative.h"
#include "FunctionLayer/Runtime/World.h"
#include "FunctionLayer/Runtime/Environment.h"
#include "ResourceLayer/Scenes/SceneSerializer.h"
#include "FunctionLayer/Physics/PhysicsWorld.h"
#include "FunctionLayer/GameUI/UiTree.h"
#include <nlohmann/json.hpp>
#include "FunctionLayer/Application/FrameData.h"
#include "FunctionLayer/Application/SandboxApp.h"
#include "FunctionLayer/Rendering/SandboxRenderer.h"
#include "WindowProfile.h"
#include "PlatformLayer/InputState.h"
#include "CoreLayer/Localization.h"
#include "CoreLayer/Log.h"
#include "ToolLayer/Editor/EditorApplication.h"
#include "ToolLayer/Editor/EditorSession.h"
#include "ToolLayer/Editor/ImGuiEditor.h"
#include "ResourceLayer/Assets/AssetStore.h"
#include <filesystem>
#include <unordered_map>
#include <imgui/imgui.h>
#include <algorithm>
#include <cmath>
#include <cstring>
#include <limits>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

namespace
{
thread_local const char* lastError = "Bridge.Success";
std::recursive_mutex sessionsMutex;
struct Session
{
    Session(){environment.ConfigureClock(1.0/60,8,false);environment.Events().RegisterType({4,"engine.scene_state",1,8});}
    ~Session(){environment.BeginShutdown();world.Clear();while(world.HasPendingCommands())world.FlushCommands();}
    se::Environment environment;
    se::World& world=environment.GetWorld();
    se::Environment* borrowedEnvironment=nullptr;
    se::Environment& Env(){return borrowedEnvironment?*borrowedEnvironment:environment;}
    const se::Environment& ReadEnvironment()const{return borrowedEnvironment?*borrowedEnvironment:editor?editor->EditEnvironment():environment;}
    se::World* borrowedWorld=nullptr;
    se::World& WorldRef() { return borrowedWorld?*borrowedWorld:world; }
    const se::World& ReadWorld() const {return borrowedWorld?*borrowedWorld:editor?editor->EditWorld():world;}
    se::PhysicsWorld physics;
    std::unordered_map<std::uint64_t,se::EntityHandle> bodyOwners;
    bool physicsDebugDraw=true;
    se::FixedStepClock physicsClock;
    std::vector<SE_ContactEvent> contactEvents;
    se::UiTree ui;
    se::UiTree* borrowedUi=nullptr;
    se::UiTree& UiRef() { return borrowedUi?*borrowedUi:ui; }
    std::vector<se::UiEvent> uiEvents;
    SE_InputState input{};
    SE_PointerState pointer{};
    std::unordered_map<SE_Entity,se::CharacterController> characters;
    Localization localization;
    std::unique_ptr<se::AssetStore> assets;
    bool windowRunning=false,stopWindow=false;
    std::filesystem::path windowCapture;
    std::uint64_t windowCaptureFrame=0;
    std::filesystem::path windowProfile;
    std::uint32_t profileWidth=0,profileHeight=0;
    se::EditorSession* editor=nullptr;
    int32_t pendingPlay=-1;
    SE_EditorCallback editorCallback=nullptr;
    void* editorContext=nullptr;
    SE_EditorDataCallback editorDataCallback=nullptr;
    void* editorDataContext=nullptr;
    std::string inspectorError;
    std::thread::id thread = std::this_thread::get_id();
    SE_UpdateCallback callback = nullptr;
    void* context = nullptr;
    bool stepping = false;
    SE_TickCallback tickCallback=nullptr;
    void* tickContext=nullptr;
    SE_EventCallback eventCallback=nullptr;
    void* eventContext=nullptr;
    SE_SceneCallback sceneCallback=nullptr;
    void* sceneContext=nullptr;
    std::unordered_map<std::uint64_t,se::EventBus::SubscriptionToken> eventSubscriptions;
    bool environmentDriven=false,environmentPhysics=false,pendingEnvironmentRelease=false,callbackFailed=false;
    double value = 0.0;
    uint64_t stepCount = 0;
};
struct Slot
{
    std::unique_ptr<Session> value;
    uint32_t generation = 1;
};
std::vector<Slot> slots;
struct EventPublisher {SE_Session session;se::EventBus::Publisher value;};
std::unordered_map<std::uint64_t,EventPublisher> eventPublishers;
se::EventBus& EventsFor(Session& session)
{
    auto& events=session.Env().Events();
    if(!session.Env().Closing()&&!events.BindWorld(session.WorldRef()))throw std::runtime_error("Event.ContextMismatch");
    if(!session.Env().Closing())events.SynchronizeIdleClock(session.Env().FrameIndex(),session.Env().FixedStepIndex());
    for(auto it=session.eventSubscriptions.begin();it!=session.eventSubscriptions.end();)if(!it->second.Active())it=session.eventSubscriptions.erase(it);else ++it;
    return events;
}
void CloseEventPublishers(SE_Session handle)
{
    for(auto it=eventPublishers.begin();it!=eventPublishers.end();)
        if(it->second.session==handle){it->second.value.Close();it=eventPublishers.erase(it);}else ++it;
}

int32_t Error(int32_t code, const char* key) noexcept { lastError = key; return code; }
template<class Function> int32_t Protect(Function&& function) noexcept
{
    try { std::lock_guard lock(sessionsMutex); lastError = "Bridge.Success"; return function(); }
    catch (const std::bad_alloc&) { return Error(SE_OUT_OF_MEMORY, "Bridge.OutOfMemory"); }
    catch (...) { return Error(SE_INTERNAL_ERROR, "Bridge.NativeException"); }
}
Session* Find(SE_Session handle, bool checkThread = true)
{
    const uint32_t encodedIndex = static_cast<uint32_t>(handle);
    const uint32_t generation = static_cast<uint32_t>(handle >> 32);
    if (encodedIndex == 0 || encodedIndex - 1 >= slots.size()
        || slots[encodedIndex - 1].generation != generation || !slots[encodedIndex - 1].value)
    {
        Error(SE_INVALID_HANDLE, "Bridge.InvalidSession");
        return nullptr;
    }
    Session* session = slots[encodedIndex - 1].value.get();
    if (checkThread && session->thread != std::this_thread::get_id())
    {
        Error(SE_WRONG_THREAD, "Bridge.WrongThread");
        return nullptr;
    }
    return session;
}
int32_t MissingSession() { return std::strcmp(lastError, "Bridge.WrongThread") == 0 ? SE_WRONG_THREAD : SE_INVALID_HANDLE; }
bool ValidUtf8(const char* text, uint32_t size)
{
    if (!text && size != 0) return false;
    for (uint32_t i = 0; i < size;)
    {
        const auto first = static_cast<unsigned char>(text[i++]);
        if (first == 0) return false;
        if (first < 0x80) continue;
        uint32_t code = 0, tail = 0, minimum = 0;
        if (first >= 0xc2 && first <= 0xdf) { code = first & 0x1f; tail = 1; minimum = 0x80; }
        else if (first >= 0xe0 && first <= 0xef) { code = first & 0x0f; tail = 2; minimum = 0x800; }
        else if (first >= 0xf0 && first <= 0xf4) { code = first & 0x07; tail = 3; minimum = 0x10000; }
        else return false;
        if (tail > size - i) return false;
        while (tail--)
        {
            const auto next = static_cast<unsigned char>(text[i++]);
            if ((next & 0xc0) != 0x80) return false;
            code = (code << 6) | (next & 0x3f);
        }
        if (code < minimum || code > 0x10ffff || (code >= 0xd800 && code <= 0xdfff)) return false;
    }
    return true;
}
int32_t CopyText(const std::string_view text, char* buffer, uint32_t capacity, uint32_t* required)
{
    if (!required || (capacity != 0 && !buffer)) return SE_INVALID_ARGUMENT;
    *required = static_cast<uint32_t>(text.size() + 1);
    if (capacity < *required) return SE_BUFFER_TOO_SMALL;
    std::memcpy(buffer, text.data(), text.size());
    buffer[text.size()] = '\0';
    return SE_OK;
}
nlohmann::json BindingJson(const se::UiEventBinding& binding)
{return {{"Event",static_cast<int>(binding.event)},{"Entity",binding.entity},{"Script",binding.script},{"Action",binding.action}};}
bool ValidTransform(const SE_Transform& t)
{
    const se::Transform transform{{t.px,t.py,t.pz},{t.qx,t.qy,t.qz,t.qw},{t.sx,t.sy,t.sz}};
    return se::IsValid(transform)&&std::abs(t.sx)>1e-6f&&std::abs(t.sy)>1e-6f&&std::abs(t.sz)>1e-6f;
}
se::BodyHandle BodyFromPacked(SE_Body handle) { return {static_cast<uint32_t>(handle),static_cast<uint32_t>(handle>>32)}; }
se::Vec3 Vector(SE_Vector3 value) { return {value.x,value.y,value.z}; }
std::filesystem::path ResourceDirectory()
{
    // 候选/便携运行时携带构建后的 shader 资源；源码 ENGINE_ROOT 仅定位工具与配置。
    HMODULE module=nullptr;wchar_t modulePath[32768]{};
    if(!GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS|GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
        reinterpret_cast<LPCWSTR>(&se_session_run_window),&module))return {};
    const DWORD pathLength=GetModuleFileNameW(module,modulePath,32768);
    if(!pathLength||pathLength>=32768)return {};
    return std::filesystem::path(modulePath).parent_path()/L"Resources";
}
std::filesystem::path EditorEnvironmentPath(const wchar_t* name)
{
    const DWORD required=GetEnvironmentVariableW(name,nullptr,0);
    if(!required||required>32768)return {};
    std::wstring value(required,L'\0');
    const auto length=GetEnvironmentVariableW(name,value.data(),required);
    if(!length||length>=required)return {};
    value.resize(length);return std::filesystem::path(value);
}
bool HasBody(Session& session, uint64_t stableId)
{
    for (auto body:session.physics.Bodies()) if (session.physics.GetBody(body)->description.entityStableId==stableId) return true;
    return false;
}
bool BodyAlive(Session& session,SE_Body body)
{
    if(!session.physics.IsAlive(BodyFromPacked(body)))return false;
    const auto owner=session.bodyOwners.find(body);
    return owner==session.bodyOwners.end()||(session.WorldRef().IsAlive(owner->second)&&static_cast<bool>(session.Env().Scenes().SceneOf(owner->second)));
}
void SweepBodies(Session& session)
{
    for (auto body:session.physics.Bodies())
        if(!BodyAlive(session,body.Pack())||!session.WorldRef().IsAlive(session.WorldRef().FindByStableId(session.physics.GetBody(body)->description.entityStableId)))session.physics.DestroyBody(body);
    std::erase_if(session.bodyOwners,[&](const auto& item){return !session.physics.IsAlive(BodyFromPacked(item.first));});
    std::erase_if(session.characters,[&](const auto& item){return !session.WorldRef().IsAlive(se::EntityHandle::FromPacked(item.first));});
}
se::SceneManager& ScenesFor(Session& session)
{
    EventsFor(session);auto& scenes=session.Env().Scenes();if(!session.Env().Closing()&&!scenes.BindWorld(session.WorldRef()))throw std::logic_error("Scene.ContextMismatch");return scenes;
}
void SceneServices(Session& session,SE_Session handle)
{
    session.Env().Events().RegisterType({4,"engine.scene_state",1,8});
    if(session.assets||!session.borrowedEnvironment)session.Env().SetTODResolver([&session](const std::string& id,std::string& error){
        std::vector<se::AssetDiagnostic> diagnostics;auto profile=session.assets?session.assets->LoadTODProfile(id,diagnostics):nullptr;
        if(!profile)error=diagnostics.empty()?"TOD.ResolveFailed":diagnostics.back().key;return profile;
    });
    session.Env().Scenes().SetValidator([&session](const se::World& candidate,std::string& error){
        for(const auto entity:candidate.Entities())if(const auto* mesh=candidate.GetMesh(entity))
        {
            std::vector<se::AssetDiagnostic> diagnostics;
            if(!session.assets||!session.assets->LoadModel(mesh->meshAsset,diagnostics)){error="Scene.DependencyFailed";return false;}
            for(const auto& material:mesh->materials)if(!material.empty()&&!session.assets->LoadMaterial(material,diagnostics)){error="Scene.DependencyFailed";return false;}
        }
        return true;
    });
    session.Env().Scenes().SetUnloadHook([&session,handle](se::SceneHandle scene,std::span<const se::EntityHandle> members,std::span<const se::StableEntityId> invalidated){
        bool failed=false;const bool previous=session.stepping;session.stepping=true;
        struct Restore{Session& session;bool previous;~Restore(){session.stepping=previous;}} restore{session,previous};
        try
        {
            if(session.sceneCallback)
            {
                std::vector<SE_Entity> packed;packed.reserve(members.size());for(const auto entity:members)packed.push_back(entity.Pack());
                struct Unlock{Unlock(){sessionsMutex.unlock();}~Unlock(){sessionsMutex.lock();}} unlock;
                failed=session.sceneCallback(handle,scene.value,packed.data(),static_cast<uint32_t>(packed.size()),invalidated.data(),static_cast<uint32_t>(invalidated.size()),session.sceneContext)!=0;
            }
        }
        catch(...){failed=true;}
        for(const auto body:session.physics.Bodies())
        {
            const auto owner=session.bodyOwners.find(body.Pack());
            if(owner!=session.bodyOwners.end()&&std::find(members.begin(),members.end(),owner->second)!=members.end())
            {session.physics.DestroyBody(body);session.bodyOwners.erase(owner);}
        }
        std::erase_if(session.characters,[&](const auto& item){return std::find_if(members.begin(),members.end(),[&](auto entity){return entity.Pack()==item.first;})!=members.end();});
        std::erase_if(session.contactEvents,[&](const auto& event){return std::any_of(members.begin(),members.end(),[&](auto entity){const auto* record=session.WorldRef().Get(entity);return record&&(record->stableId==event.entity_a||record->stableId==event.entity_b);});});
        if(failed)throw std::runtime_error("Scene.CallbackFailed");
    });
}
void CaptureInput(Session& session,const InputState& input,bool consumed,bool pointerConsumed)
{
            session.pointer={};session.pointer.x=input.mouseX;session.pointer.y=input.mouseY;session.pointer.consumed=pointerConsumed?1:0;
            if(!pointerConsumed)
            {
                for(size_t i=0;i<input.mouseButtons.size();++i)if(input.mouseButtons[i])session.pointer.down|=1u<<i;
                for(const auto& event:input.pointerEvents)
                {
                    if(event.type==PointerEventType::Cancel){session.pointer.down=session.pointer.pressed=session.pointer.released=0;session.pointer.wheel=0;session.pointer.consumed=1;continue;}
                    if(event.type==PointerEventType::Wheel)session.pointer.wheel+=event.wheel;
                    if(event.button>=3)continue;
                    if(event.type==PointerEventType::Down)session.pointer.pressed|=1u<<event.button;
                    if(event.type==PointerEventType::Up)session.pointer.released|=1u<<event.button;
                }
            }
            session.input={};session.input.keyboard_consumed=consumed?1:0;
            if(consumed)return;
            for(size_t i=0;i<input.keysDown.size();++i)session.input.down[i]=input.keysDown[i]?1:0;
            for(const auto& event:input.keyEvents)
            {
                // 取消也标记本帧不可消费，清除托管层尚未到固定步的输入边沿。
                if(event.type==KeyEventType::Reset){session.input={};session.input.keyboard_consumed=1;continue;}
                if(event.keyCode>=256)continue;
                if(event.type==KeyEventType::Pressed)session.input.pressed[event.keyCode]=1;
                else if(event.type==KeyEventType::Released)session.input.released[event.keyCode]=1;
            }
}
int32_t PhysicsStep(Session& session, double dt)
{
    if (!std::isfinite(dt)||dt<=0||dt>1) return Error(SE_INVALID_ARGUMENT,"Bridge.InvalidDelta");
    SweepBodies(session);
    const bool stepped=session.environmentDriven?session.physics.StepFromEnvironment(static_cast<float>(dt)):session.physics.Step(static_cast<float>(dt));
    if(!stepped)return Error(SE_INVALID_ARGUMENT,"Bridge.PhysicsFailed");
    for (auto body:session.physics.Bodies())
    {
        const auto& state=*session.physics.GetBody(body);
        const auto entity=session.WorldRef().FindByStableId(state.description.entityStableId);
        if (const auto* original=session.WorldRef().GetTransform(entity))
        {
            auto transform=*original;
            transform.position=state.description.shape.center;
            transform.rotation=state.description.shape.rotation;
            if (!session.WorldRef().SetTransform(entity,transform)) return Error(SE_INTERNAL_ERROR,"Bridge.WorldOperationFailed");
        }
    }
    for (const auto& event:session.physics.Events())
        session.contactEvents.push_back({event.entityA,event.entityB,event.began?1:0,event.ended?1:0});
    return SE_OK;
}
int32_t EnvironmentCallback(Session& session,SE_Session handle,std::uint64_t component,std::uint32_t phase,const se::TickContext& context)
{
    if(!session.tickCallback)return 0;
    struct Unlock{Unlock(){sessionsMutex.unlock();}~Unlock(){sessionsMutex.lock();}} unlock;
    const auto result=session.tickCallback(handle,component,phase,context.deltaSeconds,context.frame,context.fixedStep,session.tickContext);
    if(result)session.callbackFailed=true;return result;
}
se::EnvironmentHooks EnvironmentHooks(Session& session,SE_Session handle)
{
    se::EnvironmentHooks hooks;
    hooks.phase=[&session,handle](se::EnvironmentPhase phase,const se::TickContext& context){
        if(phase==se::EnvironmentPhase::FrameBegin&&session.callback)
        {
            struct Unlock{Unlock(){sessionsMutex.unlock();}~Unlock(){sessionsMutex.lock();}} unlock;
            if(session.callback(handle,context.deltaSeconds,session.context)){session.callbackFailed=true;return false;}
        }
        if(phase==se::EnvironmentPhase::BeforePhysics)SweepBodies(session);
        if(EnvironmentCallback(session,handle,0,0x100u|static_cast<std::uint32_t>(phase),context))return false;
        if(phase==se::EnvironmentPhase::FrameEnd)session.UiRef().Update(context.frameSeconds);
        return true;
    };
    hooks.physics=[&session](double seconds){return !session.environmentDriven||!session.environmentPhysics||PhysicsStep(session,seconds)==SE_OK;};
    return hooks;
}
void FinishEnvironmentRelease(Session& session)
{
    if(!session.pendingEnvironmentRelease)return;
    session.environmentDriven=false;session.environmentPhysics=false;session.pendingEnvironmentRelease=false;
    session.physics.SetEnvironmentDriven(false);
    if(!session.Env().Closing()){session.Env().ConfigureClock(1.0/60,8,false);session.Env().SetPaused(false);}session.physicsClock.ClearRemainder();
}
// 调用者持有 sessionsMutex；普通帧与编辑器单步共用回调、物理、UI 和退出清理边界。
int32_t AdvanceSession(Session& session,SE_Session handle,double seconds,bool singleStep)
{
    if(session.stepping)return Error(SE_BUSY,"Bridge.SessionBusy");
    if(!singleStep&&session.Env().FramePaused()&&!session.Env().Closing())return SE_OK;
    struct SteppingGuard{Session& value;~SteppingGuard(){value.stepping=false;}} guard{session};
    session.stepping=true;session.callbackFailed=false;
    const auto hooks=EnvironmentHooks(session,handle);
    const bool result=singleStep?session.Env().StepFrame(hooks):session.Env().RunFrame(seconds,hooks);
    SweepBodies(session);FinishEnvironmentRelease(session);
    if(!result)return session.callbackFailed?Error(SE_CALLBACK_FAILED,"Bridge.CallbackFailed"):Error(SE_INTERNAL_ERROR,session.Env().LastError().c_str());
    ++session.stepCount;return SE_OK;
}
bool EditorDocumentRequest(SE_Session handle,Session& session,const nlohmann::json& request,bool snapshot,
    nlohmann::json& response,std::string& error)
{
    error.clear();std::string reply;
    const auto command=request.dump();
    const auto result=Protect([&]()->int32_t{
        if(Find(handle)!=&session)return MissingSession();
        if(!session.editor||!session.editorDataCallback)return Error(SE_INVALID_ARGUMENT,"Bridge.EditorNotRunning");
        const auto operation=request.value("Document",std::string{});
        if(session.editor->IsPlaying()&&(operation=="Save"||operation=="Restore"))return Error(SE_BUSY,"Bridge.EditorReadOnly");
        const bool prior=session.stepping;session.stepping=true;
        struct Restore{Session& session;bool prior;~Restore(){sessionsMutex.lock();session.stepping=prior;}} restore{session,prior};
        sessionsMutex.unlock();
        std::uint32_t required=0;std::vector<char> buffer;
        if(snapshot)
        {
            const auto measured=session.editorDataCallback(handle,0,command.data(),static_cast<std::uint32_t>(command.size()),nullptr,0,&required,session.editorDataContext);
            if((measured!=SE_OK&&measured!=SE_BUFFER_TOO_SMALL)||required==0||required>32u*1024*1024)
                return Error(SE_CALLBACK_FAILED,"Script.MetadataFailed");
            buffer.resize(required);
        }
        else buffer.resize(4096); // 命令结果仅 {} 或 Error；长度不足也不能重试有副作用的命令。
        const auto called=session.editorDataCallback(handle,0,command.data(),static_cast<std::uint32_t>(command.size()),buffer.data(),
            static_cast<std::uint32_t>(buffer.size()),&required,session.editorDataContext);
        if(called!=SE_OK||required==0||required>buffer.size()||buffer[required-1]!='\0')return Error(SE_CALLBACK_FAILED,"Script.MetadataFailed");
        reply.assign(buffer.data(),required-1);return SE_OK;
    });
    if(result!=SE_OK){error=lastError;return false;}
    try
    {
        response=nlohmann::json::parse(reply);
        if(!response.is_object()){error="Script.DocumentInvalid";return false;}
        if(response.contains("Error")){error=response.at("Error").get<std::string>();if(error.empty())error="Script.DocumentInvalid";return false;}
        return true;
    }
    catch(const std::exception&){error="Script.DocumentInvalid";return false;}
}
void DrawScriptInspector(SE_Session handle,Session& session,uint64_t entity,bool playing)
{
    if(!session.editorDataCallback)return;
    ImGui::SeparatorText(session.localization.GetText("Script.Inspector").c_str());
    uint32_t required=0;
    if(se_editor_inspector_query(handle,entity,nullptr,0,&required)!=SE_BUFFER_TOO_SMALL||required>4*1024*1024)
    {ImGui::TextUnformatted(session.localization.GetText("Script.MetadataFailed").c_str());return;}
    std::vector<char> text(required);
    if(se_editor_inspector_query(handle,entity,text.data(),required,&required)!=SE_OK)return;
    try
    {
        const auto document=nlohmann::json::parse(text.data());
        auto apply=[&](const nlohmann::json& change){
            const auto request=change.dump();
            if(se_editor_inspector_apply(handle,entity,request.data(),static_cast<uint32_t>(request.size()))!=SE_OK)session.inspectorError="Script.FieldValueInvalid";
            else session.inspectorError.clear();
        };
        struct DisabledScope{explicit DisabledScope(bool value){ImGui::BeginDisabled(value);}~DisabledScope(){ImGui::EndDisabled();}} disabled{playing};
        if(document.contains("Scripts"))for(const auto& script:document.at("Scripts"))
        {
            const auto type=script.at("Type").get<std::string>();
            struct IdScope{explicit IdScope(const char* value){ImGui::PushID(value);}~IdScope(){ImGui::PopID();}} id{type.c_str()};
            ImGui::TextUnformatted(type.c_str());
            bool enabled=script.value("Enabled",true);
            if(ImGui::Checkbox(session.localization.GetText("Script.Enabled").c_str(),&enabled))apply({{"Type",type},{"Enabled",enabled}});
            if(script.contains("Diagnostics"))for(const auto& diagnostic:script.at("Diagnostics"))
                ImGui::TextWrapped("%s: %s",session.localization.GetText(diagnostic.at("Key").get<std::string>()).c_str(),diagnostic.value("Field",std::string{}).c_str());
            else if(script.value("Missing",false))ImGui::TextUnformatted(session.localization.GetText("Script.TypeMissing").c_str());
            for(const auto& field:script.at("Fields"))
            {
                const auto name=field.at("Name").get<std::string>(),kind=field.at("Kind").get<std::string>();
                auto value=field.at("Value");bool changed=false;
                const std::string label=name+" ["+kind+"]";
                if(kind=="i32"){int number=value.get<int>();changed=ImGui::InputInt(label.c_str(),&number);value=number;}
                else if(kind=="f32"){float number=value.get<float>();changed=ImGui::InputFloat(label.c_str(),&number);value=number;}
                else if(kind=="f64"){double number=value.get<double>();changed=ImGui::InputDouble(label.c_str(),&number);value=number;}
                else if(kind=="bool"){bool flag=value.get<bool>();changed=ImGui::Checkbox(label.c_str(),&flag);value=flag;}
                else if(kind=="vec3")
                {float vector[3]={value.at("X").get<float>(),value.at("Y").get<float>(),value.at("Z").get<float>()};changed=ImGui::InputFloat3(label.c_str(),vector);value={{"X",vector[0]},{"Y",vector[1]},{"Z",vector[2]}};}
                else if(kind=="string"||kind=="entity")
                {const auto original=value.is_null()?std::string{}:value.get<std::string>();std::vector<char> buffer(std::max(size_t{8192},original.size()+4097));
                    std::memcpy(buffer.data(),original.data(),original.size());
                    changed=ImGui::InputText(label.c_str(),buffer.data(),buffer.size(),kind=="entity"?ImGuiInputTextFlags_CharsDecimal:0);value=buffer.data();
                    if(kind=="entity"&&ImGui::BeginDragDropTarget())
                    {if(const auto* payload=ImGui::AcceptDragDropPayload("SE_ENTITY"))if(payload->DataSize==sizeof(se::StableEntityId)){se::StableEntityId droppedId;std::memcpy(&droppedId,payload->Data,sizeof(droppedId));value=std::to_string(droppedId);changed=true;}ImGui::EndDragDropTarget();}}
                if(changed)apply({{"Type",type},{"Field",name},{"Value",value}});
            }
            if(ImGui::Button(session.localization.GetText("Script.RemoveBinding").c_str()))apply({{"Type",type},{"Remove",true}});
        }
        if(document.contains("Available")&&ImGui::BeginCombo(session.localization.GetText("Script.AddBinding").c_str(),""))
        {for(const auto& typeValue:document.at("Available")){const auto type=typeValue.get<std::string>();if(ImGui::Selectable(type.c_str()))apply({{"Type",type},{"Add",true}});}ImGui::EndCombo();}
        if(ImGui::Button(session.localization.GetText("Script.SaveBindings").c_str()))apply({{"Save",true}});
        if(!session.inspectorError.empty())ImGui::TextUnformatted(session.localization.GetText(session.inspectorError).c_str());
    }
    catch(...){ImGui::TextUnformatted(session.localization.GetText("Script.MetadataFailed").c_str());}
}
}
static_assert(sizeof(SE_Transform) == 40);
static_assert(sizeof(SE_EventHeader)==88&&sizeof(SE_EventStats)==104);
static_assert(sizeof(SE_BodyState)==72);
static_assert(sizeof(SE_AbiInfo) == 16);
static_assert(sizeof(SE_ModelInfo)==16);
static_assert(sizeof(SE_Vector3)==12 && sizeof(SE_BoxBodyDesc)==32 && sizeof(SE_RayHit)==48);
static_assert(sizeof(SE_ContactEvent)==24 && sizeof(SE_UiStyle)==56 && sizeof(SE_InputState)==772);
static_assert(sizeof(SE_PointerState)==28 && sizeof(SE_CharacterDesc)==20 && sizeof(SE_CharacterState)==20);

int32_t SE_CALL se_get_abi_info(SE_AbiInfo* info) noexcept
{
    if (!info) return Error(SE_INVALID_ARGUMENT, "Bridge.InvalidArgument");
    *info = {1, sizeof(SE_Transform), sizeof(void*), 0};
    return SE_OK;
}
int32_t SE_CALL se_get_last_error(char* buffer, uint32_t capacity, uint32_t* required) noexcept
{
    return CopyText(lastError, buffer, capacity, required);
}
int32_t SE_CALL se_session_create(SE_Session* output) noexcept
{
    return Protect([&]() -> int32_t {
        if (!output) return Error(SE_INVALID_ARGUMENT, "Bridge.InvalidArgument");
        *output = 0;
        auto session = std::make_unique<Session>();
        size_t index = 0;
        for (; index < slots.size(); ++index)
            if (!slots[index].value && slots[index].generation != UINT32_MAX) break;
        if (index == slots.size()) slots.emplace_back();
        const auto created=(static_cast<uint64_t>(slots[index].generation)<<32)|(index+1);
        SceneServices(*session,created);slots[index].value=std::move(session);*output=created;
        return SE_OK;
    });
}
int32_t SE_CALL se_session_close(SE_Session handle) noexcept
{
    return Protect([&]() -> int32_t {
        Session* session = Find(handle, false);
        if (!session) return SE_INVALID_HANDLE;
        if (session->stepping || session->windowRunning) return Error(SE_BUSY, "Bridge.SessionBusy");
        Slot& slot = slots[static_cast<uint32_t>(handle) - 1];
        session->Env().BeginShutdown();
        CloseEventPublishers(handle);
        slot.value.reset();
        if (slot.generation != UINT32_MAX) ++slot.generation;
        return SE_OK;
    });
}
int32_t SE_CALL se_session_set_update(SE_Session handle, SE_UpdateCallback callback, void* context) noexcept
{
    return Protect([&]() -> int32_t {
        Session* session = Find(handle);
        if (!session) return MissingSession();
        if (session->stepping) return Error(SE_BUSY, "Bridge.SessionBusy");
        session->callback = callback;
        session->context = callback ? context : nullptr;
        return SE_OK;
    });
}
int32_t SE_CALL se_session_step(SE_Session handle, double dt) noexcept
{
    return Protect([&]() -> int32_t {
        Session* session = Find(handle);
        if (!session) return MissingSession();
        if (!std::isfinite(dt) || dt < 0.0 || dt > 1.0) return Error(SE_INVALID_ARGUMENT, "Bridge.InvalidDelta");
        return AdvanceSession(*session,handle,dt,false);
    });
}
int32_t SE_CALL se_environment_set_callback(SE_Session handle,SE_TickCallback callback,void* context)noexcept
{
    return Protect([&]()->int32_t{auto* s=Find(handle);if(!s)return MissingSession();if(s->stepping)return Error(SE_BUSY,"Bridge.SessionBusy");s->tickCallback=callback;s->tickContext=callback?context:nullptr;return SE_OK;});
}
int32_t SE_CALL se_environment_configure(SE_Session handle,double seconds,uint32_t maximum,int32_t automatic,int32_t physics)noexcept
{
    return Protect([&]()->int32_t{
        auto* s=Find(handle);if(!s)return MissingSession();if(s->stepping)return Error(SE_BUSY,"Bridge.SessionBusy");
        if(!std::isfinite(seconds)||seconds<=0||seconds>1||!maximum||(automatic!=0&&automatic!=1)||(physics!=0&&physics!=1))return Error(SE_INVALID_ARGUMENT,"Clock.InvalidSettings");
        if(!s->Env().ConfigureClock(seconds,maximum,automatic!=0))return Error(SE_INVALID_ARGUMENT,s->Env().LastError().c_str());
        s->environmentDriven=true;s->environmentPhysics=physics!=0;s->pendingEnvironmentRelease=false;s->Env().SetPaused(false);s->physics.SetEnvironmentDriven(true);s->physicsClock.ClearRemainder();return SE_OK;
    });
}
int32_t SE_CALL se_environment_set_paused(SE_Session handle,int32_t paused)noexcept
{
    return Protect([&]()->int32_t{auto* s=Find(handle);if(!s)return MissingSession();if(paused!=0&&paused!=1)return Error(SE_INVALID_ARGUMENT,"Bridge.InvalidArgument");s->Env().SetPaused(paused!=0);return SE_OK;});
}
int32_t SE_CALL se_environment_release(SE_Session handle)noexcept
{
    return Protect([&]()->int32_t{auto* s=Find(handle);if(!s)return MissingSession();s->pendingEnvironmentRelease=true;if(!s->stepping)FinishEnvironmentRelease(*s);return SE_OK;});
}
int32_t SE_CALL se_environment_stats(SE_Session handle,uint64_t* steps,double* simulation,double* dropped)noexcept
{
    return Protect([&]()->int32_t{auto* s=Find(handle);if(!s)return MissingSession();if(!steps||!simulation||!dropped)return Error(SE_INVALID_ARGUMENT,"Bridge.InvalidArgument");*steps=s->Env().FixedStepIndex();*simulation=s->Env().SimulationSeconds();*dropped=s->Env().Clock().DroppedSeconds();return SE_OK;});
}
int32_t SE_CALL se_tod_snapshot(SE_Session handle,char* buffer,uint32_t capacity,uint32_t* required)noexcept
{
    return Protect([&]()->int32_t{
        auto* s=Find(handle);if(!s)return MissingSession();const auto& environment=s->ReadEnvironment();
        const auto v=environment.Snapshot();const auto vector=[](se::Vec3 a){return nlohmann::json{{"X",a.x},{"Y",a.y},{"Z",a.z}};};
        const nlohmann::json data={{"Active",bool(environment.TimeOfDay().Profile())},{"FrameIndex",v.frameIndex},{"Day",v.day},{"Time",v.time},
            {"SimulationSeconds",v.simulationSeconds},{"Rate",environment.TimeOfDay().State().rate},{"Daylight",v.daylight},
            {"SunDirection",vector(v.sunDirection)},{"SunColor",vector(v.parameters.sunColor)},{"SunIntensity",v.parameters.sunIntensity},
            {"SkyColor",vector(v.parameters.skyColor)},{"FogColor",vector(v.parameters.fogColor)},{"FogDensity",v.parameters.fogDensity}};
        return CopyText(data.dump(),buffer,capacity,required);
    });
}
int32_t SE_CALL se_tod_set_time(SE_Session handle,int64_t day,double time,int32_t replay)noexcept
{
    return Protect([&]()->int32_t{auto* s=Find(handle);if(!s)return MissingSession();
        if(s->editor&&!s->borrowedEnvironment)return Error(SE_BUSY,"Bridge.EditorReadOnly");
        if(replay!=0&&replay!=1)return Error(SE_INVALID_ARGUMENT,"Bridge.InvalidArgument");
        return s->Env().TimeOfDay().SetTime(day,time,replay!=0)?SE_OK:Error(SE_INVALID_ARGUMENT,s->Env().TimeOfDay().LastError().c_str());});
}
int32_t SE_CALL se_tod_set_rate(SE_Session handle,double rate)noexcept
{
    return Protect([&]()->int32_t{auto* s=Find(handle);if(!s)return MissingSession();
        if(s->editor&&!s->borrowedEnvironment)return Error(SE_BUSY,"Bridge.EditorReadOnly");
        return s->Env().TimeOfDay().SetRate(rate)?SE_OK:Error(SE_INVALID_ARGUMENT,s->Env().TimeOfDay().LastError().c_str());});
}
int32_t SE_CALL se_environment_fixed_step(SE_Session handle,double seconds)noexcept
{
    return Protect([&]()->int32_t{
        auto* s=Find(handle);if(!s)return MissingSession();if(s->stepping)return Error(SE_BUSY,"Bridge.SessionBusy");
        if(!s->environmentDriven||!std::isfinite(seconds)||seconds<=0||seconds>1)return Error(SE_INVALID_ARGUMENT,"Tick.ManualFixedRejected");
        struct Guard{Session& s;~Guard(){s.stepping=false;}} guard{*s};s->stepping=true;s->callbackFailed=false;
        const bool ok=s->Env().RunFixed(seconds,EnvironmentHooks(*s,handle));SweepBodies(*s);FinishEnvironmentRelease(*s);
        if(!ok)return s->callbackFailed?Error(SE_CALLBACK_FAILED,"Bridge.CallbackFailed"):Error(SE_INVALID_ARGUMENT,s->Env().LastError().c_str());return SE_OK;
    });
}
int32_t SE_CALL se_event_set_callback(SE_Session handle,SE_EventCallback callback,void* context)noexcept
{
    return Protect([&]()->int32_t{auto* s=Find(handle);if(!s)return MissingSession();if(s->stepping)return Error(SE_BUSY,"Bridge.SessionBusy");s->eventCallback=callback;s->eventContext=callback?context:nullptr;return SE_OK;});
}
int32_t SE_CALL se_event_register_type(SE_Session handle,uint64_t type,uint32_t version,const char* key,uint32_t length,uint32_t maximum)noexcept
{
    return Protect([&]()->int32_t{auto* s=Find(handle);if(!s)return MissingSession();if(!key||!length||length>256||!ValidUtf8(key,length))return Error(SE_INVALID_ARGUMENT,"Event.InvalidType");
        auto& events=EventsFor(*s);return events.RegisterType({type,std::string(key,length),version,maximum})?SE_OK:Error(SE_INVALID_ARGUMENT,events.LastError().c_str());});
}
int32_t SE_CALL se_event_publisher_create(SE_Session handle,uint64_t* publisher)noexcept
{
    return Protect([&]()->int32_t{auto* s=Find(handle);if(!s)return MissingSession();if(!publisher)return Error(SE_INVALID_ARGUMENT,"Bridge.InvalidArgument");*publisher=0;
        auto& events=EventsFor(*s);auto value=events.CreatePublisher();const auto id=value.ChannelId();if(!id)return Error(SE_INVALID_ARGUMENT,events.LastError().c_str());
        eventPublishers.emplace(id,EventPublisher{handle,std::move(value)});*publisher=id;return SE_OK;});
}
int32_t SE_CALL se_event_publisher_close(uint64_t publisher)noexcept
{
    return Protect([&]()->int32_t{auto it=eventPublishers.find(publisher);if(it!=eventPublishers.end()){it->second.value.Close();eventPublishers.erase(it);}return SE_OK;});
}
int32_t SE_CALL se_event_publish(uint64_t publisher,uint64_t type,uint32_t version,uint32_t phase,const uint8_t* bytes,uint32_t length,SE_Entity source,uint64_t stable,SE_Entity target,uint64_t scene,uint32_t* result)noexcept
{
    return Protect([&]()->int32_t{
        if(!result||(!bytes&&length)||length>16*1024*1024||phase>2||(source&&!se::EntityHandle::FromPacked(source))||(target&&!se::EntityHandle::FromPacked(target)))return Error(SE_INVALID_ARGUMENT,"Bridge.InvalidArgument");
        const auto it=eventPublishers.find(publisher);*result=static_cast<uint32_t>(it==eventPublishers.end()?se::EventPostResult::Closed:
            it->second.value.Post(type,version,{bytes,length},static_cast<se::EventPhase>(phase),se::EntityHandle::FromPacked(source),stable,se::EntityHandle::FromPacked(target),scene));return SE_OK;
    });
}
int32_t SE_CALL se_event_subscribe(SE_Session handle,uint64_t publisher,uint64_t component,uint64_t type,SE_Entity target,uint64_t scene,uint64_t* subscription)noexcept
{
    return Protect([&]()->int32_t{
        auto* s=Find(handle);if(!s)return MissingSession();if(!subscription||!s->eventCallback||(target&&!se::EntityHandle::FromPacked(target)))return Error(SE_INVALID_ARGUMENT,"Bridge.InvalidArgument");*subscription=0;
        auto found=eventPublishers.find(publisher);if(found==eventPublishers.end()||found->second.session!=handle)return Error(SE_INVALID_ARGUMENT,"Event.ContextMismatch");
        auto& events=EventsFor(*s);auto token=events.Subscribe(se::ComponentHandle::FromPacked(component),type,[s,handle](const se::EventMessage& message,uint64_t token){
            if(!s->eventCallback)return false;const auto& e=message.envelope;
            const SE_EventHeader header{e.world,e.channel,e.type,e.source?e.source.Pack():0,e.sourceStableId,e.target?e.target.Pack():0,e.scene,e.frame,e.fixedStep,e.sequence,e.version,static_cast<uint32_t>(e.phase)};
            int32_t result=2;
            {struct Unlock{Unlock(){sessionsMutex.unlock();}~Unlock(){sessionsMutex.lock();}} unlock;
                result=s->eventCallback(handle,token,&header,message.payload->data(),static_cast<uint32_t>(message.payload->size()),s->eventContext);}
            if(result==3)throw se::EventPayloadRejected{};if(result!=0&&result!=2)throw std::runtime_error("Event.CallbackFailed");return result==0;
        },&found->second.value,se::EntityHandle::FromPacked(target),scene);
        const auto id=token.Id();if(!id)return Error(SE_INVALID_ARGUMENT,events.LastError().c_str());
        s->eventSubscriptions.emplace(id,std::move(token));*subscription=id;return SE_OK;
    });
}
int32_t SE_CALL se_event_unsubscribe(SE_Session handle,uint64_t subscription)noexcept
{
    return Protect([&]()->int32_t{auto* s=Find(handle);if(!s)return MissingSession();s->eventSubscriptions.erase(subscription);s->Env().Events().Unsubscribe(subscription);return SE_OK;});
}
int32_t SE_CALL se_event_stats(SE_Session handle,SE_EventStats* stats)noexcept
{
    return Protect([&]()->int32_t{auto* s=Find(handle);if(!s)return MissingSession();if(!stats)return Error(SE_INVALID_ARGUMENT,"Bridge.InvalidArgument");const auto v=EventsFor(*s).Statistics();
        *stats={v.accepted,v.delivered,v.rejected,v.droppedTargets,v.noReceiver,v.callbackFailures,v.invalidPayload,v.mailbox.pending,v.mailbox.queued,v.mailbox.bytes,v.mailbox.peakPending,v.mailbox.peakBytes,v.mailbox.closed?1u:0u,0};return SE_OK;});
}
int32_t SE_CALL se_event_trace(SE_Session handle,char* buffer,uint32_t capacity,uint32_t* required)noexcept
{
    return Protect([&]()->int32_t{auto* s=Find(handle);if(!s)return MissingSession();auto rows=nlohmann::json::array();for(const auto& row:EventsFor(*s).Trace()){
        const auto& e=row.envelope;rows.push_back({{"kind",static_cast<uint32_t>(row.kind)},{"world",e.world},{"channel",e.channel},{"type",e.type},{"version",e.version},{"source",e.source.Pack()},{"sourceStableId",e.sourceStableId},{"target",e.target.Pack()},{"scene",e.scene},{"frame",e.frame},{"fixedStep",e.fixedStep},{"phase",static_cast<uint32_t>(e.phase)},{"sequence",e.sequence},{"subscriber",row.subscriber},{"queued",row.queued},{"deliveryFrame",row.deliveryFrame},{"deliveryStep",row.deliveryStep}});}
        return CopyText(rows.dump(),buffer,capacity,required);});
}
int32_t SE_CALL se_component_tick_register(SE_Session handle,uint64_t component,uint32_t stage,int32_t simulation,int32_t paused,uint64_t* task)noexcept
{
    return Protect([&]()->int32_t{
        auto* s=Find(handle);if(!s)return MissingSession();if(!task||stage>5||(simulation!=0&&simulation!=1)||(paused!=0&&paused!=1)||!s->tickCallback)return Error(SE_INVALID_ARGUMENT,"Tick.InvalidDescriptor");
        se::TickDescriptor descriptor;descriptor.key="bridge."+std::to_string(component)+'.'+std::to_string(stage);descriptor.component=se::ComponentHandle::FromPacked(component);
        descriptor.stage=static_cast<se::TickStage>(stage);descriptor.time=simulation?se::TickTimeDomain::Simulation:se::TickTimeDomain::Frame;descriptor.runWhenPaused=paused!=0;
        descriptor.callback=[s,handle,component,stage](const se::TickContext& context){if(EnvironmentCallback(*s,handle,component,stage,context))throw std::runtime_error("Tick.CallbackFailed");};
        *task=s->Env().Ticks().Register(s->WorldRef(),std::move(descriptor));return *task?SE_OK:Error(SE_INVALID_ARGUMENT,s->Env().Ticks().LastError().c_str());
    });
}
int32_t SE_CALL se_component_tick_remove(SE_Session handle,uint64_t task)noexcept
{
    return Protect([&]()->int32_t{auto* s=Find(handle);if(!s)return MissingSession();return s->Env().Ticks().Unregister(task)?SE_OK:Error(SE_INVALID_HANDLE,"Tick.InvalidTask");});
}
int32_t SE_CALL se_component_tick_prerequisites(SE_Session handle,uint64_t task,const SE_TickPrerequisite* values,uint32_t count)noexcept
{
    return Protect([&]()->int32_t{
        auto* s=Find(handle);if(!s)return MissingSession();if(count>256||(count&&!values))return Error(SE_INVALID_ARGUMENT,"Tick.InvalidPrerequisite");
        std::vector<se::TickPrerequisite> prerequisites;
        for(uint32_t i=0;i<count;++i){if(values[i].reserved||values[i].mode>1)return Error(SE_INVALID_ARGUMENT,"Tick.InvalidPrerequisite");prerequisites.push_back({values[i].task,static_cast<se::TickDependencyMode>(values[i].mode)});}
        return s->Env().Ticks().SetPrerequisites(task,std::move(prerequisites))?SE_OK:Error(SE_INVALID_ARGUMENT,s->Env().Ticks().LastError().c_str());
    });
}
int32_t SE_CALL se_entity_is_pending(SE_Session handle,SE_Entity entity,int32_t* pending)noexcept
{return Protect([&]()->int32_t{auto* s=Find(handle);if(!s)return MissingSession();if(!pending)return Error(SE_INVALID_ARGUMENT,"Bridge.InvalidArgument");*pending=s->WorldRef().IsPending(se::EntityHandle::FromPacked(entity))?1:0;return SE_OK;});}
int32_t SE_CALL se_script_component_pending(SE_Session handle,uint64_t component,int32_t* pending)noexcept
{return Protect([&]()->int32_t{auto* s=Find(handle);if(!s)return MissingSession();if(!pending)return Error(SE_INVALID_ARGUMENT,"Bridge.InvalidArgument");*pending=s->WorldRef().IsComponentPending(se::ComponentHandle::FromPacked(component),se::ComponentTypes::Script)?1:0;return SE_OK;});}
int32_t SE_CALL se_session_get_value(SE_Session handle, double* value) noexcept
{
    return Protect([&]() -> int32_t {
        Session* session = Find(handle); if (!session) return MissingSession();
        if (!value) return Error(SE_INVALID_ARGUMENT, "Bridge.InvalidArgument");
        *value = session->value; return SE_OK;
    });
}
int32_t SE_CALL se_session_set_value(SE_Session handle, double value) noexcept
{
    return Protect([&]() -> int32_t {
        Session* session = Find(handle); if (!session) return MissingSession();
        if (!std::isfinite(value)) return Error(SE_INVALID_ARGUMENT, "Bridge.InvalidArgument");
        session->value = value; return SE_OK;
    });
}
int32_t SE_CALL se_session_get_step_count(SE_Session handle, uint64_t* count) noexcept
{
    return Protect([&]() -> int32_t {
        Session* session = Find(handle); if (!session) return MissingSession();
        if (!count) return Error(SE_INVALID_ARGUMENT, "Bridge.InvalidArgument");
        *count = session->stepCount; return SE_OK;
    });
}
int32_t SE_CALL se_session_get_input(SE_Session handle, SE_InputState* input) noexcept
{
    return Protect([&]() -> int32_t {auto* s=Find(handle);if(!s)return MissingSession();
        if(!input)return Error(SE_INVALID_ARGUMENT,"Bridge.InvalidArgument");*input=s->input;return SE_OK;});
}
int32_t SE_CALL se_session_get_pointer(SE_Session handle, SE_PointerState* input) noexcept
{
    return Protect([&]() -> int32_t {auto* s=Find(handle);if(!s)return MissingSession();
        if(!input)return Error(SE_INVALID_ARGUMENT,"Bridge.InvalidArgument");*input=s->pointer;return SE_OK;});
}
int32_t SE_CALL se_session_request_stop(SE_Session handle) noexcept
{
    return Protect([&]() -> int32_t {auto* s=Find(handle);if(!s)return MissingSession();s->stopWindow=true;return SE_OK;});
}
int32_t SE_CALL se_session_set_locale(SE_Session handle, const char* locale, uint32_t length) noexcept
{
    return Protect([&]() -> int32_t {auto* s=Find(handle);if(!s)return MissingSession();
        if(!locale||length!=5||!ValidUtf8(locale,length))return Error(SE_INVALID_ARGUMENT,"Bridge.InvalidArgument");
        const std::string language(locale,length);
        if(language!="zh-CN"&&language!="en-US")return Error(SE_INVALID_ARGUMENT,"Bridge.InvalidArgument");
        const auto directory=ResourceDirectory();std::string error;
        if(directory.empty()||!s->localization.Load(directory/"Localization"/(language+".txt"),error))return Error(SE_INTERNAL_ERROR,"Bridge.LocalizationFailed");
        return SE_OK;});
}
int32_t SE_CALL se_session_text(SE_Session handle, const char* key, uint32_t length, char* buffer, uint32_t capacity, uint32_t* required) noexcept
{
    return Protect([&]() -> int32_t {auto* s=Find(handle);if(!s)return MissingSession();
        if(!key||length>4096||!ValidUtf8(key,length))return Error(SE_INVALID_ARGUMENT,"Bridge.InvalidUtf8");
        return CopyText(s->localization.GetText(std::string_view(key,length)),buffer,capacity,required);});
}
int32_t SE_CALL se_session_set_window_capture(SE_Session handle,const char* path,uint32_t length,uint64_t frame) noexcept
{
    return Protect([&]() -> int32_t {auto* s=Find(handle);if(!s)return MissingSession();
        if(s->windowRunning||s->stepping)return Error(SE_BUSY,"Bridge.SessionBusy");
        if(length==0&&frame==0){s->windowCapture.clear();s->windowCaptureFrame=0;return SE_OK;}
        if(!path||!length||length>32768||!frame||!ValidUtf8(path,length)||std::memchr(path,0,length))return Error(SE_INVALID_ARGUMENT,"Bridge.InvalidArgument");
        std::u8string utf8(length,u8'\0');std::memcpy(utf8.data(),path,length);s->windowCapture=std::filesystem::path(utf8);s->windowCaptureFrame=frame;return SE_OK;});
}
int32_t SE_CALL se_session_set_window_profile(SE_Session handle,const char* path,uint32_t length,uint32_t width,uint32_t height) noexcept
{
    return Protect([&]() -> int32_t {auto* s=Find(handle);if(!s)return MissingSession();
        if(s->windowRunning||s->stepping)return Error(SE_BUSY,"Bridge.SessionBusy");
        if(length==0&&width==0&&height==0){s->windowProfile.clear();s->profileWidth=s->profileHeight=0;return SE_OK;}
        if(!path||!length||length>32768||width<64||height<64||width>8192||height>8192||!ValidUtf8(path,length)||std::memchr(path,0,length))return Error(SE_INVALID_ARGUMENT,"Bridge.InvalidArgument");
        std::u8string utf8(length,u8'\0');std::memcpy(utf8.data(),path,length);auto candidate=std::filesystem::absolute(std::filesystem::path(utf8));
        if(std::filesystem::exists(candidate))return Error(SE_INVALID_ARGUMENT,"Bridge.ProfileOutputExists");
        s->windowProfile=std::move(candidate);s->profileWidth=width;s->profileHeight=height;return SE_OK;});
}
int32_t SE_CALL se_session_set_asset_root(SE_Session handle,const char* root,uint32_t rootLength,const char* cache,uint32_t cacheLength) noexcept
{
    return Protect([&]() -> int32_t {auto* s=Find(handle);if(!s)return MissingSession();
        if(s->windowRunning||s->stepping)return Error(SE_BUSY,"Bridge.SessionBusy");
        if(!root||!cache||rootLength==0||cacheLength==0||rootLength>32768||cacheLength>32768||!ValidUtf8(root,rootLength)||!ValidUtf8(cache,cacheLength))return Error(SE_INVALID_ARGUMENT,"Bridge.InvalidUtf8");
        std::u8string rootUtf8(rootLength,u8'\0'),cacheUtf8(cacheLength,u8'\0');
        std::memcpy(rootUtf8.data(),root,rootLength);std::memcpy(cacheUtf8.data(),cache,cacheLength);
        auto candidate=std::make_unique<se::AssetStore>(std::filesystem::path(rootUtf8),std::filesystem::path(cacheUtf8));
        std::vector<se::AssetDiagnostic> diagnostics;
        if(!candidate->Scan(diagnostics))return Error(SE_INVALID_ARGUMENT,"Bridge.AssetScanFailed");
        s->assets=std::move(candidate);return SE_OK;});
}
int32_t SE_CALL se_assets_list(SE_Session handle,char* output,uint32_t capacity,uint32_t* required) noexcept
{
    return Protect([&]() -> int32_t {auto* s=Find(handle);if(!s)return MissingSession();
        if(!s->assets)return Error(SE_INVALID_ARGUMENT,"Bridge.AssetsNotConfigured");
        return CopyText(nlohmann::json(s->assets->ListIds()).dump(),output,capacity,required);});
}
int32_t SE_CALL se_assets_load_model_info(SE_Session handle,const char* id,uint32_t length,SE_ModelInfo* info) noexcept
{
    return Protect([&]() -> int32_t {auto* s=Find(handle);if(!s)return MissingSession();
        if(!s->assets)return Error(SE_INVALID_ARGUMENT,"Bridge.AssetsNotConfigured");
        if(!id||length!=32||!ValidUtf8(id,length)||!info)return Error(SE_INVALID_ARGUMENT,"Bridge.InvalidArgument");
        std::vector<se::AssetDiagnostic> diagnostics;const auto model=s->assets->LoadModel(std::string(id,length),diagnostics);
        if(!model)return Error(SE_INVALID_ARGUMENT,"Bridge.AssetLoadFailed");
        *info={static_cast<uint32_t>(model->meshes.size()),0,0,0};
        for(const auto& mesh:model->meshes){info->vertices+=static_cast<uint32_t>(mesh.vertices.size());info->indices+=static_cast<uint32_t>(mesh.indices.size());}
        return SE_OK;});
}
int32_t SE_CALL se_session_run_window(SE_Session handle, const char* locale, uint32_t length, int32_t visible, uint64_t maximumFrames) noexcept
{
    return Protect([&]() -> int32_t {
        auto* s=Find(handle);if(!s)return MissingSession();
        if(s->stepping||s->windowRunning)return Error(SE_BUSY,"Bridge.SessionBusy");
        if(!locale||length!=5||!ValidUtf8(locale,length))return Error(SE_INVALID_ARGUMENT,"Bridge.InvalidArgument");
        const std::string language(locale,length);
        if(language!="zh-CN"&&language!="en-US")return Error(SE_INVALID_ARGUMENT,"Bridge.InvalidArgument");
        if(!s->windowProfile.empty()){
            std::filesystem::create_directories(s->windowProfile.parent_path());
            if(!std::filesystem::create_directory(s->windowProfile))return Error(SE_INVALID_ARGUMENT,"Bridge.ProfileOutputExists");
        }
        struct WindowGuard{Session& s;~WindowGuard(){s.windowRunning=false;s.input={};s.pointer={};}} guard{*s};
        s->windowRunning=true;s->stopWindow=false;
        se::ApplicationServices services;services.ui=&s->UiRef();services.maximumFrames=maximumFrames;services.advanceUi=false;
        se::Profiler profiler(se::WindowProfileRenderer::FrameLimit);
        if(!s->windowProfile.empty()){
            profiler.SetEnabled(true);services.profiler=&profiler;
            services.initialWidth=s->profileWidth;services.initialHeight=s->profileHeight;
        }
        services.resourceDirectory=ResourceDirectory();
        if(services.resourceDirectory.empty())return Error(SE_INTERNAL_ERROR,"Bridge.WindowFailed");
        services.input=[s](const InputState& input,bool consumed,bool pointerConsumed){CaptureInput(*s,input,consumed,pointerConsumed);};
        services.runEnvironmentFrame=[s,handle,p=services.profiler](double dt){se::CpuScope scope(p,"CPU.Environment");return !s->stopWindow&&se_session_step(handle,dt)==SE_OK&&!s->stopWindow;};
        services.extract=[s](se::FrameData& data){se::BuildFrameData(s->WorldRef(),&s->UiRef(),s->physicsDebugDraw?&s->physics:nullptr,data);data.assets=s->assets.get();
            if(s->Env().TimeOfDay().Profile())data.timeOfDay=s->Env().Snapshot();
            if(s->physicsDebugDraw){for(const auto& item:s->characters){const auto lines=item.second.DebugLines();data.debugLines.insert(data.debugLines.end(),lines.begin(),lines.end());}}};
        SandboxRenderer renderer(SandboxScene::Scene,services.resourceDirectory);
        if(!s->windowCapture.empty())renderer.SetCaptureOutput(s->windowCapture,s->windowCaptureFrame);
        std::unique_ptr<se::WindowProfileRenderer> measured;
        if(!s->windowProfile.empty())measured=std::make_unique<se::WindowProfileRenderer>(renderer,profiler,s->windowProfile);
        Renderer& activeRenderer=measured?static_cast<Renderer&>(*measured):static_cast<Renderer&>(renderer);
        int exitCode=1;
        {
            struct HostUnlock{HostUnlock(){sessionsMutex.unlock();}~HostUnlock(){sessionsMutex.lock();}} unlock;
            exitCode=RunSandboxApp(language,activeRenderer,visible!=0,&services);
        }
        if(measured&&!measured->Finish(exitCode==0||s->stopWindow))return Error(SE_INTERNAL_ERROR,"Bridge.ProfileFailed");
        if(exitCode!=0&&!s->stopWindow)return Error(SE_INTERNAL_ERROR,"Bridge.WindowFailed");
        if(!renderer.CaptureSucceeded())return Error(SE_INTERNAL_ERROR,"Sample.CaptureMissing");
        return SE_OK;
    });
}
int32_t SE_CALL se_entity_create(SE_Session handle, const char* name, uint32_t length, SE_Entity* entity) noexcept
{
    return Protect([&]() -> int32_t {
        Session* session = Find(handle); if (!session) return MissingSession();
        if (!entity || length > 4096 || !ValidUtf8(name, length)) return Error(SE_INVALID_ARGUMENT, "Bridge.InvalidUtf8");
        const auto created = session->WorldRef().Create(length == 0 ? std::string{} : std::string(name, length));
        if (!created) return Error(SE_INVALID_ARGUMENT, "Bridge.WorldOperationFailed");
        *entity = created.Pack();
        return SE_OK;
    });
}
int32_t SE_CALL se_session_set_editor_callback(SE_Session handle, SE_EditorCallback callback, void* context) noexcept
{
    return Protect([&]() -> int32_t {auto* s=Find(handle);if(!s)return MissingSession();
        if(s->stepping||s->windowRunning)return Error(SE_BUSY,"Bridge.SessionBusy");
        s->editorCallback=callback;s->editorContext=callback?context:nullptr;return SE_OK;});
}
int32_t SE_CALL se_editor_set_playing(SE_Session handle, int32_t playing) noexcept
{
    return Protect([&]() -> int32_t {auto* s=Find(handle);if(!s)return MissingSession();
        if(!s->editor||playing<0||playing>1)return Error(SE_INVALID_ARGUMENT,"Bridge.EditorNotRunning");
        s->pendingPlay=playing;return SE_OK;});
}
int32_t SE_CALL se_session_set_editor_data(SE_Session handle,SE_EditorDataCallback callback,void* context) noexcept
{
    return Protect([&]() -> int32_t {auto* s=Find(handle);if(!s)return MissingSession();if(s->windowRunning||s->stepping)return Error(SE_BUSY,"Bridge.SessionBusy");
        s->editorDataCallback=callback;s->editorDataContext=callback?context:nullptr;return SE_OK;});
}
int32_t SE_CALL se_editor_inspector_query(SE_Session handle,uint64_t entity,char* output,uint32_t capacity,uint32_t* required) noexcept
{
    return Protect([&]() -> int32_t {auto* s=Find(handle);if(!s)return MissingSession();
        if(!s->editor||!s->editorDataCallback)return Error(SE_INVALID_ARGUMENT,"Bridge.EditorNotRunning");
        if(!required||(!output&&capacity))return Error(SE_INVALID_ARGUMENT,"Bridge.InvalidArgument");
        const bool prior=s->stepping;s->stepping=true;
        struct Restore{Session& s;bool prior;~Restore(){sessionsMutex.lock();s.stepping=prior;}} restore{*s,prior};
        sessionsMutex.unlock();const auto result=s->editorDataCallback(handle,entity,nullptr,0,output,capacity,required,s->editorDataContext);
        return result==SE_OK||result==SE_BUFFER_TOO_SMALL?result:Error(SE_CALLBACK_FAILED,"Script.MetadataFailed");});
}
int32_t SE_CALL se_editor_inspector_apply(SE_Session handle,uint64_t entity,const char* request,uint32_t length) noexcept
{
    return Protect([&]() -> int32_t {auto* s=Find(handle);if(!s)return MissingSession();
        if(!s->editor||!s->editorDataCallback)return Error(SE_INVALID_ARGUMENT,"Bridge.EditorNotRunning");
        if(s->editor->IsPlaying())return Error(SE_BUSY,"Bridge.EditorReadOnly");
        if(!request||length>1024*1024||!ValidUtf8(request,length))return Error(SE_INVALID_ARGUMENT,"Bridge.InvalidUtf8");
        const bool prior=s->stepping;s->stepping=true;
        struct Restore{Session& s;bool prior;~Restore(){sessionsMutex.lock();s.stepping=prior;}} restore{*s,prior};
        sessionsMutex.unlock();uint32_t required=0;const auto result=s->editorDataCallback(handle,entity,request,length,nullptr,0,&required,s->editorDataContext);
        return result==SE_OK||result==SE_BUFFER_TOO_SMALL?SE_OK:Error(SE_CALLBACK_FAILED,"Script.FieldValueInvalid");});
}
int32_t SE_CALL se_session_run_editor(SE_Session handle,const char* root,uint32_t rootLength,const char* locale,uint32_t localeLength,int32_t visible,uint64_t maximumFrames,int32_t startPlaying) noexcept
{
    return Protect([&]() -> int32_t {
        auto* s=Find(handle);if(!s)return MissingSession();
        if(s->stepping||s->windowRunning)return Error(SE_BUSY,"Bridge.SessionBusy");
        if(!root||rootLength==0||rootLength>32768||!ValidUtf8(root,rootLength)||!locale||localeLength!=5||!ValidUtf8(locale,localeLength))return Error(SE_INVALID_ARGUMENT,"Bridge.InvalidArgument");
        const std::string language(locale,localeLength);
        if(language!="zh-CN"&&language!="en-US")return Error(SE_INVALID_ARGUMENT,"Bridge.InvalidArgument");
        const auto resources=ResourceDirectory();
        const auto sceneFile=EditorEnvironmentPath(L"SELFENGINE_EDITOR_SCENE_FILE"),uiFile=EditorEnvironmentPath(L"SELFENGINE_EDITOR_UI_FILE");
        std::string localizationError;s->localization.Load(resources/"Localization"/(language+".txt"),localizationError);
        struct EditorGuard{Session& s;~EditorGuard(){s.editor=nullptr;s.borrowedWorld=nullptr;s.borrowedEnvironment=nullptr;s.borrowedUi=nullptr;s.windowRunning=false;s.input={};s.pointer={};}} guard{*s};
        s->windowRunning=true;s->stopWindow=false;s->pendingPlay=-1;
        bool callbackFailed=false;
        auto clearRuntime=[s]{for(auto body:s->physics.Bodies())s->physics.DestroyBody(body);s->bodyOwners.clear();s->characters.clear();s->physicsClock.Reset();s->contactEvents.clear();s->uiEvents.clear();s->environmentDriven=s->environmentPhysics=s->pendingEnvironmentRelease=false;s->physics.SetEnvironmentDriven(false);};
        se::EditorApplicationHooks hooks;hooks.resourceDirectory=resources;hooks.startPlaying=startPlaying!=0;
        hooks.initialize=[s,sceneFile](se::EditorSession& editor,se::UiTree& ui){
            s->editor=&editor;ui=s->ui;
            if(!sceneFile.empty())
            {if(!sceneFile.is_absolute()||!editor.Open(sceneFile,true))return false;}
            else if(!editor.Apply("Bridge.EditorInitialize",[s](se::World& world){world=s->world;return true;}))return false;
            const auto entities=editor.EditWorld().Entities();if(!entities.empty())editor.Select(editor.EditWorld().Get(entities.front())->stableId);return true;
        };
        hooks.configure=[s,sceneFile,uiFile](se::ImGuiEditor& frontend){frontend.SetDocumentPaths(s->editor?s->editor->Path():sceneFile,uiFile);};
        if(s->editorDataCallback)
        {
            hooks.externalDocuments.snapshot=[s,handle](std::vector<se::EditorExternalDocument>& output,std::string& error){
                nlohmann::json value;
                if(!EditorDocumentRequest(handle,*s,{{"Document","Snapshot"}},true,value,error))return false;
                output.clear();
                if(!value.contains("Schema"))return true; // 兼容只实现旧 Inspector 元数据的宿主。
                try
                {
                    if(value.at("Schema")!=1||value.at("Id")!="script-bindings"||value.at("Kind")!="bindings"
                        ||!value.at("Revision").is_number_unsigned()){error="Script.DocumentInvalid";return false;}
                    se::EditorExternalDocument item;
                    item.document.documentId=value.at("Id").get<std::string>();item.document.kind=value.at("Kind").get<std::string>();
                    item.document.relativePath=value.at("RelativePath").get<std::string>();item.document.content=value.at("Content").get<std::string>();
                    item.document.sourceDigest=value.at("SourceDigest").get<std::string>();item.document.revision=value.at("Revision").get<std::uint64_t>();
                    item.dirty=value.at("Dirty").get<bool>();output.push_back(std::move(item));return true;
                }
                catch(const std::exception&){error="Script.DocumentInvalid";return false;}
            };
            hooks.externalDocuments.save=[s,handle](const std::string& id,std::string& error){
                if(id!="script-bindings"){error="Script.DocumentInvalid";return false;}
                nlohmann::json response;return EditorDocumentRequest(handle,*s,{{"Document","Save"},{"AllowOverwrite",false}},false,response,error);
            };
            hooks.externalDocuments.restore=[s,handle](const se::RecoveryDocument& document,bool apply,std::string& error){
                if(document.documentId!="script-bindings"||document.kind!="bindings"){error="Script.DocumentInvalid";return false;}
                nlohmann::json response;
                return EditorDocumentRequest(handle,*s,{{"Document",apply?"Restore":"ValidateRestore"},{"Content",document.content},
                    {"RelativePath",document.relativePath},{"SourceDigest",document.sourceDigest}},false,response,error);
            };
        }
        hooks.playStarted=[&](se::World& world,se::UiTree& ui){
            clearRuntime();s->borrowedWorld=&world;s->borrowedEnvironment=s->editor->PlayEnvironment();s->borrowedUi=&ui;
            CloseEventPublishers(handle);s->eventSubscriptions.clear();SceneServices(*s,handle);
            const bool ok=!s->editorCallback||s->editorCallback(handle,0,s->editorContext)==0;
            callbackFailed|=!ok;return ok;
        };
        hooks.playStopping=[&](se::World&,se::UiTree&){
            if(s->editorCallback&&s->editorCallback(handle,1,s->editorContext)!=0)callbackFailed=true;
            clearRuntime();s->borrowedWorld=nullptr;s->borrowedEnvironment=nullptr;s->borrowedUi=nullptr;
            CloseEventPublishers(handle);s->eventSubscriptions.clear();
        };
        hooks.input=[s](const InputState& input,bool keyboard,bool pointer){CaptureInput(*s,input,keyboard,pointer);};
        hooks.frame=[s](se::EditorSession& editor,double){
            if(s->stopWindow)return false;
            if(s->pendingPlay!=-1){const auto requested=s->pendingPlay;s->pendingPlay=-1;
                if(requested==0)editor.StopPlay();else if(!editor.IsPlaying()&&!editor.StartPlay())return false;}
            return true;
        };
        hooks.update=[&](se::World&,se::UiTree&,double seconds){
            const bool ok=!s->stopWindow&&se_session_step(handle,seconds)==SE_OK&&!s->stopWindow;
            callbackFailed|=s->callbackFailed;
            if(s->pendingPlay==0&&s->editor){s->pendingPlay=-1;s->editor->StopPlay();}
            return ok;
        };
        hooks.singleStep=[&](se::World&,se::UiTree&){
            const bool ok=!s->stopWindow&&Protect([&]()->int32_t{
                auto* current=Find(handle);if(!current)return MissingSession();
                return AdvanceSession(*current,handle,current->Env().Clock().Step(),true);
            })==SE_OK&&!s->stopWindow;
            callbackFailed|=s->callbackFailed;
            if(s->pendingPlay==0&&s->editor){s->pendingPlay=-1;s->editor->StopPlay();}
            return ok;
        };
        hooks.extract=[s](se::World& world,se::UiTree& ui,se::FrameData& frame){
            se::BuildFrameData(world,&ui,s->physicsDebugDraw?&s->physics:nullptr,frame);
            if(s->Env().TimeOfDay().Profile())frame.timeOfDay=s->Env().Snapshot();
            if(s->physicsDebugDraw){for(const auto& item:s->characters){const auto lines=item.second.DebugLines();frame.debugLines.insert(frame.debugLines.end(),lines.begin(),lines.end());}}
        };
        hooks.inspector=[s,handle](uint64_t entity,bool playing){DrawScriptInspector(handle,*s,entity,playing);};
        hooks.closing=[s](se::EditorSession& editor,se::UiTree& ui){s->environment.Clear();s->world=editor.EditWorld();s->environment.Scenes().SynchronizeIdle();s->ui=ui;s->editor=nullptr;};
        SandboxRenderer renderer(SandboxScene::Scene,resources);int exitCode=1;
        const auto captureFile=EditorEnvironmentPath(L"SELFENGINE_EDITOR_CAPTURE_FILE");
        if(!captureFile.empty())
        {
            const auto captureFrame=EditorEnvironmentPath(L"SELFENGINE_EDITOR_CAPTURE_FRAME").wstring();
            renderer.SetCaptureOutput(captureFile,captureFrame.empty()?30ull:std::stoull(captureFrame));
        }
        {
            struct HostUnlock{HostUnlock(){sessionsMutex.unlock();}~HostUnlock(){sessionsMutex.lock();}} unlock;
            std::u8string rootUtf8(rootLength,u8'\0');std::memcpy(rootUtf8.data(),root,rootLength);
            exitCode=se::RunEditorApp(renderer,language,std::filesystem::path(rootUtf8),visible==0,maximumFrames,&hooks);
        }
        if(callbackFailed)return Error(SE_CALLBACK_FAILED,"Bridge.CallbackFailed");
        return (exitCode==0||s->stopWindow)&&renderer.CaptureSucceeded()?SE_OK:Error(SE_INTERNAL_ERROR,"Bridge.WindowFailed");
    });
}
int32_t SE_CALL se_entity_destroy(SE_Session handle, SE_Entity entity) noexcept
{
    return Protect([&]() -> int32_t {
        Session* session = Find(handle); if (!session) return MissingSession();
        if (!session->WorldRef().Destroy(se::EntityHandle::FromPacked(entity))) return Error(SE_INVALID_HANDLE, "Bridge.InvalidEntity");
        SweepBodies(*session);
        return SE_OK;
    });
}
int32_t SE_CALL se_entity_is_alive(SE_Session handle, SE_Entity entity, int32_t* alive) noexcept
{
    return Protect([&]() -> int32_t {
        Session* session = Find(handle); if (!session) return MissingSession();
        if (!alive) return Error(SE_INVALID_ARGUMENT, "Bridge.InvalidArgument");
        *alive = session->ReadWorld().IsAlive(se::EntityHandle::FromPacked(entity)) ? 1 : 0;
        return SE_OK;
    });
}
int32_t SE_CALL se_entity_get_enabled(SE_Session handle,SE_Entity entity,int32_t* enabled)noexcept
{
    return Protect([&]() -> int32_t {auto* s=Find(handle);if(!s)return MissingSession();if(!enabled)return Error(SE_INVALID_ARGUMENT,"Bridge.InvalidArgument");const auto* value=s->ReadWorld().Get(se::EntityHandle::FromPacked(entity));if(!value)return Error(SE_INVALID_HANDLE,"Bridge.InvalidEntity");*enabled=value->enabled?1:0;return SE_OK;});
}
int32_t SE_CALL se_entity_set_enabled(SE_Session handle,SE_Entity entity,int32_t enabled)noexcept
{
    return Protect([&]() -> int32_t {auto* s=Find(handle);if(!s)return MissingSession();if(enabled!=0&&enabled!=1)return Error(SE_INVALID_ARGUMENT,"Bridge.InvalidArgument");if(s->editor&&!s->borrowedWorld)return Error(SE_BUSY,"Bridge.EditorReadOnly");return s->WorldRef().SetEnabled(se::EntityHandle::FromPacked(entity),enabled!=0)?SE_OK:Error(SE_INVALID_HANDLE,"Bridge.InvalidEntity");});
}
int32_t SE_CALL se_script_component_attach(SE_Session handle,SE_Entity owner,uint64_t controlled,int32_t enabled,uint64_t* component)noexcept
{
    return Protect([&]() -> int32_t {auto* s=Find(handle);if(!s)return MissingSession();if(!component||!controlled||(enabled!=0&&enabled!=1))return Error(SE_INVALID_ARGUMENT,"Bridge.InvalidArgument");if(s->editor&&!s->borrowedWorld)return Error(SE_BUSY,"Bridge.EditorReadOnly");const auto value=s->WorldRef().AttachScriptComponent(se::EntityHandle::FromPacked(owner),controlled,{},enabled!=0);if(!value)return Error(SE_INVALID_ARGUMENT,s->WorldRef().LastError().c_str());*component=value.Pack();return SE_OK;});
}
int32_t SE_CALL se_script_component_remove(SE_Session handle,uint64_t component)noexcept
{
    return Protect([&]() -> int32_t {auto* s=Find(handle);if(!s)return MissingSession();const auto value=se::ComponentHandle::FromPacked(component);if(!s->WorldRef().GetScriptComponent(value)&&!s->WorldRef().IsComponentPending(value,se::ComponentTypes::Script))return Error(SE_INVALID_HANDLE,"Bridge.InvalidComponent");return s->WorldRef().RemoveComponent(value)?SE_OK:Error(SE_INVALID_HANDLE,"Bridge.InvalidComponent");});
}
int32_t SE_CALL se_script_component_set_enabled(SE_Session handle,uint64_t component,int32_t enabled)noexcept
{
    return Protect([&]() -> int32_t {auto* s=Find(handle);if(!s)return MissingSession();if(enabled!=0&&enabled!=1)return Error(SE_INVALID_ARGUMENT,"Bridge.InvalidArgument");const auto value=se::ComponentHandle::FromPacked(component);if(!s->WorldRef().GetScriptComponent(value)&&!s->WorldRef().IsComponentPending(value,se::ComponentTypes::Script))return Error(SE_INVALID_HANDLE,"Bridge.InvalidComponent");return s->WorldRef().SetComponentEnabled(value,enabled!=0)?SE_OK:Error(SE_INVALID_ARGUMENT,s->WorldRef().LastError().c_str());});
}
int32_t SE_CALL se_script_component_state(SE_Session handle,uint64_t component,int32_t* alive,int32_t* active)noexcept
{
    return Protect([&]() -> int32_t {auto* s=Find(handle);if(!s)return MissingSession();if(!alive||!active)return Error(SE_INVALID_ARGUMENT,"Bridge.InvalidArgument");const auto* value=s->WorldRef().GetScriptComponent(se::ComponentHandle::FromPacked(component));*alive=value?1:0;*active=value&&value->Active()&&value->Enabled()&&s->WorldRef().IsEnabled(value->Owner())?1:0;return SE_OK;});
}
int32_t SE_CALL se_script_component_count(SE_Session handle,uint32_t* count)noexcept
{
    return Protect([&]() -> int32_t {auto* s=Find(handle);if(!s)return MissingSession();if(!count)return Error(SE_INVALID_ARGUMENT,"Bridge.InvalidArgument");*count=0;const auto& world=s->WorldRef();for(const auto owner:world.Entities())for(const auto value:world.Components(owner))if(world.GetComponent(value)->Type()==se::ComponentTypes::Script)++*count;return SE_OK;});
}
int32_t SE_CALL se_entity_get_transform(SE_Session handle, SE_Entity entity, SE_Transform* output) noexcept
{
    return Protect([&]() -> int32_t {
        Session* session = Find(handle); if (!session) return MissingSession();
        if (!output) return Error(SE_INVALID_ARGUMENT, "Bridge.InvalidArgument");
        const auto* t = session->ReadWorld().GetTransform(se::EntityHandle::FromPacked(entity));
        if (!t) return Error(SE_INVALID_HANDLE, "Bridge.InvalidEntity");
        *output = {t->position.x,t->position.y,t->position.z,t->rotation.x,t->rotation.y,t->rotation.z,t->rotation.w,t->scale.x,t->scale.y,t->scale.z};
        return SE_OK;
    });
}
int32_t SE_CALL se_entity_set_transform(SE_Session handle, SE_Entity entity, const SE_Transform* value) noexcept
{
    return Protect([&]() -> int32_t {
        Session* session = Find(handle); if (!session) return MissingSession();
        if (!value || !ValidTransform(*value)) return Error(SE_INVALID_ARGUMENT, "Bridge.InvalidTransform");
        const auto* record=session->WorldRef().Get(se::EntityHandle::FromPacked(entity));
        if (!record&&!session->WorldRef().IsPending(se::EntityHandle::FromPacked(entity))) return Error(SE_INVALID_HANDLE,"Bridge.InvalidEntity");
        if (record&&HasBody(*session,record->stableId)) return Error(SE_BUSY,"Bridge.PhysicsOwnsTransform");
        if(session->characters.contains(entity))return Error(SE_BUSY,"Bridge.CharacterOwnsTransform");
        const se::Transform transform{{value->px,value->py,value->pz},{value->qx,value->qy,value->qz,value->qw},{value->sx,value->sy,value->sz}};
        return session->WorldRef().SetTransform(se::EntityHandle::FromPacked(entity), transform) ? SE_OK
            : Error(SE_INVALID_HANDLE, "Bridge.InvalidEntity");
    });
}
int32_t SE_CALL se_entity_get_stable_id(SE_Session handle, SE_Entity entity, uint64_t* stableId) noexcept
{
    return Protect([&]() -> int32_t {
        Session* session = Find(handle); if (!session) return MissingSession();
        if (!stableId) return Error(SE_INVALID_ARGUMENT, "Bridge.InvalidArgument");
        const auto* record = session->ReadWorld().Get(se::EntityHandle::FromPacked(entity));
        if (!record) return Error(SE_INVALID_HANDLE, "Bridge.InvalidEntity");
        *stableId = record->stableId; return SE_OK;
    });
}
int32_t SE_CALL se_entity_find(SE_Session handle, uint64_t stableId, SE_Entity* entity) noexcept
{
    return Protect([&]() -> int32_t {
        Session* session = Find(handle); if (!session) return MissingSession();
        if (!entity) return Error(SE_INVALID_ARGUMENT, "Bridge.InvalidArgument");
        const auto found = session->ReadWorld().FindByStableId(stableId);
        if (!session->ReadWorld().IsAlive(found)) return Error(SE_INVALID_HANDLE, "Bridge.InvalidEntity");
        *entity = found.Pack(); return SE_OK;
    });
}
int32_t SE_CALL se_entity_get_name(SE_Session handle, SE_Entity entity, char* buffer, uint32_t capacity, uint32_t* required) noexcept
{
    return Protect([&]() -> int32_t {
        Session* session = Find(handle); if (!session) return MissingSession();
        const auto* record = session->ReadWorld().Get(se::EntityHandle::FromPacked(entity));
        if (!record) return Error(SE_INVALID_HANDLE, "Bridge.InvalidEntity");
        return CopyText(record->name, buffer, capacity, required);
    });
}
int32_t SE_CALL se_scene_export(SE_Session handle, char* buffer, uint32_t capacity, uint32_t* required) noexcept
{
    return Protect([&]() -> int32_t {
        Session* session = Find(handle); if (!session) return MissingSession();
        std::string text, error;
        if (!se::SerializeScene(session->ReadWorld(), text, error)) return Error(SE_INTERNAL_ERROR, "Bridge.SceneInvalid");
        return CopyText(text, buffer, capacity, required);
    });
}
int32_t SE_CALL se_scene_import(SE_Session handle, const char* text, uint32_t length) noexcept
{
    return Protect([&]() -> int32_t {
        Session* session = Find(handle); if (!session) return MissingSession();
        if (session->stepping) return Error(SE_BUSY, "Bridge.SessionBusy");
        if (!text || length == 0 || length > 64u*1024u*1024u || !ValidUtf8(text, length))
            return Error(SE_INVALID_ARGUMENT, "Bridge.InvalidUtf8");
        std::string error;
        if (!se::DeserializeScene(std::string_view(text,length), session->WorldRef(), error)) return Error(SE_INVALID_ARGUMENT, "Bridge.SceneInvalid");
        CloseEventPublishers(handle);session->eventSubscriptions.clear();
        session->Env().Events().ResetWorld(session->WorldRef());
        session->Env().Scenes().ResetWorld(session->WorldRef());
        auto& events=EventsFor(*session);
        const std::uint8_t state[8]={1,0,0,0,2,0,0,0};
        events.GetPublisher().Post(4,1,state,se::EventPhase::NextFrame);
        for (auto body:session->physics.Bodies()) session->physics.DestroyBody(body);
        session->bodyOwners.clear();
        session->contactEvents.clear(); session->physicsClock.Reset();
        session->characters.clear();
        return SE_OK;
    });
}

int32_t SE_CALL se_scene_set_callback(SE_Session handle,SE_SceneCallback callback,void* context)noexcept
{
    return Protect([&]()->int32_t{auto* s=Find(handle);if(!s)return MissingSession();if(s->stepping)return Error(SE_BUSY,"Bridge.SessionBusy");s->sceneCallback=callback;s->sceneContext=callback?context:nullptr;return SE_OK;});
}
int32_t SE_CALL se_scene_create(SE_Session handle,const char* name,uint32_t length,uint64_t* scene)noexcept
{
    return Protect([&]()->int32_t{auto* s=Find(handle);if(!s)return MissingSession();if(!scene||!name||!length||length>256||!ValidUtf8(name,length))return Error(SE_INVALID_ARGUMENT,"Bridge.InvalidArgument");
        auto& scenes=ScenesFor(*s);*scene=scenes.Create(std::string(name,length)).value;return *scene?SE_OK:Error(SE_INVALID_ARGUMENT,scenes.LastError().c_str());});
}
int32_t SE_CALL se_scene_request_file(SE_Session handle,uint64_t scene,const char* path,uint32_t length,uint32_t mode,uint64_t replacing,uint64_t* request)noexcept
{
    return Protect([&]()->int32_t{auto* s=Find(handle);if(!s)return MissingSession();if(!request||!path||!length||length>32768||mode>1||!ValidUtf8(path,length))return Error(SE_INVALID_ARGUMENT,"Bridge.InvalidArgument");
        auto& scenes=ScenesFor(*s);*request=scenes.RequestFile({scene},std::filesystem::path(std::u8string(path,path+length)),static_cast<se::SceneLoadMode>(mode),{replacing});return *request?SE_OK:Error(SE_INVALID_ARGUMENT,scenes.LastError().c_str());});
}
int32_t SE_CALL se_scene_request_json(SE_Session handle,uint64_t scene,const char* text,uint32_t length,uint32_t mode,uint64_t replacing,uint64_t* request)noexcept
{
    return Protect([&]()->int32_t{auto* s=Find(handle);if(!s)return MissingSession();if(!request||!text||!length||length>16*1024*1024||mode>1||!ValidUtf8(text,length))return Error(SE_INVALID_ARGUMENT,"Bridge.InvalidArgument");
        se::SceneDocument document;std::string error;if(!se::ParseSceneDocument({text,length},document,error))return Error(SE_INVALID_ARGUMENT,"Bridge.SceneInvalid");
        auto& scenes=ScenesFor(*s);*request=scenes.RequestDocument({scene},std::move(document),static_cast<se::SceneLoadMode>(mode),{replacing});return *request?SE_OK:Error(SE_INVALID_ARGUMENT,scenes.LastError().c_str());});
}
int32_t SE_CALL se_scene_cancel(SE_Session handle,uint64_t scene)noexcept
{return Protect([&]()->int32_t{auto* s=Find(handle);if(!s)return MissingSession();auto& scenes=ScenesFor(*s);return scenes.Cancel({scene})?SE_OK:Error(SE_INVALID_ARGUMENT,scenes.LastError().c_str());});}
int32_t SE_CALL se_scene_unload(SE_Session handle,uint64_t scene)noexcept
{return Protect([&]()->int32_t{auto* s=Find(handle);if(!s)return MissingSession();auto& scenes=ScenesFor(*s);return scenes.Unload({scene})?SE_OK:Error(SE_INVALID_ARGUMENT,scenes.LastError().c_str());});}
int32_t SE_CALL se_scene_active(SE_Session handle,uint64_t* scene)noexcept
{return Protect([&]()->int32_t{auto* s=Find(handle);if(!s)return MissingSession();if(!scene)return Error(SE_INVALID_ARGUMENT,"Bridge.InvalidArgument");*scene=ScenesFor(*s).ActiveScene().value;return SE_OK;});}
int32_t SE_CALL se_scene_of_entity(SE_Session handle,SE_Entity entity,uint64_t* scene)noexcept
{return Protect([&]()->int32_t{auto* s=Find(handle);if(!s)return MissingSession();if(!scene)return Error(SE_INVALID_ARGUMENT,"Bridge.InvalidArgument");*scene=ScenesFor(*s).SceneOf(se::EntityHandle::FromPacked(entity)).value;return SE_OK;});}
int32_t SE_CALL se_scene_create_entity(SE_Session handle,uint64_t scene,const char* name,uint32_t length,SE_Entity* entity)noexcept
{
    return Protect([&]()->int32_t{auto* s=Find(handle);if(!s)return MissingSession();if(!entity||!name||length>4096||!ValidUtf8(name,length))return Error(SE_INVALID_ARGUMENT,"Bridge.InvalidArgument");
        auto& scenes=ScenesFor(*s);const auto created=scenes.CreateEntity({scene},std::string(name,length));*entity=created?created.Pack():0;return created?SE_OK:Error(SE_INVALID_ARGUMENT,scenes.LastError().c_str());});
}
int32_t SE_CALL se_scene_get(SE_Session handle,uint64_t scene,char* buffer,uint32_t capacity,uint32_t* required)noexcept
{
    return Protect([&]()->int32_t{auto* s=Find(handle);if(!s)return MissingSession();const auto value=ScenesFor(*s).Inspect({scene});if(!value.handle)return Error(SE_INVALID_HANDLE,"Scene.InvalidRequest");
        auto members=nlohmann::json::array();for(const auto entity:value.members)members.push_back(entity.Pack());
        const nlohmann::json json{{"Id",value.handle.value},{"State",static_cast<uint32_t>(value.state)},{"Active",value.active},{"Request",value.request},{"Revision",value.revision},{"Scope",value.scope},{"Name",value.name},{"Source",value.source},{"RequestedSource",value.requestedSource},{"Error",value.error},{"Members",std::move(members)}};
        return CopyText(json.dump(),buffer,capacity,required);});
}
int32_t SE_CALL se_scene_stats(SE_Session handle,char* buffer,uint32_t capacity,uint32_t* required)noexcept
{
    return Protect([&]()->int32_t{auto* s=Find(handle);if(!s)return MissingSession();const auto v=ScenesFor(*s).Statistics();
        return CopyText(nlohmann::json{{"Requested",v.requested},{"Activated",v.activated},{"Unloaded",v.unloaded},{"Failed",v.failed},{"Cancelled",v.cancelled},{"StaleResults",v.staleResults},{"CallbackFaults",v.callbackFaults},{"NotificationFailures",v.notificationFailures},{"PendingJobs",v.pendingJobs},{"Records",v.records},{"Members",v.members}}.dump(),buffer,capacity,required);});
}
int32_t SE_CALL se_scene_callback_scope(SE_Session handle,uint64_t scene,uint64_t* previous)noexcept
{
    return Protect([&]()->int32_t{auto* s=Find(handle);if(!s)return MissingSession();if(!previous)return Error(SE_INVALID_ARGUMENT,"Bridge.InvalidArgument");auto& scenes=ScenesFor(*s);se::SceneHandle prior;
        if(!scenes.ExchangeCreationScope({scene},prior))return Error(SE_INVALID_ARGUMENT,scenes.LastError().c_str());*previous=prior.value;return SE_OK;});
}
int32_t SE_CALL se_body_create(SE_Session handle, SE_Entity entity, const SE_BoxBodyDesc* desc, SE_Body* output) noexcept
{
    return Protect([&]() -> int32_t {
        auto* s=Find(handle); if(!s) return MissingSession();
        if(!desc||!output) return Error(SE_INVALID_ARGUMENT,"Bridge.InvalidArgument");
        const auto nativeEntity=se::EntityHandle::FromPacked(entity);
        const auto* record=s->WorldRef().Get(nativeEntity);
        const auto* transform=s->WorldRef().GetTransform(nativeEntity);
        if(!record||!transform) return Error(SE_INVALID_HANDLE,"Bridge.InvalidEntity");
        if(record->parent||HasBody(*s,record->stableId)||s->characters.contains(entity)) return Error(SE_INVALID_ARGUMENT,"Bridge.InvalidBodyAttachment");
        se::BodyDesc value;
        value.shape.center=transform->position; value.shape.rotation=transform->rotation;
        value.shape.halfExtents={desc->half_extents.x*std::abs(transform->scale.x),desc->half_extents.y*std::abs(transform->scale.y),desc->half_extents.z*std::abs(transform->scale.z)};
        value.mass=desc->mass; value.friction=desc->friction; value.restitution=desc->restitution;
        value.layer=desc->layer; value.mask=desc->mask; value.entityStableId=record->stableId;
        auto body=s->physics.CreateBody(value);
        if(!s->physics.IsAlive(body)) return Error(SE_INVALID_ARGUMENT,"Bridge.PhysicsFailed");
        try{s->bodyOwners.emplace(body.Pack(),nativeEntity);}catch(...){s->physics.DestroyBody(body);throw;}
        *output=body.Pack(); return SE_OK;
    });
}
int32_t SE_CALL se_body_destroy(SE_Session handle, SE_Body body) noexcept
{
    return Protect([&]() -> int32_t {auto* s=Find(handle); if(!s)return MissingSession();
        if(!s->physics.DestroyBody(BodyFromPacked(body)))return Error(SE_INVALID_HANDLE,"Bridge.InvalidBody");s->bodyOwners.erase(body);return SE_OK;});
}
int32_t SE_CALL se_body_get_state(SE_Session handle,SE_Body body,SE_BodyState* output) noexcept
{
    return Protect([&]() -> int32_t {
        auto* s=Find(handle);if(!s)return MissingSession();if(!output)return Error(SE_INVALID_ARGUMENT,"Bridge.InvalidArgument");
        const auto* state=s->physics.GetBody(BodyFromPacked(body));if(!state||!BodyAlive(*s,body))return Error(SE_INVALID_HANDLE,"Bridge.InvalidBody");
        const auto& world=s->ReadWorld();const auto* transform=world.GetTransform(world.FindByStableId(state->description.entityStableId));
        if(!transform)return Error(SE_INVALID_HANDLE,"Bridge.InvalidEntity");
        const auto& p=state->description.shape.center;const auto& q=state->description.shape.rotation;const auto& scale=transform->scale;
        const auto& linear=state->linearVelocity;const auto& angular=state->angularVelocity;
        *output={{p.x,p.y,p.z,q.x,q.y,q.z,q.w,scale.x,scale.y,scale.z},{linear.x,linear.y,linear.z},{angular.x,angular.y,angular.z},state->description.mass,state->sleeping?1:0};return SE_OK;
    });
}
int32_t SE_CALL se_body_set_velocity(SE_Session handle, SE_Body body, SE_Vector3 linear, SE_Vector3 angular) noexcept
{
    return Protect([&]() -> int32_t {auto* s=Find(handle); if(!s)return MissingSession();
        if(!BodyAlive(*s,body))return Error(SE_INVALID_HANDLE,"Bridge.InvalidBody");
        return s->physics.SetVelocity(BodyFromPacked(body),Vector(linear),Vector(angular))?SE_OK:Error(SE_INVALID_ARGUMENT,"Bridge.PhysicsFailed");});
}
int32_t SE_CALL se_body_apply_force(SE_Session handle, SE_Body body, SE_Vector3 force, SE_Vector3 point, int32_t impulse) noexcept
{
    return Protect([&]() -> int32_t {auto* s=Find(handle); if(!s)return MissingSession();
        if(!BodyAlive(*s,body))return Error(SE_INVALID_HANDLE,"Bridge.InvalidBody");
        const bool ok=impulse?s->physics.ApplyImpulse(BodyFromPacked(body),Vector(force),Vector(point)):s->physics.ApplyForce(BodyFromPacked(body),Vector(force),Vector(point));
        return ok?SE_OK:Error(SE_INVALID_ARGUMENT,"Bridge.PhysicsFailed");});
}
int32_t SE_CALL se_body_teleport(SE_Session handle, SE_Body body, const SE_Transform* value, int32_t clear) noexcept
{
    return Protect([&]() -> int32_t {auto* s=Find(handle); if(!s)return MissingSession();
        if(!value||!ValidTransform(*value))return Error(SE_INVALID_ARGUMENT,"Bridge.InvalidTransform");
        const auto* state=s->physics.GetBody(BodyFromPacked(body));
        if(!state)return Error(SE_INVALID_HANDLE,"Bridge.InvalidBody");
        auto entity=s->WorldRef().FindByStableId(state->description.entityStableId);
        const auto* old=s->WorldRef().GetTransform(entity);
        if(!old)return Error(SE_INVALID_HANDLE,"Bridge.InvalidEntity");
        if(old->scale.x!=value->sx||old->scale.y!=value->sy||old->scale.z!=value->sz)return Error(SE_INVALID_ARGUMENT,"Bridge.BodyScaleChanged");
        auto transform=*old;
        transform.position={value->px,value->py,value->pz}; transform.rotation={value->qx,value->qy,value->qz,value->qw};
        if(!s->physics.Teleport(BodyFromPacked(body),transform.position,transform.rotation,clear!=0))return Error(SE_INVALID_ARGUMENT,"Bridge.PhysicsFailed");
        return s->WorldRef().SetTransform(entity,transform)?SE_OK:Error(SE_INTERNAL_ERROR,"Bridge.WorldOperationFailed");});
}
int32_t SE_CALL se_physics_set_debug_draw(SE_Session handle,int32_t enabled,int32_t* previous) noexcept
{
    return Protect([&]() -> int32_t {auto* s=Find(handle);if(!s)return MissingSession();
        if(!previous||(enabled!=0&&enabled!=1))return Error(SE_INVALID_ARGUMENT,"Bridge.InvalidArgument");
        *previous=s->physicsDebugDraw?1:0;s->physicsDebugDraw=enabled!=0;return SE_OK;});
}
int32_t SE_CALL se_physics_step(SE_Session handle, double dt) noexcept
{
    return Protect([&]() -> int32_t {auto* s=Find(handle);if(!s)return MissingSession();if(s->environmentDriven)return Error(SE_BUSY,"Script.PhysicsClockOwned");return PhysicsStep(*s,dt);});
}
int32_t SE_CALL se_physics_advance(SE_Session handle, double dt, uint32_t* steps, double* dropped) noexcept
{
    return Protect([&]() -> int32_t {auto* s=Find(handle); if(!s)return MissingSession();
        if(!steps||!dropped||!std::isfinite(dt)||dt<0||dt>1)return Error(SE_INVALID_ARGUMENT,"Bridge.InvalidDelta");
        if(s->environmentDriven)return Error(SE_BUSY,"Script.PhysicsClockOwned");
        int32_t result=SE_OK;
        *steps=static_cast<uint32_t>(s->physicsClock.Advance(dt,[&](double fixed){if(result==SE_OK)result=PhysicsStep(*s,fixed);}));
        *dropped=s->physicsClock.DroppedSeconds(); return result;});
}
int32_t SE_CALL se_physics_raycast(SE_Session handle, SE_Vector3 origin, SE_Vector3 direction, float distance, uint32_t mask, SE_RayHit* output, int32_t* found) noexcept
{
    return Protect([&]() -> int32_t {auto* s=Find(handle); if(!s)return MissingSession();
        if(!output||!found||!std::isfinite(distance)||distance<=0)return Error(SE_INVALID_ARGUMENT,"Bridge.InvalidArgument");
        *output={}; *found=0;
        const auto hit=s->physics.Raycast(Vector(origin),Vector(direction),distance,mask);
        if(hit){*found=1; *output={hit->body.Pack(),hit->entityStableId,{hit->point.x,hit->point.y,hit->point.z},{hit->normal.x,hit->normal.y,hit->normal.z},hit->distance,0};}
        return SE_OK;});
}
int32_t SE_CALL se_physics_drain_contacts(SE_Session handle, SE_ContactEvent* output, uint32_t capacity, uint32_t* required) noexcept
{
    return Protect([&]() -> int32_t {auto* s=Find(handle); if(!s)return MissingSession();
        if(!required||(!output&&capacity!=0))return Error(SE_INVALID_ARGUMENT,"Bridge.InvalidArgument");
        *required=static_cast<uint32_t>(s->contactEvents.size());
        if(capacity<*required)return SE_BUFFER_TOO_SMALL;
        if(!s->contactEvents.empty())std::copy(s->contactEvents.begin(),s->contactEvents.end(),output);
        s->contactEvents.clear(); return SE_OK;});
}
int32_t SE_CALL se_character_create(SE_Session handle, SE_Entity entity, const SE_CharacterDesc* desc) noexcept
{
    return Protect([&]() -> int32_t {auto* s=Find(handle);if(!s)return MissingSession();
        if(!desc)return Error(SE_INVALID_ARGUMENT,"Bridge.InvalidArgument");
        const auto* record=s->WorldRef().Get(se::EntityHandle::FromPacked(entity));const auto* transform=s->WorldRef().GetTransform(se::EntityHandle::FromPacked(entity));
        if(!record||!transform)return Error(SE_INVALID_HANDLE,"Bridge.InvalidEntity");
        if(record->parent||HasBody(*s,record->stableId)||s->characters.contains(entity))return Error(SE_INVALID_ARGUMENT,"Bridge.InvalidBodyAttachment");
        if(!std::isfinite(desc->radius)||desc->radius<=0||!std::isfinite(desc->segment_half_height)||desc->segment_half_height<0
            ||!std::isfinite(desc->walk_speed)||desc->walk_speed<0||!std::isfinite(desc->jump_speed)||desc->jump_speed<0
            ||!std::isfinite(desc->maximum_push_impulse)||desc->maximum_push_impulse<0)return Error(SE_INVALID_ARGUMENT,"Bridge.InvalidCharacter");
        se::CharacterController value;value.position=transform->position;value.radius=desc->radius;value.segmentHalfHeight=desc->segment_half_height;
        value.walkSpeed=desc->walk_speed;value.jumpSpeed=desc->jump_speed;value.maximumPushImpulse=desc->maximum_push_impulse;
        s->characters.emplace(entity,value);return SE_OK;});
}
int32_t SE_CALL se_character_move(SE_Session handle, SE_Entity entity, SE_Vector3 direction, int32_t jump, float seconds, SE_CharacterState* state) noexcept
{
    return Protect([&]() -> int32_t {auto* s=Find(handle);if(!s)return MissingSession();
        if(!state)return Error(SE_INVALID_ARGUMENT,"Bridge.InvalidArgument");
        const auto found=s->characters.find(entity);const auto* transform=s->WorldRef().GetTransform(se::EntityHandle::FromPacked(entity));
        if(found==s->characters.end()||!transform)return Error(SE_INVALID_HANDLE,"Bridge.InvalidCharacter");
        auto& value=found->second;
        if(!value.Move(s->physics,Vector(direction),jump!=0,seconds))return Error(SE_INVALID_ARGUMENT,"Bridge.PhysicsFailed");
        auto updated=*transform;updated.position=value.position;
        if(!s->WorldRef().SetTransform(se::EntityHandle::FromPacked(entity),updated))return Error(SE_INTERNAL_ERROR,"Bridge.WorldOperationFailed");
        *state={{value.position.x,value.position.y,value.position.z},value.verticalVelocity,value.grounded?1:0};return SE_OK;});
}
int32_t SE_CALL se_character_teleport(SE_Session handle, SE_Entity entity, SE_Vector3 position) noexcept
{
    return Protect([&]() -> int32_t {auto* s=Find(handle);if(!s)return MissingSession();
        if(!std::isfinite(position.x)||!std::isfinite(position.y)||!std::isfinite(position.z))return Error(SE_INVALID_ARGUMENT,"Bridge.InvalidTransform");
        const auto found=s->characters.find(entity);const auto* transform=s->WorldRef().GetTransform(se::EntityHandle::FromPacked(entity));
        if(found==s->characters.end()||!transform)return Error(SE_INVALID_HANDLE,"Bridge.InvalidCharacter");
        auto& value=found->second;value.position=Vector(position);value.verticalVelocity=0;value.grounded=false;
        auto updated=*transform;updated.position=value.position;
        return s->WorldRef().SetTransform(se::EntityHandle::FromPacked(entity),updated)?SE_OK:Error(SE_INTERNAL_ERROR,"Bridge.WorldOperationFailed");});
}
int32_t SE_CALL se_character_destroy(SE_Session handle, SE_Entity entity) noexcept
{
    return Protect([&]() -> int32_t {auto* s=Find(handle);if(!s)return MissingSession();return s->characters.erase(entity)?SE_OK:Error(SE_INVALID_HANDLE,"Bridge.InvalidCharacter");});
}

int32_t SE_CALL se_ui_create(SE_Session handle, int32_t kind, uint64_t parent, uint64_t* output) noexcept
{
    return Protect([&]() -> int32_t {auto* s=Find(handle);if(!s)return MissingSession();
        if(!output||kind<0||kind>8||(parent&&!s->UiRef().Get(parent)))return Error(SE_INVALID_ARGUMENT,"Bridge.InvalidUi");
        *output=s->UiRef().Create(static_cast<se::UiKind>(kind),parent);return *output?SE_OK:Error(SE_INVALID_ARGUMENT,"Bridge.InvalidUi");});
}
int32_t SE_CALL se_ui_remove(SE_Session handle, uint64_t id) noexcept
{
    return Protect([&]() -> int32_t {auto* s=Find(handle);if(!s)return MissingSession();return s->UiRef().Remove(id)?SE_OK:Error(SE_INVALID_HANDLE,"Bridge.InvalidUi");});
}
int32_t SE_CALL se_ui_set_style(SE_Session handle, uint64_t id, const SE_UiStyle* style) noexcept
{
    return Protect([&]() -> int32_t {auto* s=Find(handle);if(!s)return MissingSession();
        if(!style||style->layout<0||style->layout>2)return Error(SE_INVALID_ARGUMENT,"Bridge.InvalidUi");
        se::UiStyle value;
        value.anchorMin={style->anchor_min_x,style->anchor_min_y};value.anchorMax={style->anchor_max_x,style->anchor_max_y};
        value.pivot={style->pivot_x,style->pivot_y};value.offset={style->offset_x,style->offset_y};value.size={style->width,style->height};
        value.padding=style->padding;value.spacing=style->spacing;value.layout=static_cast<se::UiLayout>(style->layout);value.clip=style->clip!=0;
        return s->UiRef().SetStyle(id,value)?SE_OK:Error(SE_INVALID_ARGUMENT,"Bridge.InvalidUi");});
}
int32_t SE_CALL se_ui_set_text(SE_Session handle, uint64_t id, const char* text, uint32_t length) noexcept
{
    return Protect([&]() -> int32_t {auto* s=Find(handle);if(!s)return MissingSession();
        if(length>1024*1024||!ValidUtf8(text,length))return Error(SE_INVALID_ARGUMENT,"Bridge.InvalidUtf8");
        return s->UiRef().SetText(id,std::string_view(text?text:"",length))?SE_OK:Error(SE_INVALID_HANDLE,"Bridge.InvalidUi");});
}
int32_t SE_CALL se_ui_set_property(SE_Session handle, uint64_t id, int32_t property, double value) noexcept
{
    return Protect([&]() -> int32_t {auto* s=Find(handle);if(!s)return MissingSession();
        if(!std::isfinite(value))return Error(SE_INVALID_ARGUMENT,"Bridge.InvalidUi");
        bool ok=false;
        switch(property){case 0:ok=s->UiRef().SetValue(id,value);break;case 1:ok=s->UiRef().SetVisible(id,value!=0);break;
        case 2:ok=s->UiRef().SetEnabled(id,value!=0);break;case 3:ok=s->UiRef().Focus(id);break;
        case 4:ok=(value==0||value==1)&&s->UiRef().SetBlocksPointer(id,value!=0);break;case 5:ok=s->UiRef().SetStep(id,value);break;
        case 6:ok=value==1&&s->UiRef().CancelTransitions(id);break;default:break;}
        return ok?SE_OK:Error(SE_INVALID_ARGUMENT,"Bridge.InvalidUi");});
}
int32_t SE_CALL se_ui_set_range(SE_Session handle, uint64_t id, double minimum, double maximum) noexcept
{
    return Protect([&]() -> int32_t {auto* s=Find(handle);if(!s)return MissingSession();return s->UiRef().SetRange(id,minimum,maximum)?SE_OK:Error(SE_INVALID_ARGUMENT,"Bridge.InvalidUi");});
}
int32_t SE_CALL se_ui_set_image(SE_Session handle, uint64_t id, const char* asset, uint32_t length) noexcept
{
    return Protect([&]() -> int32_t {auto* s=Find(handle);if(!s)return MissingSession();
        if(!asset||length>4096||!ValidUtf8(asset,length))return Error(SE_INVALID_ARGUMENT,"Bridge.InvalidUtf8");
        return s->UiRef().SetImage(id,std::string(asset,length))?SE_OK:Error(SE_INVALID_HANDLE,"Bridge.InvalidUi");});
}
int32_t SE_CALL se_ui_exists(SE_Session handle, uint64_t id, int32_t* exists) noexcept
{
    return Protect([&]() -> int32_t {auto* s=Find(handle);if(!s)return MissingSession();if(!exists)return Error(SE_INVALID_ARGUMENT,"Bridge.InvalidArgument");
        *exists=s->UiRef().Get(id)?1:0;return SE_OK;});
}
int32_t SE_CALL se_ui_bind_action(SE_Session handle,uint64_t id,int32_t eventKind,uint64_t entity,
    const char* script,uint32_t scriptLength,const char* action,uint32_t actionLength) noexcept
{
    return Protect([&]() -> int32_t {auto* s=Find(handle);if(!s)return MissingSession();const auto node=s->UiRef().Get(id);
        if(!node||eventKind<0||eventKind>5)return Error(SE_INVALID_ARGUMENT,"Bridge.InvalidUi");
        const bool clear=entity==0&&scriptLength==0&&actionLength==0;
        if(!clear&&(!script||!action||scriptLength>128||actionLength>128||!ValidUtf8(script,scriptLength)||!ValidUtf8(action,actionLength)))return Error(SE_INVALID_ARGUMENT,"Bridge.InvalidUi");
        auto bindings=node->bindings;const auto event=static_cast<se::UiEventKind>(eventKind);
        const auto old=std::find_if(bindings.begin(),bindings.end(),[&](const auto& binding){return binding.event==event;});
        if(clear){if(old!=bindings.end())bindings.erase(old);}
        else
        {
            se::UiEventBinding value{event,entity,std::string(script,scriptLength),std::string(action,actionLength)};
            if(old==bindings.end())bindings.push_back(std::move(value));else *old=std::move(value);
        }
        return s->UiRef().SetBindings(id,bindings)?SE_OK:Error(SE_INVALID_ARGUMENT,"Bridge.InvalidUi");});
}
int32_t SE_CALL se_ui_read_bindings(SE_Session handle,uint64_t id,char* buffer,uint32_t capacity,uint32_t* required) noexcept
{
    return Protect([&]() -> int32_t {auto* s=Find(handle);if(!s)return MissingSession();const auto node=s->UiRef().Get(id);
        if(!node)return Error(SE_INVALID_HANDLE,"Bridge.InvalidUi");auto values=nlohmann::json::array();
        for(const auto& binding:node->bindings)values.push_back(BindingJson(binding));return CopyText(values.dump(),buffer,capacity,required);});
}
int32_t SE_CALL se_ui_fade(SE_Session handle, uint64_t id, float opacity, double seconds) noexcept
{
    return Protect([&]() -> int32_t {auto* s=Find(handle);if(!s)return MissingSession();
        if(!s->UiRef().Get(id))return Error(SE_INVALID_HANDLE,"Bridge.InvalidUi");
        if(!std::isfinite(opacity)||opacity<0||opacity>1||!std::isfinite(seconds)||seconds<0)return Error(SE_INVALID_ARGUMENT,"Bridge.InvalidUi");
        s->UiRef().FadeTo(id,opacity,seconds);return SE_OK;});
}
int32_t SE_CALL se_ui_move(SE_Session handle,uint64_t id,float x,float y,double seconds) noexcept
{
    return Protect([&]() -> int32_t {auto* s=Find(handle);if(!s)return MissingSession();
        if(!s->UiRef().Get(id))return Error(SE_INVALID_HANDLE,"Bridge.InvalidUi");
        if(!std::isfinite(x)||!std::isfinite(y)||std::abs(x)>1000000||std::abs(y)>1000000||!std::isfinite(seconds)||seconds<0)return Error(SE_INVALID_ARGUMENT,"Bridge.InvalidUi");
        s->UiRef().MoveTo(id,{x,y},seconds);return SE_OK;});
}
int32_t SE_CALL se_ui_color(SE_Session handle,uint64_t id,float r,float g,float b,float a,double seconds) noexcept
{
    return Protect([&]() -> int32_t {auto* s=Find(handle);if(!s)return MissingSession();
        if(!s->UiRef().Get(id))return Error(SE_INVALID_HANDLE,"Bridge.InvalidUi");
        if(!std::isfinite(seconds)||seconds<0)return Error(SE_INVALID_ARGUMENT,"Bridge.InvalidUi");
        for(float v:{r,g,b,a})if(!std::isfinite(v)||v<0||v>1)return Error(SE_INVALID_ARGUMENT,"Bridge.InvalidUi");
        s->UiRef().ColorTo(id,{r,g,b,a},seconds);return SE_OK;});
}
int32_t SE_CALL se_ui_layout(SE_Session handle, float width, float height) noexcept
{
    return Protect([&]() -> int32_t {auto* s=Find(handle);if(!s)return MissingSession();
        if(!std::isfinite(width)||!std::isfinite(height)||width<0||height<0)return Error(SE_INVALID_ARGUMENT,"Bridge.InvalidUi");
        s->UiRef().Layout(width,height);return SE_OK;});
}
int32_t SE_CALL se_ui_pointer(SE_Session handle, int32_t operation, float x, float y, float wheel, int32_t* consumed) noexcept
{
    return Protect([&]() -> int32_t {auto* s=Find(handle);if(!s)return MissingSession();
        if(!consumed||!std::isfinite(x)||!std::isfinite(y)||!std::isfinite(wheel))return Error(SE_INVALID_ARGUMENT,"Bridge.InvalidUi");
        bool result=false;switch(operation){case 0:result=s->UiRef().PointerMove({x,y});break;case 1:result=s->UiRef().PointerDown({x,y});break;
        case 2:result=s->UiRef().PointerUp({x,y});break;case 3:result=s->UiRef().Wheel({x,y},wheel);break;case 4:s->UiRef().CancelInput();break;default:return Error(SE_INVALID_ARGUMENT,"Bridge.InvalidUi");}
        *consumed=result?1:0;return SE_OK;});
}
int32_t SE_CALL se_ui_key(SE_Session handle, uint32_t key, int32_t shift, int32_t control, int32_t* consumed) noexcept
{
    return Protect([&]() -> int32_t {auto* s=Find(handle);if(!s)return MissingSession();if(!consumed||key>255)return Error(SE_INVALID_ARGUMENT,"Bridge.InvalidUi");
        *consumed=s->UiRef().EditKey(key,shift!=0,control!=0)?1:0;return SE_OK;});
}
int32_t SE_CALL se_ui_input_text(SE_Session handle, const char* text, uint32_t length, int32_t composition, int32_t* consumed) noexcept
{
    return Protect([&]() -> int32_t {auto* s=Find(handle);if(!s)return MissingSession();
        if(!consumed||length>1024*1024||!ValidUtf8(text,length))return Error(SE_INVALID_ARGUMENT,"Bridge.InvalidUtf8");
        const std::string_view value(text?text:"",length);
        *consumed=(composition?s->UiRef().SetComposition(value):s->UiRef().InsertText(value))?1:0;return SE_OK;});
}
int32_t SE_CALL se_ui_consumes_keyboard(SE_Session handle, int32_t* consumed) noexcept
{
    return Protect([&]() -> int32_t {auto* s=Find(handle);if(!s)return MissingSession();if(!consumed)return Error(SE_INVALID_ARGUMENT,"Bridge.InvalidArgument");
        *consumed=s->UiRef().ConsumesKeyboard()?1:0;return SE_OK;});
}
int32_t SE_CALL se_ui_read_json(SE_Session handle, int32_t kind, char* buffer, uint32_t capacity, uint32_t* required) noexcept
{
    return Protect([&]() -> int32_t {auto* s=Find(handle);if(!s)return MissingSession();
        std::string text;
        if(kind==0)text=s->UiRef().SaveJson();
        else if(kind==1)
        {
            auto values=nlohmann::json::array();
            for(const auto& v:s->UiRef().Visuals())values.push_back({{"Id",v.id},{"Kind",static_cast<int>(v.kind)},
                {"X",v.rectangle.x},{"Y",v.rectangle.y},{"Width",v.rectangle.width},{"Height",v.rectangle.height},
                {"ClipX",v.clip.x},{"ClipY",v.clip.y},{"ClipWidth",v.clip.width},{"ClipHeight",v.clip.height},
                {"Text",v.text},{"ImageAsset",v.imageAsset},{"Composition",v.composition},{"Value",v.value},
                {"Opacity",v.opacity},{"Hovered",v.hovered},{"Pressed",v.pressed},{"Focused",v.focused},{"Enabled",v.enabled},
                {"Caret",v.caret},{"SelectionAnchor",v.selectionAnchor},{"CompositionCaret",v.compositionCaret==std::string::npos?0:v.compositionCaret}});
            text=values.dump();
        }
        else if(kind==2)
        {
            auto fresh=s->UiRef().TakeEvents();s->uiEvents.insert(s->uiEvents.end(),fresh.begin(),fresh.end());
            auto values=nlohmann::json::array();
            for(const auto& e:s->uiEvents)
            {
                const auto node=s->UiRef().Get(e.target);if(!node)continue;
                nlohmann::json value{{"Kind",static_cast<int>(e.kind)},{"Target",e.target},{"Value",e.value},{"Text",e.text}};
                for(const auto& binding:node->bindings)if(binding.event==e.kind){value["Binding"]=BindingJson(binding);break;}
                values.push_back(std::move(value));
            }
            text=values.dump();
        }
        else return Error(SE_INVALID_ARGUMENT,"Bridge.InvalidUi");
        const auto result=CopyText(text,buffer,capacity,required);
        if(result==SE_OK&&kind==2)s->uiEvents.clear();return result;});
}
int32_t SE_CALL se_ui_load_json(SE_Session handle, const char* text, uint32_t length) noexcept
{
    return Protect([&]() -> int32_t {auto* s=Find(handle);if(!s)return MissingSession();
        if(!text||length>16*1024*1024||!ValidUtf8(text,length))return Error(SE_INVALID_ARGUMENT,"Bridge.InvalidUtf8");
        std::string error;if(!s->UiRef().LoadJson(std::string_view(text,length),error))return Error(SE_INVALID_ARGUMENT,"Bridge.InvalidUi");
        s->uiEvents.clear();return SE_OK;});
}
