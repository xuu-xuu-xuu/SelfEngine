#pragma once
#include "PlatformLayer/Graphics/VisibilityData.h"
#include "PlatformLayer/Graphics/Rhi.h"
#include "CoreLayer/Profiler.h"
#include <nlohmann/json.hpp>
#include <memory>

class Localization;
struct D3D12Context;
struct D3D12Presentation;
namespace se
{
struct IndirectScene
{
    std::vector<rhi::InstanceParameters> instances;
    std::vector<VisibilityItem> bounds;
    Mat4 viewProjection;
    VisibilityFrustum frustum;
};
IndirectScene BuildIndirectScene(std::uint32_t count,std::uint32_t visibleRate,bool equalDepth=false);
// G06.05：大遮挡体 + 后方隐藏实例 + 前方/外侧可见实例，用于遮挡开关逐像素对照。
IndirectScene BuildOcclusionScene();
// G06.06：重着色净收益场景。400 个隐藏实例铺满遮挡体投影、400 个前方可见实例，
// 遮挡率约 50%；关闭遮挡时后方实例先着色再被覆盖，开启后由 HZB 整批剔除。
IndirectScene BuildDenseOcclusionScene();
struct IndirectFrame
{
    std::uint64_t sequence=0;
    bool gpu=false,occlusion=false;
    std::uint32_t inputCount=0,cpuVisible=0;
    double cpuValidationMs=0,cpuCullMs=0,cpuGatherMs=0,cpuRenderMs=0,cpuFrameMs=0;
    rhi::GpuTiming drawing;
    std::array<std::uint64_t,5> indirectTicks{};
    std::array<std::uint64_t,3> hizTicks{};
};
// G06.06：HUD/证据共用的遮挡计数。visible 来自间接参数读回，不由 CPU 估算。
struct IndirectOcclusionCounters
{
    std::uint32_t candidates=0,visible=0,culled=0;
    float occlusionRate=0;
    bool enabled=false;
};
// 独立Tool小样；CPU基线和GPU路径共用原Native Presentation/PBR绑定，输出受其双帧Fence保护。
class IndirectExperiment
{
public:
    IndirectExperiment(const Localization& text,std::uint32_t capacity,bool visible=false);
    ~IndirectExperiment();
    IndirectExperiment(const IndirectExperiment&)=delete;
    IndirectExperiment& operator=(const IndirectExperiment&)=delete;
    rhi::Image Render(const IndirectScene& scene,bool gpu,bool capture=false,bool occlusion=false);
    bool Pump(bool& toggleRequested);
    void Resize(std::uint32_t width,std::uint32_t height);
    void Drain();
    // G06.06：读取最近一次 GPU 渲染的间接参数实例数；调用方需先 Drain，确保该帧完成。
    std::uint32_t LastVisibleCount();
    // G06.06：HZB 可视化读回（mip0，R32_FLOAT）与 HUD 计数；两者都要先 Drain。
    bool ReadHiz(std::vector<float>& depth,std::uint32_t& width,std::uint32_t& height);
    IndirectOcclusionCounters LastOcclusionCounters();
    void Shutdown();
    std::vector<IndirectFrame> Frames() const;
    std::vector<CpuFrame> CpuFrames() const;
    nlohmann::json Statistics() const;
    // 检验受控队列Fence；不得另绑队列或绕过宿主改写在途资源。
    D3D12Context& Context();
    D3D12Presentation& Presentation();
private:
    struct Impl;std::unique_ptr<Impl> impl_;
};
}
