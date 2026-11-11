#pragma once
#include "FunctionLayer/Runtime/World.h"
#include "FunctionLayer/Runtime/Camera.h"
#include "FunctionLayer/Runtime/Environment.h"
#include "FunctionLayer/Physics/PhysicsWorld.h"
#include "FunctionLayer/GameUI/UiTree.h"
#include "PlatformLayer/Graphics/Rhi.h"
#include "ResourceLayer/Assets/MaterialDocument.h"
#include <functional>
#include <filesystem>
#include <Windows.h>
#include <optional>

struct InputState;
struct ImDrawData;
namespace se
{
class AssetStore;
class Profiler;
struct MeshInstance { StableEntityId entity; Mat4 world; MeshComponent mesh; };
struct LightInstance { StableEntityId entity; Mat4 world; LightComponent light; };
struct DirectionalShadowView
{
    Vec3 center{};
    float halfExtent=8,lightDistance=16;
    std::uint32_t resolution=1024;
    float depthBias=0.0008f,slopeBias=0.002f,normalBias=0.015f;
    bool enabled=true,pcf=true,preview=false;
};
struct EnvironmentView {std::string asset;float intensity=1,yaw=0;};
// 一帧自有 CPU 快照，不含可变 World 指针、COM 对象或 GPU 句柄。
struct FrameData
{
    std::vector<MeshInstance> meshes;
    std::vector<CameraInstance> cameras;
    std::optional<StableEntityId> cameraBinding; // 每视口显式绑定；空时按有效 primary 的稳定 ID 选择。
    std::vector<LightInstance> lights;
    std::vector<UiVisual> ui;
    bool referenceUiOverlays=false; // 仅旧后处理基准使用固定矩形路径，普通控件使用 GameUIRenderer。
    std::vector<DebugLine> debugLines;
    UiRect sceneViewport{}; // 编辑器内场景区域；Play UI 使用其宽高，原点位于场景纹理左上。
    const ImDrawData* editorDrawData = nullptr; // 仅本次Render有效；所有权属于编辑器context。
    const unsigned char* editorFontPixels = nullptr; // 借用 RGBA8，渲染器自行上传。
    int editorFontWidth = 0, editorFontHeight = 0;
    std::uint64_t editorFontToken = 1; // CPU 资源标识，绝不是 D3D12 描述符。
    std::uint64_t editorFontRevision = 0; // 图集内容修订；变化时渲染器必须重传字体纹理。
    struct UiImageSource
    {
        std::uint64_t token = 0;
        std::uint32_t width = 0, height = 0;
        const unsigned char* rgba8 = nullptr;
        std::size_t bytes = 0;
    };
    // 本帧借用的编辑器图像（资产缩略图等）；token 由宿主分配，宿主保证像素存活到本帧结束。
    std::vector<UiImageSource> editorUiImages;
    bool editorGuiEnabled=false;
    GuiGeometry editorGui; // Tool 完整复制后的自有数据，Function 不解释 ImGui。
    std::optional<CameraSnapshot> editorCamera; // 当前布局尺寸下的独立编辑镜头，Play 留空。
    std::optional<MaterialPreview> materialPreview;
    AssetStore* assets = nullptr; // 仅主线程、本次 Render 借用；加载得到的 shared_ptr 可由渲染器自行持有。
    float exposure=1;
    Vec3 skyColor{0.02f,0.04f,0.08f}; // 线性 HDR；普通 Sky 的固定背景，环境模式使用 environment。
    DirectionalShadowView shadow;
    EnvironmentView environment;
    std::optional<EnvironmentSnapshot> timeOfDay; // 自有 CPU 昼夜值；GPU 消费另行接入。
    std::optional<rhi::PostProcessSettings> post; // 只有数值；不含 GPU 句柄或 prepared 对象。
    // G15：编辑器 LUT 选择（绝对路径，空表示不启用）；由 BasicLit 输出采样并热重载。
    std::filesystem::path colorLutPath;
    bool frustumCulling=false; // 只裁剪主相机不透明列表，阴影投射列表独立保留。
    bool sortOpaque=false,cacheOpaqueBindings=false; // 两项独立，默认保留原始提交路径。
    bool instancing=false; // 当前仅 PBR/Benchmark，单例也使用实例管线。
};
void BuildFrameData(const World& world,const UiTree* ui,const PhysicsWorld* physics,FrameData& output);
void BuildFrameData(const Environment& environment,const UiTree* ui,const PhysicsWorld* physics,FrameData& output);
struct ApplicationServices
{
    std::filesystem::path resourceDirectory; // 包含 Localization 的资源目录；空则用 exe/Resources。
    std::function<bool(HWND)> windowOpened;
    std::function<void()> windowClosing;
    std::function<std::optional<LRESULT>(HWND,UINT,WPARAM,LPARAM)> messageHandler;
    std::function<bool()> captureKeyboard, capturePointer;
    UiTree* ui = nullptr; // 借用，须覆盖整个 RunSandboxApp 生命周期。
    bool advanceUi = true; // update 自己推进 UI 时关闭，确保每帧只推进一次。
    std::function<void(const InputState&,bool keyboardConsumed,bool pointerConsumed)> input;
    std::function<bool(double)> update;
    std::function<bool(double)> runEnvironmentFrame; // 完整阶段入口；返回后才允许提取快照。
    std::function<void(FrameData&)> extract;
    std::uint64_t maximumFrames = 0; // 0 为正常交互；非零供自动化有限帧验证。
    Profiler* profiler=nullptr; // 借用，可选的应用更新/提取与渲染 CPU 采集。
    std::uint32_t initialWidth=0,initialHeight=0; // 同为 0 使用默认窗口；否则在渲染器初始化前设置客户区。
    double fixedDeltaSeconds=0; // 0 使用真实 dt；受控演示可覆盖更新步长，原始时间仍保留。
};
}
