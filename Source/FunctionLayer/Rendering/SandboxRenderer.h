#pragma once

#include "FunctionLayer/Rendering/Renderer.h"
#include "PlatformLayer/Graphics/Rhi.h"
#include "FunctionLayer/Rendering/RenderTelemetry.h"
#include "FunctionLayer/Rendering/MaterialInstance.h"
#include "FunctionLayer/Runtime/Camera.h"
#include "StaticModelRender.h"
#include "LodSelect.h"
#include "HlodSelect.h"
#include "HlodStream.h"
#include "HlodStreamer.h"
#include "HlodProxyCache.h"
#include "EnvironmentRender.h"
#include "PostProcessRenderer.h"
#include "FrameGraph.h"
#include "Visibility.h"
#include "Instancing.h"
#include "GuiRenderer.h"
#include "GuiCompositor.h"
#include "EditorViewportTarget.h"
#include "GameUIRenderer.h"
#include "ColorLutFile.h"
#include <unordered_set>

// 共用图形宿主；样例可显式启用三角形，世界/UI 绘制按各自任务接入。
enum class SandboxScene { Clear, Triangle, TexturedCube, Scene, Materials, Lighting, Shadows, Environment, PostProcess, Benchmark, BasicLit, Hlod };
struct SandboxDrawStatistics {std::uint64_t candidates=0,visible=0,draws=0,culled=0,boundsUpdates=0,instances=0,instanceBatches=0,instanceCapacity=0,debugLines=0,occludedInstances=0;};
class SandboxRenderer final : public Renderer
{
public:
    explicit SandboxRenderer(bool drawTriangle = false) : scene_(drawTriangle?SandboxScene::Triangle:SandboxScene::Clear) {}
    explicit SandboxRenderer(SandboxScene scene,std::filesystem::path resources={}) : scene_(scene),resourceDirectory_(std::move(resources)),models_(scene==SandboxScene::Scene||scene==SandboxScene::Materials||scene==SandboxScene::Lighting||scene==SandboxScene::Shadows||scene==SandboxScene::Environment||scene==SandboxScene::PostProcess||scene==SandboxScene::Benchmark||scene==SandboxScene::BasicLit||scene==SandboxScene::Hlod) {}
    ~SandboxRenderer() override { Shutdown(); }
    bool Initialize(const RenderSurface& surface, const Localization& localization) override;
    RenderResult Render(const RenderFrame& frame, const Localization& localization) override;
    EditorRenderView QueryEditorView() const override;
    void Shutdown() noexcept override;
    void SetProfileOutput(std::filesystem::path directory){profileOutput_=std::move(directory);}
    bool ProfileExportSucceeded() const noexcept {return !profileExportFailed_;}
    void SetCaptureOutput(std::filesystem::path path,std::uint64_t frame){captureOutput_=std::move(path);captureFrame_=frame;captured_=false;}
    bool CaptureSucceeded() const noexcept {return captureOutput_.empty()||captured_;}
    se::rhi::Statistics Statistics() const {return device_?device_->Stats():se::rhi::Statistics{};}
    std::vector<std::string> DebugMessages()const{return device_?device_->DebugMessages():std::vector<std::string>{};}
    const se::RenderTelemetry& Telemetry() const noexcept {return telemetry_;}
    se::rhi::AdapterInformation Adapter() const {return device_?device_->Adapter():se::rhi::AdapterInformation{};}
    SandboxDrawStatistics DrawStatistics() const {return drawStatistics_;}
    se::FrameGraphStats FrameGraphStatistics() const noexcept {return frameGraph_.Stats();}
    se::GameUiRenderStatistics UiStatistics()const{return gameUi_.Statistics();}
    std::vector<se::rhi::GpuTiming> GpuTimingsSince(std::uint64_t submission){return device_?device_->GpuTimingsSince(submission):std::vector<se::rhi::GpuTiming>{};}
    se::rhi::Result WaitIdle(){return device_?device_->WaitIdle():se::rhi::Result{se::rhi::Error::InvalidState};}

private:
    std::filesystem::path ResourceDirectory() const;
    // G15：把当前 `.selut` 晶格烘成 1089×33 显示线性 strip；失败保留旧 strip。
    // G15.09：默认 Rgba16Float（保留 >1 值与精度），SELFENGINE_LUT_8BIT=1 回退 Rgba8 做 A/B。
    bool UpdateLutStrip(const Localization& localization);
    std::shared_ptr<se::rhi::Device> device_;
    SandboxScene scene_=SandboxScene::Clear;
    std::filesystem::path resourceDirectory_;
    se::rhi::Pipeline pipeline_;
    se::rhi::Pipeline instancedPipeline_;
    // G06.10：Forward 实例化（布局 30）与只读深度变体；仅在 Lighting 场景按需创建。
    se::rhi::Pipeline forwardInstancedPipeline_;
    se::rhi::Pipeline forwardOcclusionPipeline_;
    se::rhi::Pipeline compressedPipeline_;
    se::PbrInstanceBatcher instances_;
    se::rhi::Pipeline outputPipeline_;
    se::rhi::Pipeline skyPipeline_;
    se::rhi::Pipeline shadowPipeline_;
    std::shared_ptr<const se::StaticRenderModel> cube_;
    std::shared_ptr<const se::StaticRenderModel> sphere_;
    se::StaticModelRenderCache models_;
    se::EnvironmentRenderCache environments_;
    se::PostProcessRenderer post_;
    se::GuiRenderer gui_;
    std::unique_ptr<se::GuiCompositor> editorGui_;
    std::unique_ptr<se::EditorViewportTargetResource> editorTarget_;
    se::GameUIRenderer gameUi_;
    se::WorldBoundsCache bounds_;
    std::unordered_set<std::string> reportedDiagnostics_;
    std::optional<se::CameraSnapshot> camera_;
    se::CameraError lastCameraError_ = se::CameraError::None;
    bool lastMultiplePrimary_ = false, lastRejectedPrimary_ = false, controlsReported_ = false;
    std::filesystem::path profileOutput_;
    bool profileExportFailed_=false;
    se::RenderTelemetry telemetry_;
    std::uint64_t timingCursor_=0;
    std::uint64_t frameTriangles_=0;
    std::uint64_t reportedBc1Uploads_=0;
    bool lodEnabled_=false;
    // G05.08 A/B 钩子：SELFENGINE_LOD_LEVEL=0..3 固定级别并跳过过渡，仅用于同相机参照图。
    std::uint32_t forcedLodLevel_=UINT32_MAX;
    // G05.08：过渡状态按“模型 + 实体”区分；同一内建模型可被多个实体复用，不能只按模型指针。
    struct LodTransitionKey
    {
        const se::StaticRenderModel* model=nullptr;
        std::uint64_t entity=0;
        bool operator==(const LodTransitionKey&)const=default;
    };
    struct LodTransitionKeyHash
    {
        std::size_t operator()(const LodTransitionKey& key)const noexcept
        {
            const auto model=std::hash<const void*>{}(key.model),entity=std::hash<std::uint64_t>{}(key.entity);
            return model^(entity+0x9e3779b97f4a7c15ull+(model<<6)+(model>>2));
        }
    };
    std::unordered_map<LodTransitionKey,se::LodTransition,LodTransitionKeyHash> lodTransitions_;
    // G05.10：HLOD 运行时接入。单元与代理按实例集合缓存，实例数变化时整体重建。
    bool hlodEnabled_=false,hlodBaked_=false,hlodFailed_=false;
    std::uint32_t hlodLevel_=0;
    std::size_t hlodInstanceCount_=0;
    std::vector<se::HlodCell> hlodCells_;
    std::vector<std::shared_ptr<const se::StaticRenderModel>> hlodProxies_;
    std::vector<std::vector<std::shared_ptr<const se::PbrRenderMaterial>>> hlodOverrides_;
    std::vector<bool> hlodProxyActive_;
    std::vector<std::int32_t> hlodCandidateCell_;
    // G05.11：`.sehlod` 加载与按相机半径驻留；非驻留单元回退到实例绘制。
    se::HlodStream hlodStream_;
    // G05.13：后台读取 `.sehlod` + 代理显存预算/每帧上传上限；不启用时保持 G05.11 的全量同步行为。
    se::HlodStreamer hlodStreamer_;
    se::HlodProxyCache hlodProxyCache_;
    bool hlodAsync_=false,hlodLazyProxies_=false;
    std::uint32_t hlodUploadBudget_=0;
    std::uint64_t hlodVramBudget_=0;
    std::vector<std::uint64_t> hlodProxyBytes_;
    std::filesystem::path hlodFilePath_,hlodSavePath_;
    bool hlodFileLoaded_=false,hlodSaved_=false;
    float hlodResidentRadius_=0;
    // G05.12：16 视角 Impostor。远于阈值的实例改画朝向相机的四边形，alpha test 保留轮廓。
    bool impostorEnabled_=false;
    float impostorDistance_=1.0e9f;
    se::rhi::Texture impostorTexture_;
    se::rhi::Pipeline impostorPipeline_;
    std::shared_ptr<const se::StaticRenderModel> impostorQuad_;
    se::VariantInventory variants_;
    // G13：SELFENGINE_FRAMEGRAPH=1 时改用 Frame Graph 编译出的 Pass 顺序；
    // DOT/JSON 只在首帧导出一次，默认关闭保持 R 系列手写路径。
    se::FrameGraph frameGraph_;
    bool frameGraphEnabled_=false,frameGraphDumped_=false;
    std::filesystem::path frameGraphDotPath_,frameGraphJsonPath_;
    // G15：SELFENGINE_AUTO_EXPOSURE=1 启用 GPU 直方图曝光；SELFENGINE_LUT=1 启用 33³ LUT。
    // 两者默认关闭，保持既有 Reinhard/CPU 曝光路径逐字节不变。
    bool autoExposureEnabled_=false,lutEnabled_=false;
    // G15：SELFENGINE_LUT_FILE 选中的 `.selut` 每帧按修改时间热重载；失败时保留上一版。
    se::ColorLutFile lutFile_;
    std::uint64_t reportedLutFailures_=0;
    // G15：输出 LUT。BasicLit（ShadowOutput）取编辑器选择，Lighting（FinalOutput）取 SELFENGINE_LUT_FILE；
    // strip 为 1089×33 显示线性（Rgba16Float，可选 Rgba8）；禁用时绑定 1×1 占位并由 Shader 跳过。
    bool outputLutScene_=false;
    std::filesystem::path editorLutPath_;
    se::rhi::Texture lutStrip_,lutDummy_;
    std::uint64_t lutStripRevision_=0;
    bool lutStripReady_=false,lutStrip16F_=true;
    // G06.08：SELFENGINE_OCCLUSION=1 时，单一实例批次的场景改用 RHI 遮挡批次
    // （prepass → HZB → 间接剔除 → ExecuteIndirect）；多批次/HLOD/Impostor/LOD 帧回退普通实例绘制。
    bool occlusionEnabled_=false;
    // G06.09：SELFENGINE_OCCLUSION_HUD=1 时请求 48B 参数回读，并在场景 UI 之后画 HUD。
    // 默认关闭；occlusionCommitted_ 记录上一帧是否真的提交了遮挡批次，避免回退帧显示陈旧计数。
    bool occlusionHudEnabled_=false,occlusionCommitted_=false;
    // G06.11：SELFENGINE_OCCLUSION_TIMING=1 时请求每批次 cull 与 prepass/HZB/main 的 GPU 时间。
    bool occlusionTimingEnabled_=false;
    se::rhi::Pipeline occlusionPipeline_;
    // G14：SELFENGINE_TAA=1 启用抖动、速度缓冲与历史；上一帧矩阵/世界用于重投影。
    bool taaEnabled_=false;
    se::Mat4 previousViewProjection_{};
    bool previousViewProjectionValid_=false;
    std::uint32_t lastSurfaceWidth_=0,lastSurfaceHeight_=0;
    std::unordered_map<se::StableEntityId,se::Mat4> previousWorlds_;
    std::filesystem::path captureOutput_;
    std::uint64_t captureFrame_=0;
    bool captured_=false;
    SandboxDrawStatistics drawStatistics_;
};
