#include "FunctionLayer/Scripting/NativeBridge/EngineNative.h"
#include <nlohmann/json.hpp>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <stdexcept>
#include <thread>
#include <vector>

namespace
{
void Check(bool value,const char* text){if(!value)throw std::runtime_error(text);}
struct Context {std::thread::id thread=std::this_thread::get_id();std::vector<uint8_t> values;SE_EventHeader last{};bool wrongThread=false,fault=false;};
int32_t SE_CALL Receive(SE_Session,uint64_t,const SE_EventHeader* header,const uint8_t* bytes,uint32_t length,void* data)
{
    auto& context=*static_cast<Context*>(data);context.wrongThread|=std::this_thread::get_id()!=context.thread;
    if(length!=4)return 3;if(context.fault)return 1;context.last=*header;context.values.push_back(bytes[0]);return 0;
}
struct Session {SE_Session value=0;Session(){Check(se_session_create(&value)==SE_OK,"create");}~Session(){se_session_close(value);}};
}
int main(int argc,char** argv)
{
    try
    {
        Session first,other;Context context;SE_Entity entity=0;uint64_t component=0,publisher=0,subscription=0;
        Check(se_event_set_callback(first.value,Receive,&context)==SE_OK&&se_event_register_type(first.value,1,1,"engine.int32",12,4)==SE_OK,"callback and explicit type");
        Check(se_entity_create(first.value,"owner",5,&entity)==SE_OK&&se_script_component_attach(first.value,entity,1,1,&component)==SE_OK,"component owner");
        Check(se_event_publisher_create(first.value,&publisher)==SE_OK&&se_event_subscribe(first.value,publisher,component,1,0,0,&subscription)==SE_OK,"owned subscription");
        uint64_t foreign=0;Check(se_event_subscribe(other.value,publisher,component,1,0,0,&foreign)!=SE_OK,"foreign session rejected");
        uint32_t result=0;uint8_t bytes[4]={7,0,0,0};
        Check(se_event_publish(publisher,1,2,2,bytes,4,0,0,0,0,&result)==SE_OK&&result==3,"unsupported version has an observable post result");
        Check(se_event_publish(publisher,1,1,2,bytes,4,0,0,0,0,&result)==SE_OK&&result==0&&context.values.empty(),"publication cannot call receiver inline");bytes[0]=99;
        Check(se_session_step(first.value,0)==SE_OK&&context.values==std::vector<uint8_t>{7}&&context.last.frame==0&&context.last.phase==2,"owned bytes and explicit envelope");
        bool workerOk=true;std::thread producer([&]{for(uint8_t i=0;i<32;++i){const uint8_t value[4]={i,0,0,0};uint32_t post=99;workerOk&=se_event_publish(publisher,1,1,2,value,4,0,0,0,0,&post)==SE_OK&&post==0;}});producer.join();
        Check(workerOk&&context.values.size()==1&&se_session_step(first.value,0)==SE_OK&&context.values.size()==33&&!context.wrongThread,"worker posts and main-thread delivery");
        for(uint8_t i=0;i<32;++i)Check(context.values[i+1]==i,"same producer FIFO");
        Check(se_event_publish(publisher,1,1,2,bytes,1,0,0,0,0,&result)==SE_OK&&result==0&&se_session_step(first.value,0)==SE_OK,"malformed value reaches registered decoder");
        int32_t alive=0,active=0;SE_EventStats stats{};
        Check(se_event_stats(first.value,&stats)==SE_OK&&stats.invalid_payload==1&&stats.callback_failures==0&&se_script_component_state(first.value,component,&alive,&active)==SE_OK&&alive&&active,"bad payload does not disable a healthy receiver");
        context.fault=true;Check(se_event_publish(publisher,1,1,2,bytes,4,0,0,0,0,&result)==SE_OK&&se_session_step(first.value,0)==SE_OK,"callback failure contained by native dispatch");
        Check(se_event_stats(first.value,&stats)==SE_OK&&stats.callback_failures==1&&se_script_component_state(first.value,component,&alive,&active)==SE_OK&&!active,"callback fault disables its owner");
        uint32_t required=0;Check(se_event_trace(first.value,nullptr,0,&required)==SE_BUFFER_TOO_SMALL,"trace length");std::vector<char> trace(required);
        Check(se_event_trace(first.value,trace.data(),required,&required)==SE_OK&&!nlohmann::json::parse(trace.data()).empty(),"trace includes queue, source and delivery context");
        Check(se_event_unsubscribe(first.value,subscription)==SE_OK&&se_event_unsubscribe(first.value,subscription)==SE_OK,"idempotent unsubscribe");
        Check(se_event_publisher_close(publisher)==SE_OK,"publisher close");std::thread late([&]{workerOk=se_event_publish(publisher,1,1,2,bytes,4,0,0,0,0,&result)==SE_OK&&result==1;});late.join();Check(workerOk,"late worker cannot reopen channel");
        uint64_t orphan=0;{Session closing;Check(se_event_register_type(closing.value,1,1,"engine.int32",12,4)==SE_OK&&se_event_publisher_create(closing.value,&orphan)==SE_OK,"closing fixture");}
        Check(se_event_publish(orphan,1,1,2,bytes,4,0,0,0,0,&result)==SE_OK&&result==1,"publisher does not retain closed session");
        const std::filesystem::path proof=argc>1?argv[1]:"out/native-event/verification.json";std::filesystem::create_directories(proof.parent_path());
        std::ofstream(proof)<<nlohmann::json{{"success",true},{"headerBytes",sizeof(SE_EventHeader)},{"statsBytes",sizeof(SE_EventStats)},{"ownedBytes",true},{"foreignSession",true},{"workerMessages",32},{"mainThreadCallbacks",true},{"badVersion",true},{"invalidPayload",true},{"callbackContained",true},{"latePublisherClosed",true},{"weakSession",true}}.dump(2);
        std::cout<<"NativeEvent passed (wire ABI, owned values, workers, failure isolation and close)\n";return 0;
    }
    catch(const std::exception& exception){std::cerr<<"[FAIL] "<<exception.what()<<'\n';return 1;}
}
