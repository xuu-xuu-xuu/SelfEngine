#include "FunctionLayer/Scripting/NativeBridge/EngineNative.h"
#include <nlohmann/json.hpp>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <stdexcept>
#include <vector>
namespace
{
void Check(bool value,const char* message){if(!value)throw std::runtime_error(message);}
template<class Read> nlohmann::json Json(Read read)
{
    std::uint32_t required=0;Check(read(nullptr,0,&required)==SE_BUFFER_TOO_SMALL&&required>0,"binding size query");std::vector<char> bytes(required);Check(read(bytes.data(),required,&required)==SE_OK,"binding string copy");return nlohmann::json::parse(bytes.data());
}
void Test()
{
    static_assert(sizeof(SE_UiStyle)==56);SE_Session session=0;Check(se_session_create(&session)==SE_OK,"binding session");struct Guard{SE_Session s;~Guard(){se_session_close(s);}} guard{session};std::uint64_t button=0;
    Check(se_ui_create(session,3,0,&button)==SE_OK&&se_ui_layout(session,200,160)==SE_OK,"binding button");
    const auto bindings=[&]{return Json([&](char* b,std::uint32_t c,std::uint32_t* r){return se_ui_read_bindings(session,button,b,c,r);});};
    Check(bindings().empty()&&se_ui_bind_action(session,button,0,77,"test.actions",12,"fire",4)==SE_OK,"bind stable action");
    auto saved=bindings();Check(saved.size()==1&&saved[0]["Entity"]==77&&saved[0]["Script"]=="test.actions"&&saved[0]["Action"]=="fire","binding query payload");
    Check(se_ui_bind_action(session,button,0,77,"test.actions",12,"again",5)==SE_OK&&bindings().size()==1,"same event replaces description");
    saved=bindings();Check(se_ui_bind_action(session,button,6,77,"test.actions",12,"fire",4)==SE_INVALID_ARGUMENT&&bindings()==saved,"invalid event preserves binding");
    Check(se_ui_bind_action(session,button,0,0,"test.actions",12,"fire",4)==SE_INVALID_ARGUMENT&&bindings()==saved,"zero target rejected except explicit clear");
    Check(se_ui_bind_action(session,button,0,77,"invalid name",12,"fire",4)==SE_INVALID_ARGUMENT&&bindings()==saved,"invalid identifier is atomic");
    int consumed=0;const auto click=[&]{Check(se_ui_pointer(session,1,10,10,0,&consumed)==SE_OK&&consumed&&se_ui_pointer(session,2,10,10,0,&consumed)==SE_OK&&consumed,"native click");};click();
    const auto events=Json([&](char* b,std::uint32_t c,std::uint32_t* r){return se_ui_read_json(session,2,b,c,r);});unsigned clicks=0;
    for(const auto& event:events)if(event["Kind"]==0){++clicks;Check(event["Target"]==button&&event["Binding"]==bindings()[0],"event carries current stable description");}
    Check(clicks==1&&Json([&](char* b,std::uint32_t c,std::uint32_t* r){return se_ui_read_json(session,2,b,c,r);}).empty(),"size/copy produces exactly one event");
    Check(se_ui_bind_action(session,button,0,0,nullptr,0,nullptr,0)==SE_OK&&bindings().empty(),"explicit unbind");
    Check(se_ui_bind_action(session,button,0,77,"test.actions",12,"fire",4)==SE_OK,"bind before buffered delete");click();std::uint32_t required=0;Check(se_ui_read_json(session,2,nullptr,0,&required)==SE_BUFFER_TOO_SMALL,"buffer events before deletion");Check(se_ui_remove(session,button)==SE_OK,"delete event source");std::vector<char> buffer(required);Check(se_ui_read_json(session,2,buffer.data(),required,&required)==SE_OK&&nlohmann::json::parse(buffer.data()).empty(),"retired buffered source cannot dispatch");
    Check(se_session_set_window_capture(session,nullptr,0,0)==SE_OK&&se_session_set_window_capture(session,"capture.ppm",11,0)==SE_INVALID_ARGUMENT,"capture explicit frame/clear");Check(se_session_set_window_capture(session,"capture.ppm",11,3)==SE_OK&&se_session_set_window_capture(session,nullptr,0,0)==SE_OK,"capture settings preserve old window ABI");
}
}
int main(int argc,char** argv){try{Test();const std::filesystem::path path=argc>1?argv[1]:"out/validation/ui-binding/verification.json";std::filesystem::create_directories(path.parent_path());std::ofstream report(path);report<<"{\"success\":true,\"styleBytes\":56,\"typedBinding\":true,\"replace\":true,\"atomicInvalid\":true,\"oneEvent\":true,\"unbind\":true,\"retiredBufferedSource\":true,\"captureSettings\":true}\n";Check(bool(report),"binding evidence");std::cout<<"UiBindingTests passed (C ABI descriptions, event buffers, invalid writes, deletion and capture configuration).\n";return 0;}catch(const std::exception& error){std::cerr<<error.what()<<'\n';return 1;}}
