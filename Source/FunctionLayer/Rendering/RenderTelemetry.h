#pragma once
#include "PlatformLayer/Graphics/Rhi.h"
#include <array>
#include <cstddef>
#include <cstdint>
#include <deque>
#include <filesystem>
#include <span>
#include <string>

namespace se
{
// G01：固定的渲染 Pass 槽位。timestamp、PIX marker 与 CSV 列共用同一枚举，
// 新增 Pass 只能使用 Other 之外的预留槽，不允许运行时拼接名称。
enum class RenderPassId : std::uint8_t
{
    Shadow,
    Prepass,
    Hzb,
    ClusterCull,
    Sky,
    Opaque,
    AmbientOcclusion,
    ScreenSpaceReflection,
    Fog,
    TemporalAA,
    Bloom,
    Exposure,
    ToneMapping,
    Gui,
    Composite,
    Other,
    Count
};

inline constexpr std::size_t RenderPassCount = static_cast<std::size_t>(RenderPassId::Count);
static_assert(RenderPassCount == 16, "G01 约定 16 个 Pass 槽位");

inline constexpr const char* RenderPassName(RenderPassId id) noexcept
{
    switch (id)
    {
    case RenderPassId::Shadow: return "Shadow";
    case RenderPassId::Prepass: return "Prepass";
    case RenderPassId::Hzb: return "Hzb";
    case RenderPassId::ClusterCull: return "ClusterCull";
    case RenderPassId::Sky: return "Sky";
    case RenderPassId::Opaque: return "Opaque";
    case RenderPassId::AmbientOcclusion: return "AmbientOcclusion";
    case RenderPassId::ScreenSpaceReflection: return "ScreenSpaceReflection";
    case RenderPassId::Fog: return "Fog";
    case RenderPassId::TemporalAA: return "TemporalAA";
    case RenderPassId::Bloom: return "Bloom";
    case RenderPassId::Exposure: return "Exposure";
    case RenderPassId::ToneMapping: return "ToneMapping";
    case RenderPassId::Gui: return "Gui";
    case RenderPassId::Composite: return "Composite";
    default: return "Other";
    }
}

// G01：一帧的固定字段统计。HUD 与 CSV 只读该结构，不在渲染路径内格式化或分配。
struct RenderStats
{
    std::uint64_t frameIndex=0;
    std::uint64_t submission=0; // 与 GpuTiming::submission 对齐，用于迟到时间戳回填。
    float cpuPrepareMilliseconds=0;
    std::array<float,RenderPassCount> gpuPassMilliseconds{};
    std::uint32_t drawCalls=0,instances=0,triangles=0,stateChanges=0;
    std::uint32_t psoCreated=0,uploadBytes=0,descriptorUsed=0,vramUsedMiB=0;
    std::array<RenderPassId,3> topPasses{};
    std::array<float,3> topPassMilliseconds{};
    const char* longFrameReason=nullptr;

    float GpuTotalMilliseconds() const noexcept
    {
        float total=0;
        for (const float value : gpuPassMilliseconds) total+=value;
        return total;
    }
};

// G01：固定 240 帧的遥测环。计数逐帧采集；GPU 时间戳晚到，按 submission 回填。
class RenderTelemetry
{
public:
    static constexpr std::size_t Capacity=240;

    explicit RenderTelemetry(float budgetMilliseconds=16.67f) noexcept;
    void SetBudget(float budgetMilliseconds) noexcept;
    float Budget() const noexcept {return budget_;}
    void BeginFrame(std::uint64_t frameIndex,const rhi::Statistics& stats);
    void EndFrame(const rhi::Statistics& stats,std::uint32_t draws,std::uint32_t instances,std::uint32_t triangles,float cpuMilliseconds);
    void ObserveTimings(std::span<const rhi::GpuTiming> timings) noexcept;
    const RenderStats* Latest() const noexcept {return frames_.empty()?nullptr:&frames_.back();}
    const std::deque<RenderStats>& Frames() const noexcept {return frames_;}
    std::size_t LongFrames() const noexcept {return longFrames_;}
    bool ExportCsv(const std::filesystem::path& path,std::string& error) const;
    void Reset() noexcept;

private:
    void Publish(const RenderStats& stats);
    void ApplyTiming(RenderStats& stats,const rhi::GpuTiming& timing) const noexcept;

    float budget_=16.67f;
    rhi::Statistics previous_{};
    std::deque<RenderStats> frames_;
    std::size_t longFrames_=0;
    bool frameOpen_=false;
    std::uint64_t frameIndex_=0;
    float cpuMilliseconds_=0;
};
}
