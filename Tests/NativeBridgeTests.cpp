#include "FunctionLayer/Scripting/NativeBridge/EngineNative.h"
#include <cmath>
#include <cstring>
#include <filesystem>
#include <iostream>
#include <limits>
#include <stdexcept>
#include <thread>
#include <vector>

namespace
{
void Require(bool condition, const char* message) { if (!condition) throw std::runtime_error(message); }
int32_t SE_CALL Update(SE_Session session, double dt, void* data)
{
    auto* calls = static_cast<int*>(data);
    ++*calls;
    double value = 0;
    if (se_session_get_value(session, &value) != SE_OK || se_session_set_value(session, value + dt) != SE_OK) return 1;
    if (se_session_close(session) != SE_BUSY || se_session_step(session, dt) != SE_BUSY) return 1;
    return 0;
}
int32_t SE_CALL Throwing(SE_Session, double, void*) { throw std::runtime_error("expected callback exception"); }
int32_t SE_CALL Rejecting(SE_Session, double, void*) { return 17; }
void UiBlockingContract()
{
    static_assert(sizeof(SE_UiStyle)==56);SE_Session session=0;Require(se_session_create(&session)==SE_OK,"UI blocking session");
    struct Guard{SE_Session value;~Guard(){se_session_close(value);}} guard{session};uint64_t panel=0;int32_t consumed=0;
    Require(se_ui_create(session,0,0,&panel)==SE_OK&&se_ui_layout(session,640,480)==SE_OK,"UI blocking fixture");
    Require(se_ui_pointer(session,1,10,10,0,&consumed)==SE_OK&&!consumed,"panel pass-through default");
    Require(se_ui_set_property(session,panel,4,1)==SE_OK&&se_ui_pointer(session,1,10,10,0,&consumed)==SE_OK&&consumed,"blocking panel consumes");
    Require(se_ui_consumes_keyboard(session,&consumed)==SE_OK&&consumed,"blocking panel focuses");
    Require(se_ui_set_property(session,panel,4,.5)==SE_INVALID_ARGUMENT&&se_ui_set_property(session,panel,4,2)==SE_INVALID_ARGUMENT,"blocking property validates bool domain");
    Require(se_ui_consumes_keyboard(session,&consumed)==SE_OK&&consumed,"invalid bool preserves focus");
    Require(se_ui_set_property(session,panel,4,0)==SE_OK&&se_ui_consumes_keyboard(session,&consumed)==SE_OK&&!consumed,"clearing blocking clears focus");
    Require(se_ui_pointer(session,1,10,10,0,&consumed)==SE_OK&&!consumed,"clearing blocking clears capture");
}
void WindowProfileContract()
{
    SE_Session session=0;Require(se_session_create(&session)==SE_OK,"profile session");
    struct Guard{SE_Session value;~Guard(){se_session_close(value);}} guard{session};
    Require(se_session_set_window_profile(session,nullptr,0,0,0)==SE_OK,"disable profile");
    Require(se_session_set_window_profile(session,nullptr,4,1280,720)==SE_INVALID_ARGUMENT,"profile null path");
    Require(se_session_set_window_profile(session,"x",1,63,720)==SE_INVALID_ARGUMENT,"profile small width");
    Require(se_session_set_window_profile(session,"x",1,1280,8193)==SE_INVALID_ARGUMENT,"profile large height");
    Require(se_session_set_window_profile(session,"\xc0\x80",2,1280,720)==SE_INVALID_ARGUMENT,"profile invalid UTF8");
    Require(se_session_set_window_profile(session,"x\0y",3,1280,720)==SE_INVALID_ARGUMENT,"profile embedded NUL");
    Require(se_session_set_window_profile(session,".",1,1280,720)==SE_INVALID_ARGUMENT,"profile existing directory");
    const auto path=(std::filesystem::temp_directory_path()/("selfengine-profile-"+std::to_string(session))).u8string();
    Require(se_session_set_window_profile(session,reinterpret_cast<const char*>(path.data()),static_cast<uint32_t>(path.size()),1280,720)==SE_OK,"profile valid target");
    Require(!std::filesystem::exists(std::filesystem::path(path)),"configuring profile has no file side effects");
    Require(se_session_set_update(session,[](SE_Session s,double,void*)->int32_t{return se_session_set_window_profile(s,nullptr,0,0,0)==SE_BUSY?0:1;},nullptr)==SE_OK&&se_session_step(session,.01)==SE_OK,"profile cannot change during step");
    Require(se_session_set_window_profile(session,nullptr,0,0,0)==SE_OK,"profile disable after step");
}
void BodyStateContract()
{
    static_assert(sizeof(SE_BodyState)==72);SE_Session session=0;Require(se_session_create(&session)==SE_OK,"body state session");
    struct Guard{SE_Session value;~Guard(){se_session_close(value);}} guard{session};SE_Entity entity=0;
    Require(se_entity_create(session,"body state",10,&entity)==SE_OK,"body state entity");
    const SE_Transform original{0,10,0,0,0,0,1,1,1,1};Require(se_entity_set_transform(session,entity,&original)==SE_OK,"body state pose");
    int32_t previousDebug=9;
    Require(se_physics_set_debug_draw(session,0,&previousDebug)==SE_OK&&previousDebug==1,"debug drawing initially enabled");
    previousDebug=17;Require(se_physics_set_debug_draw(session,2,&previousDebug)==SE_INVALID_ARGUMENT&&previousDebug==17,"invalid debug drawing preserves output");
    Require(se_physics_set_debug_draw(session,1,nullptr)==SE_INVALID_ARGUMENT,"debug drawing requires previous output");
    Require(se_physics_set_debug_draw(session,1,&previousDebug)==SE_OK&&previousDebug==0,"failed debug requests preserve disabled state");
    const SE_BoxBodyDesc description{{.5f,.5f,.5f},2,.6f,0,1,UINT32_MAX};SE_Body body=0;Require(se_body_create(session,entity,&description,&body)==SE_OK,"body creation");
    SE_BodyState state{};Require(se_body_get_state(session,body,nullptr)==SE_INVALID_ARGUMENT,"null state output");
    Require(se_body_get_state(session,body,&state)==SE_OK&&state.mass==2&&state.transform.py==10&&state.transform.sx==1&&state.linear_velocity.y==0,"complete body state layout");
    Require(se_body_set_velocity(session,body,{2,0,0},{0,2,0})==SE_OK&&se_body_apply_force(session,body,{12,0,0},{0,11,0},0)==SE_OK&&se_physics_step(session,1.0/60)==SE_OK,"force and angular integration");
    Require(se_body_get_state(session,body,&state)==SE_OK&&state.linear_velocity.x>2&&state.angular_velocity.z<-.5f&&state.transform.py<10,"state exposes integrated motion");
    SE_Transform fetched{};Require(se_entity_get_transform(session,entity,&fetched)==SE_OK&&fetched.px==state.transform.px&&fetched.py==state.transform.py,"body pose synchronized with World");
    Require(se_entity_set_transform(session,entity,&original)==SE_BUSY,"physics owns transform writes");
    auto teleported=original;teleported.px=3;teleported.py=4;teleported.pz=5;const auto before=state;
    Require(se_body_teleport(session,body,&teleported,0)==SE_OK&&se_body_get_state(session,body,&state)==SE_OK&&state.transform.px==3&&state.linear_velocity.x==before.linear_velocity.x,"teleport preserves velocity on request");
    Require(se_body_teleport(session,body,&teleported,1)==SE_OK&&se_body_get_state(session,body,&state)==SE_OK&&state.linear_velocity.x==0&&state.angular_velocity.z==0,"teleport clears velocities on request");
    Require(se_body_destroy(session,body)==SE_OK,"destroy queried body");state.mass=-123;
    Require(se_body_get_state(session,body,&state)==SE_INVALID_HANDLE&&state.mass==-123,"stale state query preserves output");
    SE_Body replacement=0;Require(se_body_create(session,entity,&description,&replacement)==SE_OK&&replacement!=body&&se_entity_destroy(session,entity)==SE_OK&&se_body_get_state(session,replacement,&state)==SE_INVALID_HANDLE,"entity destruction invalidates body query");
}
}

