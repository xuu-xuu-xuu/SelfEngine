#include "FunctionLayer/Scripting/NativeBridge/EngineNative.h"
#include <Windows.h>
#include <cmath>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <stdexcept>
#include <vector>

namespace
{
void Check(bool value,const char* message){if(!value)throw std::runtime_error(message);}
struct Context {SE_Entity original=0,current=0;std::uint64_t stable=0;unsigned calls=0,documentCalls=0;bool valid=true;};
int32_t SE_CALL Metadata(SE_Session session,uint64_t stable,const char* request,uint32_t requestLength,char* output,uint32_t capacity,uint32_t* required,void* user)
{
    auto& context=*static_cast<Context*>(user);
    if(stable==0&&request&&requestLength)
    {
        ++context.documentCalls;
        constexpr char document[]=R"({"Schema":1,"Id":"script-bindings","Kind":"bindings","RelativePath":"Scripts/Bindings.json","Content":"{\"Version\":1,\"Scripts\":[]}","SourceDigest":"","Revision":0,"Dirty":false})";
        if(!required)return SE_INVALID_ARGUMENT;*required=sizeof(document);if(capacity<sizeof(document))return SE_BUFFER_TOO_SMALL;
        if(!output)return SE_INVALID_ARGUMENT;std::memcpy(output,document,sizeof(document));return SE_OK;
    }
    ++context.calls;SE_Entity current=0;int32_t oldAlive=1;uint64_t resolved=0;char name[64]{};uint32_t length=0;SE_Transform transform{};
    context.valid=context.valid&&stable==context.stable&&se_entity_find(session,stable,&current)==SE_OK&&current!=context.original
        &&se_entity_is_alive(session,context.original,&oldAlive)==SE_OK&&oldAlive==0&&se_entity_get_stable_id(session,current,&resolved)==SE_OK&&resolved==stable
        &&se_entity_get_name(session,current,name,sizeof(name),&length)==SE_OK&&std::strcmp(name,"editor source")==0
        &&se_entity_get_transform(session,current,&transform)==SE_OK&&transform.px==3&&transform.py==2&&transform.pz==1;
    context.current=current;constexpr char document[]="{\"Scripts\":[],\"Available\":[]}";
    if(!required)return SE_INVALID_ARGUMENT;*required=sizeof(document);if(capacity<sizeof(document))return SE_BUFFER_TOO_SMALL;
    if(!output)return SE_INVALID_ARGUMENT;std::memcpy(output,document,sizeof(document));return SE_OK;
}
struct Session {SE_Session value=0;~Session(){if(value)se_session_close(value);}};
struct ScopedEnvironment
{
    std::wstring name,previous;bool existed=false;
    ScopedEnvironment(const wchar_t* variable,const std::filesystem::path& value):name(variable)
    {
        const auto size=GetEnvironmentVariableW(variable,nullptr,0);existed=size!=0;
        if(existed){previous.resize(size);const auto length=GetEnvironmentVariableW(variable,previous.data(),size);previous.resize(length);}
        Check(SetEnvironmentVariableW(variable,value.empty()?nullptr:value.c_str())!=FALSE,"set test environment");
    }
    ~ScopedEnvironment(){SetEnvironmentVariableW(name.c_str(),existed?previous.c_str():nullptr);}
};
std::string ReadUi(SE_Session session)
{
    uint32_t length=0;Check(se_ui_read_json(session,0,nullptr,0,&length)==SE_BUFFER_TOO_SMALL,"UI snapshot size");
    std::vector<char> text(length);Check(se_ui_read_json(session,0,text.data(),length,&length)==SE_OK,"UI snapshot");return text.data();
}
struct PausedContext{unsigned updates=0,frameTicks=0,fixedTicks=0,controls=0;};
int32_t SE_CALL CountUpdate(SE_Session,double,void* user)
{++static_cast<PausedContext*>(user)->updates;return 0;}
int32_t SE_CALL CountTick(SE_Session,uint64_t component,uint32_t phase,double,uint64_t,uint64_t,void* user)
{
    if(component){auto& state=*static_cast<PausedContext*>(user);if(phase==2)++state.fixedTicks;if(phase==4)++state.frameTicks;if(phase==0)++state.controls;}return 0;
}
void PausedBridge()
{
    Session session;PausedContext context;Check(se_session_create(&session.value)==SE_OK,"paused bridge session");
    SE_Entity entity=0;Check(se_entity_create(session.value,"body",4,&entity)==SE_OK,"paused bridge entity");
    const SE_Transform pose{0,10,0,0,0,0,1,1,1,1};Check(se_entity_set_transform(session.value,entity,&pose)==SE_OK,"paused body pose");
    SE_Body body=0;const SE_BoxBodyDesc description{{.5f,.5f,.5f},1,.6f,0,1,UINT32_MAX};
    Check(se_body_create(session.value,entity,&description,&body)==SE_OK,"paused real physics body");
    uint64_t component=0,frameTask=0,fixedTask=0,controlTask=0,panel=0;
    Check(se_environment_set_callback(session.value,CountTick,&context)==SE_OK
        &&se_script_component_attach(session.value,entity,1,1,&component)==SE_OK
        &&se_component_tick_register(session.value,component,4,0,0,&frameTask)==SE_OK
        &&se_component_tick_register(session.value,component,2,1,0,&fixedTask)==SE_OK
        &&se_component_tick_register(session.value,component,0,0,1,&controlTask)==SE_OK,"bridge component callbacks registered");
    Check(se_session_set_update(session.value,CountUpdate,&context)==SE_OK
        &&se_environment_configure(session.value,.02,8,1,1)==SE_OK,"bridge automatic physics clock");
    Check(se_ui_create(session.value,0,0,&panel)==SE_OK&&se_ui_fade(session.value,panel,0,1)==SE_OK,"UI transition fixture");
    Check(se_session_step(session.value,.02)==SE_OK&&context.updates==1&&context.frameTicks==1&&context.fixedTicks==1&&context.controls==1,"normal bridge frame advances callbacks");
    const auto uiBefore=ReadUi(session.value);SE_BodyState bodyBefore{};
    Check(se_body_get_state(session.value,body,&bodyBefore)==SE_OK&&bodyBefore.transform.py<10,"normal bridge frame advances physics");
    Check(se_environment_set_paused(session.value,1)==SE_OK,"pause bridge");
    for(unsigned frame=0;frame<3;++frame)Check(se_session_step(session.value,.2)==SE_OK,"paused bridge display frame");
    uint64_t steps=0;double simulation=0,dropped=0;SE_BodyState bodyAfter{};
    Check(se_environment_stats(session.value,&steps,&simulation,&dropped)==SE_OK&&steps==1&&std::abs(simulation-.02)<1e-12
        &&context.updates==4&&context.controls==4&&context.frameTicks==1&&context.fixedTicks==1,"runtime pause keeps session and control callbacks but holds normal scripts and simulation clock");
    Check(ReadUi(session.value)!=uiBefore&&se_body_get_state(session.value,body,&bodyAfter)==SE_OK
        &&bodyAfter.transform.py==bodyBefore.transform.py,"runtime pause animates UI while holding real physics");
    Check(se_environment_set_paused(session.value,0)==SE_OK&&se_session_step(session.value,.02)==SE_OK
        &&context.updates==5&&context.controls==5&&context.frameTicks==2&&context.fixedTicks==2&&ReadUi(session.value)!=uiBefore,"resume advances one frame without paused catch-up");
}
}
int main(int argc,char** argv)
{
    try
    {
        PausedBridge();
        const auto root=std::filesystem::absolute(argc>1?argv[1]:"out/validation/native-editor-bridge")/(std::to_string(GetCurrentProcessId())+"-"+std::to_string(GetTickCount64()));std::filesystem::create_directories(root/"Assets");
        Session session;Context context;Check(se_session_create(&session.value)==SE_OK,"native session");
        Check(se_entity_create(session.value,"editor source",13,&context.original)==SE_OK&&se_entity_get_stable_id(session.value,context.original,&context.stable)==SE_OK,"source identity");
        const SE_Transform pose{3,2,1,0,0,0,1,1,1,1};Check(se_entity_set_transform(session.value,context.original,&pose)==SE_OK,"source pose");
        uint32_t sceneLength=0;Check(se_scene_export(session.value,nullptr,0,&sceneLength)==SE_BUFFER_TOO_SMALL,"scene source size");
        std::vector<char> scene(sceneLength);Check(se_scene_export(session.value,scene.data(),sceneLength,&sceneLength)==SE_OK,"scene source export");
        const auto scenePath=root/"Main.scene.json";{std::ofstream file(scenePath,std::ios::binary);file<<scene.data();Check(bool(file),"scene file fixture");}
        ScopedEnvironment engineRoot(L"SELFENGINE_EDITOR_ENGINE_ROOT",std::filesystem::current_path());
        ScopedEnvironment sceneFile(L"SELFENGINE_EDITOR_SCENE_FILE",scenePath),uiFile(L"SELFENGINE_EDITOR_UI_FILE",{});
        Check(se_session_set_locale(session.value,"en-US",5)==SE_OK,"source engine root does not override runtime localization resources");
        Check(se_session_set_editor_data(session.value,Metadata,&context)==SE_OK,"editor metadata callback");const auto utf8=root.u8string();
        Check(se_session_run_editor(session.value,reinterpret_cast<const char*>(utf8.data()),static_cast<uint32_t>(utf8.size()),"en-US",5,0,3,0)==SE_OK,"actual native editor window");
        Check(context.valid&&context.calls>=2&&context.current!=0,"metadata reads current edit World generation and values");
        Check(context.documentCalls>=2,"external document snapshot uses existing metadata callback with entity zero");
        int32_t alive=1;SE_Entity reopened=0;Check(se_entity_is_alive(session.value,context.current,&alive)==SE_OK&&alive==0&&se_entity_find(session.value,context.stable,&reopened)==SE_OK&&reopened!=context.current,"closing copy invalidates edit handles and preserves stable reference");
        std::ifstream original(scenePath,std::ios::binary);const std::string unchanged{std::istreambuf_iterator<char>(original),{}};
        Check(unchanged==scene.data(),"editor initialization and closing do not overwrite source scene");
        std::ofstream proof(root/"verification.json");proof<<"{\"schema\":1,\"success\":true,\"metadata_calls\":"<<context.calls<<",\"current_edit_world\":true,\"closing_handles_invalidated\":true,\"runtime_paused_scripts_stopped\":true,\"runtime_paused_ui_and_controls_advance\":true,\"paused_physics_stopped\":true,\"explicit_engine_and_scene\":true}\n";
        std::cout<<"NativeEditorBridgeTests passed: current edit World, readonly queries and native GPU host.\n";return 0;
    }
    catch(const std::exception& error){std::cerr<<"NativeEditorBridgeTests failed: "<<error.what()<<'\n';return 1;}
}
