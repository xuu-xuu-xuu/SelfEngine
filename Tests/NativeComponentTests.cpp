#include "FunctionLayer/Scripting/NativeBridge/EngineNative.h"
#include <nlohmann/json.hpp>
#include <filesystem>
#include <fstream>
#include <stdexcept>
#include <iostream>
namespace
{
void Check(bool value,const char* message){if(!value)throw std::runtime_error(message);}
void Test()
{
    SE_Session first=0,second=0;Check(se_session_create(&first)==SE_OK&&se_session_create(&second)==SE_OK,"component sessions");
    struct Guard{SE_Session first,second;~Guard(){se_session_close(first);se_session_close(second);}} guard{first,second};SE_Entity owner=0,other=0;
    Check(se_entity_create(first,"owner",5,&owner)==SE_OK&&se_entity_create(second,"other",5,&other)==SE_OK,"component owners");
    std::uint64_t a=0,b=0,replacement=0;Check(se_script_component_attach(first,owner,1,0,&a)==SE_OK&&se_script_component_attach(first,owner,2,1,&b)==SE_OK&&a!=b,"independent ABI component instances");
    std::uint32_t count=0;int alive=0,active=0,enabled=0;
    Check(se_script_component_count(first,&count)==SE_OK&&count==2&&se_script_component_state(first,a,&alive,&active)==SE_OK&&alive&&!active,"initial disabled adapter state");
    Check(se_script_component_attach(first,owner,1,1,&replacement)!=SE_OK&&se_script_component_attach(first,owner,3,2,&replacement)==SE_INVALID_ARGUMENT,"duplicate controls and invalid flag rejected");
    Check(se_script_component_state(second,a,&alive,&active)==SE_OK&&!alive&&!active&&se_script_component_set_enabled(second,a,1)==SE_INVALID_HANDLE,"foreign-world component rejected");
    Check(se_entity_set_enabled(first,owner,0)==SE_OK&&se_entity_get_enabled(first,owner,&enabled)==SE_OK&&!enabled&&se_script_component_state(first,b,&alive,&active)==SE_OK&&alive&&!active,"owner disables the adapter");
    Check(se_script_component_set_enabled(first,a,1)==SE_OK&&se_script_component_state(first,a,&alive,&active)==SE_OK&&!active,"individual enable cannot bypass owner");
    Check(se_entity_set_enabled(first,owner,1)==SE_OK&&se_script_component_state(first,a,&alive,&active)==SE_OK&&active,"owner reactivation");
    Check(se_script_component_remove(first,a)==SE_OK&&se_script_component_attach(first,owner,1,1,&replacement)==SE_OK&&replacement!=a&&se_script_component_state(first,a,&alive,&active)==SE_OK&&!alive,"old component never reaches a reused slot");
    Check(se_entity_destroy(first,owner)==SE_OK&&se_script_component_count(first,&count)==SE_OK&&count==0&&se_script_component_state(first,b,&alive,&active)==SE_OK&&!alive&&se_script_component_remove(first,b)==SE_INVALID_HANDLE,"owner destruction clears every controlled association");
    Check(se_script_component_state(first,b,nullptr,&active)==SE_INVALID_ARGUMENT&&se_entity_set_enabled(second,other,2)==SE_INVALID_ARGUMENT,"invalid ABI outputs and flags");
}
}
int main(int argc,char** argv)
{try{Test();const std::filesystem::path path=argc>1?argv[1]:"out/validation/native-components/verification.json";std::filesystem::create_directories(path.parent_path());std::ofstream file(path);file<<nlohmann::json{{"success",true},{"controlledIds",true},{"multiInstance",true},{"initialDisabled",true},{"ownerEnable",true},{"crossWorld",true},{"staleGeneration",true},{"destroyCleanup",true},{"countAfterDestroy",0}}.dump(2);Check(bool(file),"component ABI evidence");std::cout<<"NativeComponentTests passed (opaque controlled instances, enabled state, context identity and cleanup).\n";return 0;}catch(const std::exception& error){std::cerr<<error.what()<<'\n';return 1;}}