int main()
{
    try
    {
        SE_AbiInfo abi{};
        Require(se_get_abi_info(&abi) == SE_OK && abi.version == 1 && abi.transform_size == 40 && abi.pointer_size == 8, "ABI layout");
        BodyStateContract();
        UiBlockingContract();
        WindowProfileContract();
        SE_Session session = 0;
        Require(se_session_create(&session) == SE_OK && session != 0, "create session");
        int calls = 0;
        Require(se_session_set_update(session, Update, &calls) == SE_OK, "set callback");
        for (int frame = 0; frame < 120; ++frame) Require(se_session_step(session, 1.0 / 60.0) == SE_OK, "callback step");
        double value = 0;
        uint64_t count = 0;
        Require(se_session_get_value(session, &value) == SE_OK && std::abs(value - 2.0) < 1e-9 && calls == 120, "callback value and count");
        Require(se_session_get_step_count(session, &count) == SE_OK && count == 120, "native step counter");
        Require(se_session_step(session, std::numeric_limits<double>::quiet_NaN()) == SE_INVALID_ARGUMENT, "reject NaN dt");
        Require(se_session_set_update(session, Throwing, nullptr) == SE_OK && se_session_step(session, 0.1) == SE_INTERNAL_ERROR, "native callback exception boundary");
        Require(se_session_set_update(session, Rejecting, nullptr) == SE_OK && se_session_step(session, 0.1) == SE_CALLBACK_FAILED, "callback rejection");
        Require(se_session_set_update(session, nullptr, nullptr) == SE_OK, "unregister callback");
        int32_t threadResult = SE_OK;
        std::thread wrongThread([&] { double number = 0; threadResult = se_session_get_value(session, &number); });
        wrongThread.join();
        Require(threadResult == SE_WRONG_THREAD, "owner thread check");

        const char name[] = "\xe4\xb8\xad\xe6\x96\x87 Object";
        SE_Entity entity = 0;
        Require(se_entity_create(session, name, sizeof(name)-1, &entity) == SE_OK, "create UTF8 entity");
        uint32_t required = 0;
        Require(se_entity_get_name(session, entity, nullptr, 0, &required) == SE_BUFFER_TOO_SMALL && required == sizeof(name), "UTF8 query length");
        std::vector<char> bytes(required);
        Require(se_entity_get_name(session, entity, bytes.data(), required, &required) == SE_OK && std::strcmp(bytes.data(), name) == 0, "UTF8 name roundtrip");
        SE_Entity bad = 0;
        const char invalid[] = "\xc0\x80";
        Require(se_entity_create(session, invalid, 2, &bad) == SE_INVALID_ARGUMENT, "invalid UTF8");
        SE_Transform transform{1,2,3,0,0,0,1,1,1,1};
        Require(se_entity_set_transform(session, entity, &transform) == SE_OK, "set transform");
        SE_Transform fetched{};
        Require(se_entity_get_transform(session, entity, &fetched) == SE_OK && fetched.py == 2 && fetched.qw == 1, "get transform");
        transform.sx = 0;
        Require(se_entity_set_transform(session, entity, &transform) == SE_INVALID_ARGUMENT, "singular transform rejected");
        transform.sx=1;transform.qx=std::numeric_limits<float>::max();
        Require(se_entity_set_transform(session,entity,&transform)==SE_INVALID_ARGUMENT,"overflowing quaternion norm rejected at ABI");
        uint64_t stableId = 0;
        Require(se_entity_get_stable_id(session, entity, &stableId) == SE_OK && stableId != 0, "stable ID");
        Require(se_scene_export(session, nullptr, 0, &required) == SE_BUFFER_TOO_SMALL, "scene length");
        bytes.resize(required);
        Require(se_scene_export(session, bytes.data(), required, &required) == SE_OK, "scene export");
        Require(se_scene_import(session, bytes.data(), required-1) == SE_OK, "scene import");
        int32_t alive = 1;
        Require(se_entity_is_alive(session, entity, &alive) == SE_OK && alive == 0, "old scene handle invalidated");
        Require(se_entity_find(session, stableId, &entity) == SE_OK, "stable ID restored");
        Require(se_scene_import(session, "{}", 2) == SE_INVALID_ARGUMENT && se_entity_is_alive(session, entity, &alive) == SE_OK && alive == 1, "bad scene preserves world");
        Require(se_entity_destroy(session, entity) == SE_OK, "destroy entity");
        Require(se_entity_get_transform(session, entity, &fetched) == SE_INVALID_HANDLE, "stale entity rejected");
        Require(se_entity_create(session, "replacement", 11, &bad) == SE_OK && bad != entity, "slot reuse has new generation");

        const SE_Session oldSession = session;
        Require(se_session_close(session) == SE_OK && se_session_close(session) == SE_INVALID_HANDLE, "close invalidates session");
        for (int iteration = 0; iteration < 50; ++iteration)
        {
            Require(se_session_create(&session) == SE_OK && session != oldSession, "new session generation");
            Require(se_session_close(session) == SE_OK, "repeated lifecycle");
        }
        Require(se_session_get_value(oldSession, &value) == SE_INVALID_HANDLE, "stale session rejected");
        char diagnostic[128]{};
        Require(se_get_last_error(diagnostic, sizeof(diagnostic), &required) == SE_OK && std::strcmp(diagnostic, "Bridge.InvalidSession") == 0, "stable error key");
        std::cout << "NativeBridgeTests PASS: ABI, callbacks, failures, UTF8, scene, handles, repeated lifetime\n";
        return 0;
    }
    catch (const std::exception& error) { std::cerr << "NativeBridgeTests FAIL: " << error.what() << '\n'; return 1; }
}
