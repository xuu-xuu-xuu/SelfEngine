#pragma once
#include <stdint.h>

#if defined(_WIN32)
#define SE_CALL __cdecl
#if defined(SELFENGINE_NATIVE_EXPORTS)
#define SE_API __declspec(dllexport)
#else
#define SE_API __declspec(dllimport)
#endif
#else
#define SE_CALL
#define SE_API __attribute__((visibility("default")))
#endif
#ifdef __cplusplus
#define SE_NOEXCEPT noexcept
extern "C" {
#else
#define SE_NOEXCEPT
#endif

// ABI 只使用定宽数字、顺序布局和调用期间借用的缓冲区，绝不跨边界释放内存。
enum SE_Result {
    SE_OK = 0, SE_INVALID_ARGUMENT = 1, SE_INVALID_HANDLE = 2,
    SE_BUFFER_TOO_SMALL = 3, SE_BUSY = 4, SE_CALLBACK_FAILED = 5,
    SE_WRONG_THREAD = 6, SE_OUT_OF_MEMORY = 7, SE_INTERNAL_ERROR = 8
};
typedef uint64_t SE_Session;
typedef uint64_t SE_Entity;
typedef uint64_t SE_Body;
typedef struct SE_Vector3 { float x,y,z; } SE_Vector3;
typedef struct SE_BoxBodyDesc {
    SE_Vector3 half_extents;
    float mass, friction, restitution;
    uint32_t layer, mask;
} SE_BoxBodyDesc;
typedef struct SE_RayHit {
    SE_Body body;
    uint64_t entity_stable_id;
    SE_Vector3 point, normal;
    float distance;
    uint32_t reserved;
} SE_RayHit;
typedef struct SE_ContactEvent {
    uint64_t entity_a, entity_b;
    int32_t began, ended;
} SE_ContactEvent;
typedef struct SE_UiStyle {
    float anchor_min_x,anchor_min_y,anchor_max_x,anchor_max_y,pivot_x,pivot_y;
    float offset_x,offset_y,width,height,padding,spacing;
    int32_t layout,clip;
} SE_UiStyle;
typedef struct SE_InputState { uint8_t down[256],pressed[256],released[256]; int32_t keyboard_consumed; } SE_InputState;
typedef struct SE_PointerState { int32_t x,y; uint32_t down,pressed,released; float wheel; int32_t consumed; } SE_PointerState;
typedef struct SE_CharacterDesc { float radius,segment_half_height,walk_speed,jump_speed,maximum_push_impulse; } SE_CharacterDesc;
typedef struct SE_CharacterState { SE_Vector3 position; float vertical_velocity; int32_t grounded; } SE_CharacterState;
typedef struct SE_ModelInfo { uint32_t meshes,vertices,indices,reserved; } SE_ModelInfo;
typedef struct SE_Transform {
    float px, py, pz;
    float qx, qy, qz, qw;
    float sx, sy, sz;
} SE_Transform;

typedef struct SE_BodyState {
    SE_Transform transform;
    SE_Vector3 linear_velocity,angular_velocity;
    float mass;
    int32_t sleeping;
} SE_BodyState;
typedef struct SE_AbiInfo {
    uint32_t version;
    uint32_t transform_size;
    uint32_t pointer_size;
    uint32_t reserved;
} SE_AbiInfo;
typedef int32_t (SE_CALL *SE_UpdateCallback)(SE_Session session, double delta_seconds, void* context);
// component 为 0 时 phase=0x100|EnvironmentPhase；否则为组件句柄和 TickStage。
typedef int32_t (SE_CALL *SE_TickCallback)(SE_Session session,uint64_t component,uint32_t phase,double delta_seconds,uint64_t frame,uint64_t fixed_step,void* context);
typedef struct SE_TickPrerequisite {uint64_t task;uint32_t mode,reserved;} SE_TickPrerequisite;
typedef struct SE_EventHeader {
    uint64_t world,channel,type,source,source_stable_id,target,scene,frame,fixed_step,sequence;
    uint32_t version,phase;
} SE_EventHeader;
typedef struct SE_EventStats {
    uint64_t accepted,delivered,rejected,dropped_targets,no_receiver,callback_failures,invalid_payload,pending,queued,pending_bytes,peak_pending,peak_bytes;
    uint32_t closed,reserved;
} SE_EventStats;
// 0=已调用，2=接收者不参与，3=载荷无效；其他值为接收回调失败。头/字节仅在调用期间借用。
typedef int32_t (SE_CALL *SE_EventCallback)(SE_Session session,uint64_t subscription,const SE_EventHeader* header,const uint8_t* bytes,uint32_t length,void* context);
// 成员仅在调用期间借用；停用后、物理代理与 GO 回收之前通知宿主。
typedef int32_t (SE_CALL *SE_SceneCallback)(SE_Session session,uint64_t scene,const SE_Entity* members,uint32_t count,const uint64_t* invalidated_ids,uint32_t invalidated_count,void* context);
typedef int32_t (SE_CALL *SE_EditorCallback)(SE_Session session, int32_t phase, void* context); // 0=Play，1=停止。
typedef int32_t (SE_CALL *SE_EditorDataCallback)(SE_Session session,uint64_t entity_stable_id,const char* request,uint32_t request_length,char* output,uint32_t capacity,uint32_t* required,void* context);

SE_API int32_t SE_CALL se_get_abi_info(SE_AbiInfo* info) SE_NOEXCEPT;
// required 包含结尾零；诊断为稳定文本键。查询错误不会覆盖上一次错误。
SE_API int32_t SE_CALL se_get_last_error(char* buffer, uint32_t capacity, uint32_t* required) SE_NOEXCEPT;
SE_API int32_t SE_CALL se_session_create(SE_Session* session) SE_NOEXCEPT;
SE_API int32_t SE_CALL se_session_close(SE_Session session) SE_NOEXCEPT;
// 除 close 外，session 只能由创建线程调用；回调同步发生在 step 的同一线程。
SE_API int32_t SE_CALL se_session_set_update(SE_Session session, SE_UpdateCallback callback, void* context) SE_NOEXCEPT;
SE_API int32_t SE_CALL se_session_step(SE_Session session, double delta_seconds) SE_NOEXCEPT;
SE_API int32_t SE_CALL se_environment_set_callback(SE_Session session,SE_TickCallback callback,void* context) SE_NOEXCEPT;
SE_API int32_t SE_CALL se_environment_configure(SE_Session session,double fixed_seconds,uint32_t maximum_steps,int32_t automatic,int32_t simulate_physics) SE_NOEXCEPT;
SE_API int32_t SE_CALL se_environment_set_paused(SE_Session session,int32_t paused) SE_NOEXCEPT;
SE_API int32_t SE_CALL se_environment_release(SE_Session session) SE_NOEXCEPT;
SE_API int32_t SE_CALL se_environment_stats(SE_Session session,uint64_t* fixed_steps,double* simulation_seconds,double* dropped_seconds) SE_NOEXCEPT;
SE_API int32_t SE_CALL se_tod_snapshot(SE_Session session,char* buffer,uint32_t capacity,uint32_t* required) SE_NOEXCEPT;
SE_API int32_t SE_CALL se_tod_set_time(SE_Session session,int64_t day,double time,int32_t replay_boundaries) SE_NOEXCEPT;
SE_API int32_t SE_CALL se_tod_set_rate(SE_Session session,double rate) SE_NOEXCEPT;
SE_API int32_t SE_CALL se_environment_fixed_step(SE_Session session,double seconds) SE_NOEXCEPT;
SE_API int32_t SE_CALL se_component_tick_register(SE_Session session,uint64_t component,uint32_t stage,int32_t simulation_time,int32_t run_when_paused,uint64_t* task) SE_NOEXCEPT;
SE_API int32_t SE_CALL se_component_tick_remove(SE_Session session,uint64_t task) SE_NOEXCEPT;
SE_API int32_t SE_CALL se_component_tick_prerequisites(SE_Session session,uint64_t task,const SE_TickPrerequisite* prerequisites,uint32_t count) SE_NOEXCEPT;
SE_API int32_t SE_CALL se_entity_is_pending(SE_Session session,SE_Entity entity,int32_t* pending) SE_NOEXCEPT;
SE_API int32_t SE_CALL se_script_component_pending(SE_Session session,uint64_t component,int32_t* pending) SE_NOEXCEPT;
SE_API int32_t SE_CALL se_event_set_callback(SE_Session session,SE_EventCallback callback,void* context) SE_NOEXCEPT;
SE_API int32_t SE_CALL se_event_register_type(SE_Session session,uint64_t type,uint32_t version,const char* key,uint32_t key_length,uint32_t maximum_bytes) SE_NOEXCEPT;
SE_API int32_t SE_CALL se_event_publisher_create(SE_Session session,uint64_t* publisher) SE_NOEXCEPT;
SE_API int32_t SE_CALL se_event_publisher_close(uint64_t publisher) SE_NOEXCEPT;
// 可在工作线程调用；publisher 是弱上下文句柄，不持有 session 或 World。
SE_API int32_t SE_CALL se_event_publish(uint64_t publisher,uint64_t type,uint32_t version,uint32_t phase,const uint8_t* bytes,uint32_t length,SE_Entity source,uint64_t source_stable_id,SE_Entity target,uint64_t scene,uint32_t* post_result) SE_NOEXCEPT;
SE_API int32_t SE_CALL se_event_subscribe(SE_Session session,uint64_t publisher,uint64_t component,uint64_t type,SE_Entity target,uint64_t scene,uint64_t* subscription) SE_NOEXCEPT;
SE_API int32_t SE_CALL se_event_unsubscribe(SE_Session session,uint64_t subscription) SE_NOEXCEPT;
SE_API int32_t SE_CALL se_event_stats(SE_Session session,SE_EventStats* stats) SE_NOEXCEPT;
SE_API int32_t SE_CALL se_event_trace(SE_Session session,char* buffer,uint32_t capacity,uint32_t* required) SE_NOEXCEPT;
SE_API int32_t SE_CALL se_session_get_value(SE_Session session, double* value) SE_NOEXCEPT;
SE_API int32_t SE_CALL se_session_set_value(SE_Session session, double value) SE_NOEXCEPT;
SE_API int32_t SE_CALL se_session_get_step_count(SE_Session session, uint64_t* count) SE_NOEXCEPT;
SE_API int32_t SE_CALL se_session_get_input(SE_Session session, SE_InputState* input) SE_NOEXCEPT;
SE_API int32_t SE_CALL se_session_get_pointer(SE_Session session, SE_PointerState* input) SE_NOEXCEPT;
SE_API int32_t SE_CALL se_session_run_window(SE_Session session, const char* locale, uint32_t length, int32_t visible, uint64_t maximum_frames) SE_NOEXCEPT;
// 空路径/零帧清除设置；非空路径必须指定大于零的宿主更新帧号，只能在窗口外配置。
SE_API int32_t SE_CALL se_session_set_window_capture(SE_Session session,const char* path,uint32_t length,uint64_t frame) SE_NOEXCEPT;
// 可选 Player 诊断；空路径/零尺寸关闭，活动窗口禁止修改，输出目录不覆盖。
SE_API int32_t SE_CALL se_session_set_window_profile(SE_Session session,const char* path,uint32_t length,uint32_t width,uint32_t height) SE_NOEXCEPT;
SE_API int32_t SE_CALL se_session_request_stop(SE_Session session) SE_NOEXCEPT;
SE_API int32_t SE_CALL se_session_set_locale(SE_Session session, const char* locale, uint32_t length) SE_NOEXCEPT;
SE_API int32_t SE_CALL se_session_text(SE_Session session, const char* key, uint32_t length, char* buffer, uint32_t capacity, uint32_t* required) SE_NOEXCEPT;
SE_API int32_t SE_CALL se_session_set_asset_root(SE_Session session,const char* root,uint32_t root_length,const char* cache,uint32_t cache_length) SE_NOEXCEPT;
SE_API int32_t SE_CALL se_assets_list(SE_Session session,char* output,uint32_t capacity,uint32_t* required) SE_NOEXCEPT;
SE_API int32_t SE_CALL se_assets_load_model_info(SE_Session session,const char* id,uint32_t length,SE_ModelInfo* info) SE_NOEXCEPT;
SE_API int32_t SE_CALL se_session_set_editor_callback(SE_Session session, SE_EditorCallback callback, void* context) SE_NOEXCEPT;
SE_API int32_t SE_CALL se_session_run_editor(SE_Session session, const char* root, uint32_t root_length, const char* locale, uint32_t locale_length, int32_t visible, uint64_t maximum_frames, int32_t start_playing) SE_NOEXCEPT;
SE_API int32_t SE_CALL se_editor_set_playing(SE_Session session, int32_t playing) SE_NOEXCEPT;
SE_API int32_t SE_CALL se_session_set_editor_data(SE_Session session,SE_EditorDataCallback callback,void* context) SE_NOEXCEPT;
SE_API int32_t SE_CALL se_editor_inspector_query(SE_Session session,uint64_t entity_stable_id,char* output,uint32_t capacity,uint32_t* required) SE_NOEXCEPT;
SE_API int32_t SE_CALL se_editor_inspector_apply(SE_Session session,uint64_t entity_stable_id,const char* request,uint32_t length) SE_NOEXCEPT;
SE_API int32_t SE_CALL se_session_set_locale(SE_Session session, const char* locale, uint32_t length) SE_NOEXCEPT;
SE_API int32_t SE_CALL se_session_text(SE_Session session, const char* key, uint32_t length, char* buffer, uint32_t capacity, uint32_t* required) SE_NOEXCEPT;
SE_API int32_t SE_CALL se_entity_create(SE_Session session, const char* name_utf8, uint32_t name_length, SE_Entity* entity) SE_NOEXCEPT;
SE_API int32_t SE_CALL se_entity_destroy(SE_Session session, SE_Entity entity) SE_NOEXCEPT;
SE_API int32_t SE_CALL se_entity_is_alive(SE_Session session, SE_Entity entity, int32_t* alive) SE_NOEXCEPT;
SE_API int32_t SE_CALL se_entity_get_enabled(SE_Session session,SE_Entity entity,int32_t* enabled) SE_NOEXCEPT;
SE_API int32_t SE_CALL se_entity_set_enabled(SE_Session session,SE_Entity entity,int32_t enabled) SE_NOEXCEPT;
// 受控实例只传标识，不传托管对象或 C++ 继承布局；句柄仍为独立 64 位代数身份。
SE_API int32_t SE_CALL se_script_component_attach(SE_Session session,SE_Entity owner,uint64_t controlled_id,int32_t enabled,uint64_t* component) SE_NOEXCEPT;
SE_API int32_t SE_CALL se_script_component_remove(SE_Session session,uint64_t component) SE_NOEXCEPT;
SE_API int32_t SE_CALL se_script_component_set_enabled(SE_Session session,uint64_t component,int32_t enabled) SE_NOEXCEPT;
SE_API int32_t SE_CALL se_script_component_state(SE_Session session,uint64_t component,int32_t* alive,int32_t* active) SE_NOEXCEPT;
SE_API int32_t SE_CALL se_script_component_count(SE_Session session,uint32_t* count) SE_NOEXCEPT;
SE_API int32_t SE_CALL se_entity_get_transform(SE_Session session, SE_Entity entity, SE_Transform* transform) SE_NOEXCEPT;
SE_API int32_t SE_CALL se_entity_set_transform(SE_Session session, SE_Entity entity, const SE_Transform* transform) SE_NOEXCEPT;
SE_API int32_t SE_CALL se_entity_get_stable_id(SE_Session session, SE_Entity entity, uint64_t* stable_id) SE_NOEXCEPT;
SE_API int32_t SE_CALL se_entity_find(SE_Session session, uint64_t stable_id, SE_Entity* entity) SE_NOEXCEPT;
SE_API int32_t SE_CALL se_entity_get_name(SE_Session session, SE_Entity entity, char* buffer, uint32_t capacity, uint32_t* required) SE_NOEXCEPT;
SE_API int32_t SE_CALL se_scene_export(SE_Session session, char* buffer, uint32_t capacity, uint32_t* required) SE_NOEXCEPT;
SE_API int32_t SE_CALL se_scene_import(SE_Session session, const char* json_utf8, uint32_t length) SE_NOEXCEPT;
SE_API int32_t SE_CALL se_scene_set_callback(SE_Session session,SE_SceneCallback callback,void* context) SE_NOEXCEPT;
SE_API int32_t SE_CALL se_scene_create(SE_Session session,const char* name,uint32_t length,uint64_t* scene) SE_NOEXCEPT;
SE_API int32_t SE_CALL se_scene_request_file(SE_Session session,uint64_t scene,const char* path,uint32_t length,uint32_t mode,uint64_t replacing,uint64_t* request) SE_NOEXCEPT;
SE_API int32_t SE_CALL se_scene_request_json(SE_Session session,uint64_t scene,const char* json,uint32_t length,uint32_t mode,uint64_t replacing,uint64_t* request) SE_NOEXCEPT;
SE_API int32_t SE_CALL se_scene_cancel(SE_Session session,uint64_t scene) SE_NOEXCEPT;
SE_API int32_t SE_CALL se_scene_unload(SE_Session session,uint64_t scene) SE_NOEXCEPT;
SE_API int32_t SE_CALL se_scene_get(SE_Session session,uint64_t scene,char* buffer,uint32_t capacity,uint32_t* required) SE_NOEXCEPT;
SE_API int32_t SE_CALL se_scene_stats(SE_Session session,char* buffer,uint32_t capacity,uint32_t* required) SE_NOEXCEPT;
SE_API int32_t SE_CALL se_scene_active(SE_Session session,uint64_t* scene) SE_NOEXCEPT;
SE_API int32_t SE_CALL se_scene_of_entity(SE_Session session,SE_Entity entity,uint64_t* scene) SE_NOEXCEPT;
SE_API int32_t SE_CALL se_scene_create_entity(SE_Session session,uint64_t scene,const char* name,uint32_t length,SE_Entity* entity) SE_NOEXCEPT;
SE_API int32_t SE_CALL se_scene_callback_scope(SE_Session session,uint64_t scene,uint64_t* previous) SE_NOEXCEPT;
// 每实体最多一个 box body，只允许根实体；物理拥有位置/旋转写权，移动必须使用 teleport。
SE_API int32_t SE_CALL se_body_create(SE_Session session, SE_Entity entity, const SE_BoxBodyDesc* description, SE_Body* body) SE_NOEXCEPT;
SE_API int32_t SE_CALL se_body_destroy(SE_Session session, SE_Body body) SE_NOEXCEPT;
SE_API int32_t SE_CALL se_body_get_state(SE_Session session, SE_Body body, SE_BodyState* state) SE_NOEXCEPT;
SE_API int32_t SE_CALL se_body_set_velocity(SE_Session session, SE_Body body, SE_Vector3 linear, SE_Vector3 angular) SE_NOEXCEPT;
SE_API int32_t SE_CALL se_body_apply_force(SE_Session session, SE_Body body, SE_Vector3 force, SE_Vector3 world_point, int32_t impulse) SE_NOEXCEPT;
SE_API int32_t SE_CALL se_body_teleport(SE_Session session, SE_Body body, const SE_Transform* transform, int32_t clear_velocity) SE_NOEXCEPT;
SE_API int32_t SE_CALL se_physics_step(SE_Session session, double delta_seconds) SE_NOEXCEPT;
// 原子替换调试绘制状态并返回旧值，物理模拟不受影响。
SE_API int32_t SE_CALL se_physics_set_debug_draw(SE_Session session, int32_t enabled, int32_t* previous) SE_NOEXCEPT;
SE_API int32_t SE_CALL se_physics_advance(SE_Session session, double delta_seconds, uint32_t* steps, double* dropped_seconds) SE_NOEXCEPT;
SE_API int32_t SE_CALL se_physics_raycast(SE_Session session, SE_Vector3 origin, SE_Vector3 direction, float distance, uint32_t mask, SE_RayHit* hit, int32_t* found) SE_NOEXCEPT;
// 查询数量不会消费事件；缓冲区足够时一次复制并清空，回调由托管层在步骤结束后分发。
SE_API int32_t SE_CALL se_physics_drain_contacts(SE_Session session, SE_ContactEvent* buffer, uint32_t capacity, uint32_t* required) SE_NOEXCEPT;
SE_API int32_t SE_CALL se_character_create(SE_Session session, SE_Entity entity, const SE_CharacterDesc* description) SE_NOEXCEPT;
SE_API int32_t SE_CALL se_character_move(SE_Session session, SE_Entity entity, SE_Vector3 direction, int32_t jump, float seconds, SE_CharacterState* state) SE_NOEXCEPT;
SE_API int32_t SE_CALL se_character_teleport(SE_Session session, SE_Entity entity, SE_Vector3 position) SE_NOEXCEPT;
SE_API int32_t SE_CALL se_character_destroy(SE_Session session, SE_Entity entity) SE_NOEXCEPT;
SE_API int32_t SE_CALL se_ui_create(SE_Session session, int32_t kind, uint64_t parent, uint64_t* id) SE_NOEXCEPT;
SE_API int32_t SE_CALL se_ui_remove(SE_Session session, uint64_t id) SE_NOEXCEPT;
SE_API int32_t SE_CALL se_ui_set_style(SE_Session session, uint64_t id, const SE_UiStyle* style) SE_NOEXCEPT;
SE_API int32_t SE_CALL se_ui_set_text(SE_Session session, uint64_t id, const char* text, uint32_t length) SE_NOEXCEPT;
SE_API int32_t SE_CALL se_ui_set_image(SE_Session session, uint64_t id, const char* asset_id, uint32_t length) SE_NOEXCEPT;
SE_API int32_t SE_CALL se_ui_exists(SE_Session session, uint64_t id, int32_t* exists) SE_NOEXCEPT;
// entity=0 且两个 ID 长度为 0 时清除该事件；其他情况遵守 UiEventBinding schema。
SE_API int32_t SE_CALL se_ui_bind_action(SE_Session session,uint64_t id,int32_t event_kind,uint64_t entity,
    const char* script,uint32_t script_length,const char* action,uint32_t action_length) SE_NOEXCEPT;
SE_API int32_t SE_CALL se_ui_read_bindings(SE_Session session,uint64_t id,char* buffer,uint32_t capacity,uint32_t* required) SE_NOEXCEPT;
SE_API int32_t SE_CALL se_ui_fade(SE_Session session, uint64_t id, float opacity, double seconds) SE_NOEXCEPT;
SE_API int32_t SE_CALL se_ui_move(SE_Session session, uint64_t id, float x, float y, double seconds) SE_NOEXCEPT;
SE_API int32_t SE_CALL se_ui_color(SE_Session session, uint64_t id, float r, float g, float b, float a, double seconds) SE_NOEXCEPT;
// property: 0=value, 1=visible, 2=enabled, 3=focus, 4=blocksPointer（仅 0/1）, 5=step, 6=cancelTransitions（仅 1）。range 使用独立接口。
SE_API int32_t SE_CALL se_ui_set_property(SE_Session session, uint64_t id, int32_t property, double value) SE_NOEXCEPT;
SE_API int32_t SE_CALL se_ui_set_range(SE_Session session, uint64_t id, double minimum, double maximum) SE_NOEXCEPT;
SE_API int32_t SE_CALL se_ui_layout(SE_Session session, float width, float height) SE_NOEXCEPT;
// operation: 0=move,1=down,2=up,3=wheel,4=cancel；consumed 用于输入路由。
SE_API int32_t SE_CALL se_ui_pointer(SE_Session session, int32_t operation, float x, float y, float wheel, int32_t* consumed) SE_NOEXCEPT;
SE_API int32_t SE_CALL se_ui_key(SE_Session session, uint32_t key, int32_t shift, int32_t control, int32_t* consumed) SE_NOEXCEPT;
SE_API int32_t SE_CALL se_ui_input_text(SE_Session session, const char* text, uint32_t length, int32_t composition, int32_t* consumed) SE_NOEXCEPT;
SE_API int32_t SE_CALL se_ui_consumes_keyboard(SE_Session session, int32_t* consumed) SE_NOEXCEPT;
// JSON 输出 caller-owned；kind: 0=文档，1=视觉快照，2=待派发事件（成功复制后消费）。
SE_API int32_t SE_CALL se_ui_read_json(SE_Session session, int32_t kind, char* buffer, uint32_t capacity, uint32_t* required) SE_NOEXCEPT;
SE_API int32_t SE_CALL se_ui_load_json(SE_Session session, const char* text, uint32_t length) SE_NOEXCEPT;

#ifdef __cplusplus
}
#endif
