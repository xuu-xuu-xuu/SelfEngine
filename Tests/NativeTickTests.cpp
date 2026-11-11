#include "FunctionLayer/Scripting/NativeBridge/EngineNative.h"
#include <nlohmann/json.hpp>
#include <cmath>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <stdexcept>

namespace
{
void Check(bool condition,const char* message){if(!condition)throw std::runtime_error(message);}
struct Context
{
    uint64_t a=0,b=0;SE_Entity target=0,born=0;
    int fixedA=0,fixedB=0,tickB=0,controls=0;
    bool birth=true,destroy=false,fault=false;
};
int32_t SE_CALL Callback(SE_Session session,uint64_t component,uint32_t phase,double,uint64_t,uint64_t,void* opaque)
{
    auto& context=*static_cast<Context*>(opaque);
    if(!component){if(phase==0x101)++context.controls;return 0;}
    if(component==context.a&&phase==2)
    {
        ++context.fixedA;
        if(context.destroy){context.destroy=false;if(se_entity_destroy(session,context.target)!=SE_OK)return 1;}
    }
    if(component==context.b&&phase==2)++context.fixedB;
    if(component==context.b&&phase==4)++context.tickB;
    if(component==context.a&&phase==4)
    {
        if(context.fault)return 17;
        if(context.birth)
        {
            context.birth=false;int32_t alive=1,pending=0;
            const SE_Transform value{3,4,5,0,0,0,1,1,1,1};
            if(se_entity_create(session,"pending",7,&context.born)!=SE_OK||se_entity_is_alive(session,context.born,&alive)!=SE_OK||alive
                ||se_entity_is_pending(session,context.born,&pending)!=SE_OK||!pending||se_entity_set_transform(session,context.born,&value)!=SE_OK)return 1;
        }
    }
    return 0;
}
}
int main(int argc,char** argv)
{
    try
    {
        static_assert(sizeof(SE_TickPrerequisite)==16);
        SE_Session first=0,second=0;Check(se_session_create(&first)==SE_OK&&se_session_create(&second)==SE_OK,"tick sessions");
        struct Guard{SE_Session a,b;~Guard(){se_session_close(a);se_session_close(b);}} guard{first,second};
        Context context;SE_Entity owner=0;Check(se_entity_create(first,"producer",8,&owner)==SE_OK&&se_entity_create(first,"consumer",8,&context.target)==SE_OK,"entities");
        Check(se_script_component_attach(first,owner,1,1,&context.a)==SE_OK&&se_script_component_attach(first,context.target,2,1,&context.b)==SE_OK,"controlled components");
        Check(se_environment_set_callback(first,Callback,&context)==SE_OK&&se_environment_set_callback(second,Callback,&context)==SE_OK,"callback installation");
        Check(se_environment_configure(first,1.0/60,8,1,1)==SE_OK,"clock configuration");
        SE_Body body=0;const SE_BoxBodyDesc description{{.5f,.5f,.5f},1,.6f,0,1,UINT32_MAX};
        const SE_Transform pose{0,10,0,0,0,0,1,1,1,1};Check(se_entity_set_transform(first,context.target,&pose)==SE_OK&&se_body_create(first,context.target,&description,&body)==SE_OK,"physics body");
        uint64_t af=0,bf=0,at=0,bt=0,foreign=999;
        Check(se_component_tick_register(first,context.a,2,1,0,&af)==SE_OK&&se_component_tick_register(first,context.b,2,1,0,&bf)==SE_OK
            &&se_component_tick_register(first,context.a,4,0,0,&at)==SE_OK&&se_component_tick_register(first,context.b,4,0,0,&bt)==SE_OK,"actual component tasks");
        Check(se_component_tick_register(second,context.a,2,1,0,&foreign)==SE_INVALID_ARGUMENT&&foreign==0,"cross-world registration rejects");
        const SE_TickPrerequisite dependency{af,1,0};Check(se_component_tick_prerequisites(first,bf,&dependency,1)==SE_OK,"fresh dependency");
        const SE_TickPrerequisite cycle{bf,0,0},future{at,0,0},bad{af,0,1};
        Check(se_component_tick_prerequisites(first,af,&cycle,1)==SE_INVALID_ARGUMENT&&se_component_tick_prerequisites(first,af,&future,1)==SE_INVALID_ARGUMENT
            &&se_component_tick_prerequisites(first,af,&bad,1)==SE_INVALID_ARGUMENT,"cycle future and reserved flags reject");
        Check(se_component_tick_remove(second,af)==SE_INVALID_HANDLE&&se_environment_stats(first,nullptr,nullptr,nullptr)==SE_INVALID_ARGUMENT,"foreign token and null outputs reject");
        Check(se_session_step(first,0)==SE_OK&&context.fixedA==0&&context.born!=0,"zero-step frame and deferred birth");
        int32_t alive=1,pending=0;Check(se_entity_is_alive(first,context.born,&alive)==SE_OK&&!alive&&se_entity_is_pending(first,context.born,&pending)==SE_OK&&pending,"birth remains pending through FrameEnd");
        Check(se_entity_destroy(first,context.born)==SE_OK,"stop cancels a pending birth before activation");
        context.birth=true;context.born=0;
        Check(se_session_step(first,0)==SE_OK&&context.born!=0,"canceled activation and property commands do not poison the next frame");
        Check(se_session_step(first,0)==SE_OK&&se_entity_is_alive(first,context.born,&alive)==SE_OK&&alive,"next FrameBegin activates");
        SE_Transform prepared{};Check(se_entity_get_transform(first,context.born,&prepared)==SE_OK&&prepared.px==3&&prepared.py==4,"queued properties prepared");
        Check(se_session_step(first,1)==SE_OK&&context.fixedA==8&&context.fixedB==8,"eight callbacks on one clock");
        uint64_t steps=0;double simulation=0,dropped=0;
        Check(se_environment_stats(first,&steps,&simulation,&dropped)==SE_OK&&steps==8&&std::abs(simulation-8.0/60)<1e-9&&std::abs(dropped-52.0/60)<1e-9,"actual steps and drop statistics");
        SE_BodyState state{};Check(se_body_get_state(first,body,&state)==SE_OK&&state.transform.py<10,"actual physics progressed");
        uint32_t extra=0;double discarded=0;
        Check(se_physics_step(first,1.0/60)==SE_BUSY&&se_physics_advance(first,1.0/60,&extra,&discarded)==SE_BUSY&&se_environment_fixed_step(first,1.0/60)==SE_INVALID_ARGUMENT,"all second-driver entries reject");
        Check(se_environment_set_paused(first,1)==SE_OK&&se_session_step(first,1)==SE_OK&&context.fixedA==8,"pause no fixed step");
        Check(se_environment_set_paused(first,0)==SE_OK&&se_session_step(first,1.0/60)==SE_OK&&context.fixedA==9&&context.fixedB==9,"resume exactly one step");
        context.destroy=true;Check(se_session_step(first,1.0/60)==SE_OK&&context.fixedA==10&&context.fixedB==9,"destroyed consumer skips later callback");
        Check(se_body_get_state(first,body,&state)==SE_INVALID_HANDLE,"destroyed body removed before next solver step");
        context.fault=true;Check(se_session_step(first,0)==SE_OK,"component exception remains isolated");
        int32_t active=1;Check(se_script_component_state(first,context.a,&alive,&active)==SE_OK&&alive&&!active,"failed component disabled");
        Check(se_environment_release(first)==SE_OK&&se_physics_step(first,1.0/60)==SE_OK,"release restores standalone physics");
        const std::filesystem::path proof=argc>1?argv[1]:"out/native-tick/verification.json";std::filesystem::create_directories(proof.parent_path());
        std::ofstream(proof)<<nlohmann::json{{"success",true},{"prerequisiteBytes",16},{"componentCallbacks",true},{"clockSteps",8},{"droppedSteps",52},{"actualPhysics",true},{"secondDriverRejected",true},{"pauseResume",true},{"pendingBirth",true},{"pendingCancel",true},{"preparedProperties",true},{"foreignRejected",true},{"badGraphRejected",true},{"destroyBeforeSolver",true},{"callbackIsolated",true},{"releaseRestoresPhysics",true}}.dump(2);
        std::cout<<"NativeTick passed (phase ABI, clock, deferred birth, physics ownership and cleanup)\n";return 0;
    }
    catch(const std::exception& exception){std::cerr<<"[FAIL] "<<exception.what()<<'\n';return 1;}
}
