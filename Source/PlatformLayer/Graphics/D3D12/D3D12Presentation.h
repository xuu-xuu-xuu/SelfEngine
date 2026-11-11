#pragma once

#include <d3d12.h>
#include <dxgi1_4.h>
#include <wrl/client.h>
#include <wrl/wrappers/corewrappers.h>
#include <array>
#include "CoreLayer/Math/Projection.h"
#include "D3D12Mesh.h"
#include "D3D12UploadArena.h"
#include "D3D12StateTracker.h"
#include <cstddef>
#include <deque>
#include <unordered_map>
struct D3D12ShadowTarget;
struct D3D12PostTargets;
struct D3D12GuiContent;
struct D3D12GuiTarget;
struct D3D12GuiLayer;
struct D3D12IndirectBatch;
// G06.11：RHI 运行时遮挡时间查询。queries 为与 batch.timestamps 相同的 TIMESTAMP 堆；
// 呈现层记录 prepass 边界与全部间接绘制结束，RHI 在 Fence 完成后读取。
struct D3D12OcclusionTelemetry
{
    ID3D12QueryHeap* queries=nullptr;
};
namespace se {class Profiler;}

class Localization;
struct D3D12Context;
struct D3D12Triangle;
class D3D12StateRegistry;
class D3D12MemoryAllocator;

// 调用期间借用描述；readback 资源需由调用者保持到提交完成后，才能读回或释放。
// 复制在 Present 前记录，避免依赖 flip-discard 呈现后的未定义缓冲内容。
struct D3D12FrameCapture
{
    ID3D12Resource* readback = nullptr;
    D3D12_PLACED_SUBRESOURCE_FOOTPRINT footprint{};
};

enum class D3D12WaitReason : size_t { FrameReuse, Resize, Shutdown, Readback, Upload, Count };

struct D3D12FrameContext
{
    Microsoft::WRL::ComPtr<ID3D12CommandAllocator> allocator;
    UINT64 fenceValue = 0;
    D3D12UploadArena uploads;
    D3D12UploadArena baselineConstants; // 保留 T19 的每帧 4KB 常量缓冲作为实测基线。
    std::vector<D3D12UploadSlice> drawConstants;
    std::vector<D3D12UploadSlice> instanceSlices;
    std::vector<Microsoft::WRL::ComPtr<ID3D12Resource>> uploadDestinations;
    D3D12FrameDescriptors descriptors;
    UINT constantCursor = 0;
    Microsoft::WRL::ComPtr<ID3D12QueryHeap> timestampQueries;
    Microsoft::WRL::ComPtr<ID3D12Resource> timestampReadback;
    bool timestampsPending=false;
    UINT64 timestampFrame=0,timestampSubmission=0;
    UINT timestampDraws=0;
    bool timestampForward=false;
    D3D12UploadSlice forwardConstants;
    D3D12FrameDescriptors outputDescriptors;
    D3D12FrameDescriptors environmentDescriptors;
    D3D12FrameDescriptors postDescriptors;
    D3D12FrameDescriptors guiDescriptors;
    D3D12FrameDescriptors gameUiDescriptors;
    std::shared_ptr<D3D12ShadowTarget> shadow;
    bool timestampShadow=false;
    bool timestampPost=false,timestampBloom=false,timestampToneMapping=false,timestampFxaa=false,timestampUi=false;
};

struct D3D12FrameContent
{
    const D3D12Triangle* triangle = nullptr;
    const D3D12MeshPipeline* meshPipeline = nullptr;
    std::span<const D3D12MeshDraw> meshes;
    se::Mat4 viewProjection;
    UINT64 sourceFrame=0;
    const se::rhi::ForwardLighting* forward=nullptr;
    const D3D12MeshPipeline* outputPipeline=nullptr;
    const D3D12MeshPipeline* skyPipeline=nullptr;
    const D3D12FrameCapture* hdrCapture=nullptr;
    const se::rhi::ShadowSettings* shadow=nullptr;
    const D3D12MeshPipeline* shadowPipeline=nullptr;
    std::span<const D3D12MeshDraw> shadowMeshes;
    const D3D12FrameCapture* shadowCapture=nullptr;
    const se::rhi::EnvironmentBindings* environment=nullptr;
    std::array<const D3D12Texture*,3> environmentTextures{};
    // G15：输出 LUT 布局的 ShadowOutput 采样源；由 ForwardLighting::colorLut 解析。
    const D3D12Texture* colorLut=nullptr;
    const se::rhi::PostProcessSettings* post=nullptr;
    std::array<const D3D12MeshPipeline*,5> postPipelines{};
    std::span<const se::rhi::OverlayQuad> overlays;
    bool cacheOpaqueBindings=false;
    const D3D12GuiContent* gui=nullptr;
    const D3D12GuiTarget* sceneHdr=nullptr;
    const D3D12GuiTarget* sceneLdr=nullptr;
    const D3D12GuiLayer* gameUi=nullptr;
    // G06.10：多批次遮挡；与 content.meshes 一一对应，空表示普通绘制。
    const D3D12IndirectBatch* indirects=nullptr;
    std::size_t indirectCount=0;
    // G06.11：可选运行时遮挡时间；只在所有批次都启用时间戳且 HZB 开启时使用。
    const D3D12OcclusionTelemetry* occlusionTelemetry=nullptr;
};

