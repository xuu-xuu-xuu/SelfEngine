#include "FunctionLayer/Scripting/NativeBridge/EngineNative.h"
#include <nlohmann/json.hpp>
#include <array>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <stdexcept>
#include <thread>

namespace
{
void Check(bool value,const char* message){if(!value)throw std::runtime_error(message);}
template<class Read> std::string Text(Read read)
{uint32_t required=0;Check(read(nullptr,0,&required)==SE_BUFFER_TOO_SMALL,"wire text length");std::string result(required,'\0');Check(read(result.data(),required,&required)==SE_OK,"wire text copy");result.pop_back();return result;}
nlohmann::json Info(SE_Session session,uint64_t scene){return nlohmann::json::parse(Text([&](char* buffer,uint32_t count,uint32_t* required){return se_scene_get(session,scene,buffer,count,required);}));}
std::string Document(uint64_t id,float x=0,bool missing=false)
{
    const nlohmann::json transform{{"position",{x,0,0}},{"rotation",{0,0,0,1}},{"scale",{1,1,1}}};
    auto components=nlohmann::json::array();components.push_back({{"type","1"},{"version",1},{"enabled",true},{"properties",transform}});
    if(missing)components.push_back({{"type","3"},{"version",1},{"enabled",true},{"properties",{{"asset","missing"},{"materials",nlohmann::json::array()},{"castsShadow",true},{"receivesShadow",true}}}});
    return nlohmann::json{{"format","SelfEngine.Scene"},{"version",2},{"entities",nlohmann::json::array({{{"id",std::to_string(id)},{"name","actor"},{"enabled",true},{"parent",nullptr},{"components",components}}})}}.dump();
}
struct Context {std::thread::id owner=std::this_thread::get_id();SE_Entity old=0;int callbacks=0;bool main=true,alive=true,busy=true,fault=false;uint32_t invalidated=0;};
int32_t SE_CALL Retiring(SE_Session session,uint64_t,const SE_Entity* members,uint32_t count,const uint64_t*,uint32_t invalidated,void* data)
{
    auto& value=*static_cast<Context*>(data);++value.callbacks;value.main&=std::this_thread::get_id()==value.owner;value.invalidated=invalidated;
    int32_t alive=0;value.alive&=count!=0&&members&&se_entity_is_alive(session,value.old,&alive)==SE_OK&&alive;
    value.busy&=se_session_close(session)==SE_BUSY;return value.fault?1:0;
}
struct Session {SE_Session value=0;Session(){Check(se_session_create(&value)==SE_OK,"session create");}~Session(){if(value)se_session_close(value);}};
}
int main(int argc,char** argv)
{
    try
    {
        Session first,foreign;Context context;Check(se_scene_set_callback(first.value,Retiring,&context)==SE_OK,"scene callback");
        uint64_t a=0,b=0,request=0;Check(se_scene_create(first.value,"A",1,&a)==SE_OK&&se_scene_create(first.value,"B",1,&b)==SE_OK,"scene handles");
        auto source=Document(42);Check(se_scene_request_json(first.value,a,source.data(),static_cast<uint32_t>(source.size()),0,0,&request)==SE_OK&&request&&se_session_step(first.value,0)==SE_OK,"queued document activation");
        SE_Entity original=0;Check(se_entity_find(first.value,42,&original)==SE_OK&&original,"activated entity");context.old=original;
        const auto oldScope=Info(first.value,a)["Scope"].get<uint64_t>();uint64_t scene=0;Check(se_scene_of_entity(first.value,original,&scene)==SE_OK&&scene==a,"one scene per entity");
        const SE_BoxBodyDesc box{{.5f,.5f,.5f},1,.6f,0,1,UINT32_MAX};SE_Body body=0;Check(se_body_create(first.value,original,&box,&body)==SE_OK,"owned physical proxy");
        source=Document(42,5);Check(se_scene_request_json(first.value,a,source.data(),static_cast<uint32_t>(source.size()),0,0,&request)==SE_OK&&se_session_step(first.value,0)==SE_OK,"same-ID reload through ABI");
        SE_Entity replacement=0;SE_BodyState state{};Check(se_entity_find(first.value,42,&replacement)==SE_OK&&replacement!=original&&context.callbacks==1&&context.invalidated==0&&context.alive&&context.main&&context.busy,"borrowed teardown context is safe and reused IDs remain references");
        Check(Info(first.value,a)["Scope"].get<uint64_t>()!=oldScope&&se_body_get_state(first.value,body,&state)==SE_INVALID_HANDLE,"old body cannot bind to new object with same stable ID");
        Check(se_body_create(first.value,replacement,&box,&body)==SE_OK,"fresh body after reload");context.old=replacement;
        const auto proof=argc>1?std::filesystem::path(argv[1]):std::filesystem::path("out/native-scene/verification.json");std::filesystem::create_directories(proof.parent_path()/"Assets");std::filesystem::create_directories(proof.parent_path()/"Cache");
        const auto assets=std::filesystem::absolute(proof.parent_path()/"Assets").u8string(),cache=std::filesystem::absolute(proof.parent_path()/"Cache").u8string();
        Check(se_session_set_asset_root(first.value,reinterpret_cast<const char*>(assets.data()),static_cast<uint32_t>(assets.size()),reinterpret_cast<const char*>(cache.data()),static_cast<uint32_t>(cache.size()))==SE_OK,"real empty asset catalog");
        source=Document(43,0,true);Check(se_scene_request_json(first.value,b,source.data(),static_cast<uint32_t>(source.size()),0,0,&request)==SE_OK&&se_session_step(first.value,0)==SE_OK,"dependency request processed");
        Check(Info(first.value,b)["State"]==5&&Info(first.value,b)["Error"]=="Scene.DependencyFailed"&&Info(first.value,a)["Active"]==true&&se_body_get_state(first.value,body,&state)==SE_OK,"missing actual asset preserves old scene and body");
        uint32_t required=0;Check(se_scene_get(foreign.value,a,nullptr,0,&required)==SE_INVALID_HANDLE,"foreign scene handle rejected");
        context.fault=true;Check(se_scene_unload(first.value,a)==SE_OK&&Info(first.value,a)["State"]==4&&se_body_get_state(first.value,body,&state)==SE_INVALID_HANDLE&&context.invalidated==1,"callback failure is contained and proxy teardown continues");
        Check(se_session_step(first.value,0)==SE_OK&&Info(first.value,a)["State"]==0&&context.callbacks==2,"safe boundary destroys members once");
        const auto stats=nlohmann::json::parse(Text([&](char* buffer,uint32_t count,uint32_t* size){return se_scene_stats(first.value,buffer,count,size);}));Check(stats["CallbackFaults"]==1,"callback diagnostic count");
        context.fault=false;const auto path=proof.parent_path()/L"中文场景.scene.json";source=Document(50);std::ofstream(path,std::ios::binary)<<source;const auto encoded=std::filesystem::absolute(path).u8string();
        Check(se_scene_request_file(first.value,b,reinterpret_cast<const char*>(encoded.data()),static_cast<uint32_t>(encoded.size()),1,0,&request)==SE_OK,"Unicode file request");
        for(int i=0;i<2000&&!Info(first.value,b)["Active"].get<bool>();++i){Check(se_session_step(first.value,0)==SE_OK,"file completion frame");std::this_thread::sleep_for(std::chrono::milliseconds(1));}
        Check(Info(first.value,b)["Active"]==true,"worker file result activates on main thread");int32_t wrongThread=SE_OK;std::thread worker([&]{wrongThread=se_scene_unload(first.value,b);});worker.join();Check(wrongThread==SE_WRONG_THREAD&&Info(first.value,b)["Active"]==true,"worker cannot mutate scene");
        uint64_t closing=0;Check(se_scene_create(first.value,"closing",7,&closing)==SE_OK&&se_scene_request_file(first.value,closing,reinterpret_cast<const char*>(encoded.data()),static_cast<uint32_t>(encoded.size()),1,0,&request)==SE_OK,"pending close request");
        Check(se_session_close(first.value)==SE_OK&&context.callbacks==2,"close cancels async work without invoking world callbacks");first.value=0;
        std::ofstream(proof)<<nlohmann::json{{"success",true},{"ownedSceneSnapshot",true},{"sameIdFreshEntity",true},{"scopeRotated",true},{"oldBodyInvalid",true},{"missingAssetPreservesLive",true},{"foreignRejected",true},{"callbackContained",true},{"mainThreadCallbacks",true},{"borrowedMembers",true},{"reusedReferencesKept",true},{"removedReferencesReported",true},{"unicodeWorkerLoad",true},{"workerMutationRejected",true},{"closeCancels",true}}.dump(2);
        std::cout<<"NativeScene passed (requests, callbacks, dependencies, physical generations and close)\n";return 0;
    }
    catch(const std::exception& exception){std::cerr<<"[FAIL] "<<exception.what()<<'\n';return 1;}
}