// 与一个窗口关联的呈现资源；应用持有，设备上下文需比它活得更久。
struct D3D12Presentation
{
    static constexpr UINT bufferCount = 2;
    static constexpr UINT frameCount = 2;
    static constexpr DXGI_FORMAT bufferFormat = DXGI_FORMAT_R8G8B8A8_UNORM;
    static constexpr DXGI_FORMAT depthFormat = DXGI_FORMAT_D32_FLOAT;

    // 提交后的常规退出先调用显式释放函数，确认 GPU 不再使用这些资源。
    Microsoft::WRL::ComPtr<ID3D12CommandQueue> queue;
    Microsoft::WRL::ComPtr<IDXGISwapChain3> swapChain;
    std::shared_ptr<D3D12DescriptorRange> rtvs,gameUiRtvs,dsv;
    std::shared_ptr<D3D12DescriptorRange> hdrRtv,hdrSrv;
    std::shared_ptr<D3D12DescriptorPool> srvPool,rtvPool,dsvPool;
    std::shared_ptr<D3D12ShadowTarget> shadow;
    std::unordered_map<UINT,std::weak_ptr<D3D12ShadowTarget>> shadowTargets;
    UINT64 shadowCreations=0;
    Microsoft::WRL::ComPtr<ID3D12Resource> hdr;
    UINT64 hdrCreations=0;
    std::shared_ptr<D3D12PostTargets> post;
    UINT64 postCreations=0;
    Microsoft::WRL::ComPtr<ID3D12Resource> depth;
    std::array<Microsoft::WRL::ComPtr<ID3D12Resource>, bufferCount> backBuffers;
    std::array<D3D12FrameContext, frameCount> frames;
    Microsoft::WRL::ComPtr<ID3D12GraphicsCommandList> commandList;
    Microsoft::WRL::ComPtr<ID3D12Fence> fence;
    Microsoft::WRL::Wrappers::Event fenceEvent;
    UINT64 nextFenceValue = 1;
    UINT frameCursor = 0; // 独立于 GetCurrentBackBufferIndex，不按交换链索引复用 allocator。
    UINT64 submittedFrames = 0;
    se::rhi::DrawBindingStatistics opaqueBindings;
    UINT64 frameInstances=0,frameInstanceBytes=0,frameDrawConstantBytes=0;
    UINT64 signalCount = 0;
    std::array<UINT64, static_cast<size_t>(D3D12WaitReason::Count)> waitCounts{};
    std::array<UINT64, static_cast<size_t>(D3D12WaitReason::Count)> flushCounts{};
    bool gpuWorkPending = false;
    bool untrackedSubmission = false;
    bool failed = false;
    bool occluded = false;
    UINT width = 0;
    UINT height = 0;
    std::shared_ptr<D3D12StateRegistry> states;
    std::shared_ptr<D3D12MemoryAllocator> memory;
    std::unique_ptr<D3D12StateRecording> uploadStates;
    UINT64 uploadSubmissions=0,uploadedBytes=0;
    bool dedicatedUploads=false;
    se::Profiler* cpuProfiler=nullptr; // 呈现线程借用，须覆盖本次记录/等待。
    bool timingEnabled=false;
    UINT64 timestampFrequency=0,droppedTimingSamples=0;
    std::deque<se::rhi::GpuTiming> timings;
};

// 创建呈现资源、两组 FrameContext、共用命令列表和 Fence；初始化阶段不提交命令。
// 只接受空输出；失败自动清理临时资源，成功后交给 presentation 持有。
bool InitializeD3D12Presentation(D3D12Presentation& presentation,
    const D3D12Context& context, HWND hwnd, const Localization& localization);

// 普通帧只等待将要复用的 FrameContext；成功返回不保证 GPU 已完成。
bool RenderD3D12Frame(D3D12Presentation& presentation, const Localization& localization,
    const D3D12Triangle* triangle = nullptr, const D3D12FrameCapture* capture = nullptr,
    const se::Mat4& clipFromWorld = {});
bool RenderD3D12Frame(D3D12Presentation& presentation, const Localization& localization,
    const D3D12FrameContent& content, const D3D12FrameCapture* capture = nullptr);

// 保留清屏调用入口，使用相同的多帧寿命规则。
bool RenderD3D12ClearFrame(D3D12Presentation& presentation, const Localization& localization);

// 在队列尾部 Signal 后等待，明确的同步点使用；不在普通帧中全局等待。
bool WaitForD3D12Gpu(D3D12Presentation& presentation, const Localization& localization,
    D3D12WaitReason reason = D3D12WaitReason::Readback);

// 同尺寸和任一零尺寸不操作；调用者在零尺寸期间不绘制。
bool ResizeD3D12Presentation(D3D12Presentation& presentation, UINT width, UINT height,
    const Localization& localization);

// 先收拢在途工作再释放，支持部分初始化和重复调用；无法证明安全时终止而非释放在用资源。
void ReleaseD3D12Presentation(D3D12Presentation& presentation) noexcept;

// 上传和绘制共用 FrameContext；只有完成所属 Fence 后才可复用页/allocator。
bool BeginD3D12FrameCommands(D3D12Presentation& presentation,const Localization& text);
bool BeginD3D12UploadBatch(D3D12Presentation& presentation,const Localization& text);
bool EndD3D12UploadBatch(D3D12Presentation& presentation,UINT64& fence,const Localization& text);
bool WaitD3D12Submission(D3D12Presentation& presentation,UINT64 fence,const Localization& text,D3D12WaitReason reason);
void CollectD3D12UploadReferences(D3D12Presentation& presentation);
