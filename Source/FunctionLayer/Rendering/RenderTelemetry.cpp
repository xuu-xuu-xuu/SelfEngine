#include "FunctionLayer/Rendering/RenderTelemetry.h"
#include "ResourceLayer/Assets/AssetIO.h"
#include <algorithm>
#include <iomanip>
#include <sstream>

namespace se
{
namespace
{
float Duration(std::uint64_t begin,std::uint64_t end,std::uint64_t frequency) noexcept
{
    if(!frequency||end<begin)return 0;
    return static_cast<float>(static_cast<double>(end-begin)*1000.0/static_cast<double>(frequency));
}

std::uint32_t Delta(std::uint64_t current,std::uint64_t previous) noexcept
{
    return current>=previous?static_cast<std::uint32_t>(current-previous):0;
}
}

RenderTelemetry::RenderTelemetry(float budgetMilliseconds) noexcept : budget_(budgetMilliseconds)
{
}

void RenderTelemetry::SetBudget(float budgetMilliseconds) noexcept
{
    budget_=budgetMilliseconds>0?budgetMilliseconds:16.67f;
}

void RenderTelemetry::BeginFrame(std::uint64_t frameIndex,const rhi::Statistics& stats)
{
    frameOpen_=true;frameIndex_=frameIndex;cpuMilliseconds_=0;previous_=stats;
}

void RenderTelemetry::EndFrame(const rhi::Statistics& stats,std::uint32_t draws,std::uint32_t instances,std::uint32_t triangles,float cpuMilliseconds)
{
    RenderStats current;
    current.frameIndex=frameOpen_?frameIndex_:stats.submittedFrames;
    current.submission=stats.submittedFrames;
    current.cpuPrepareMilliseconds=cpuMilliseconds;
    current.drawCalls=draws;current.instances=instances;current.triangles=triangles;
    current.stateChanges=Delta(stats.stateTransitions,previous_.stateTransitions);
    current.psoCreated=Delta(stats.pipelineCreations,previous_.pipelineCreations);
    current.uploadBytes=Delta(stats.uploadedBytes,previous_.uploadedBytes);
    current.descriptorUsed=stats.persistentDescriptors;
    current.vramUsedMiB=static_cast<std::uint32_t>(stats.memoryUsedBytes/(1024ull*1024ull));
    Publish(current);
    frameOpen_=false;
}

void RenderTelemetry::ApplyTiming(RenderStats& stats,const rhi::GpuTiming& timing) const noexcept
{
    if(!timing.frequency)return;
    const auto ms=[&](std::uint64_t begin,std::uint64_t end){return Duration(begin,end,timing.frequency);};
    if(timing.forward)
    {
        stats.gpuPassMilliseconds[static_cast<std::size_t>(RenderPassId::Sky)]=ms(timing.passTicks[2],timing.passTicks[3]);
        stats.gpuPassMilliseconds[static_cast<std::size_t>(RenderPassId::Opaque)]=ms(timing.passTicks[4],timing.passTicks[5]);
        stats.gpuPassMilliseconds[static_cast<std::size_t>(RenderPassId::Composite)]=ms(timing.passTicks[6],timing.passTicks[7]);
        stats.gpuPassMilliseconds[static_cast<std::size_t>(RenderPassId::Other)]=ms(timing.passTicks[0],timing.passTicks[1]);
    }
    if(timing.shadow)
        stats.gpuPassMilliseconds[static_cast<std::size_t>(RenderPassId::Shadow)]=ms(timing.shadowTicks[0],timing.shadowTicks[1]);
    if(timing.post)
    {
        stats.gpuPassMilliseconds[static_cast<std::size_t>(RenderPassId::Bloom)]=
            ms(timing.postTicks[0],timing.postTicks[1])+ms(timing.postTicks[2],timing.postTicks[3])+ms(timing.postTicks[4],timing.postTicks[5]);
        stats.gpuPassMilliseconds[static_cast<std::size_t>(RenderPassId::ToneMapping)]=ms(timing.postTicks[6],timing.postTicks[7]);
        stats.gpuPassMilliseconds[static_cast<std::size_t>(RenderPassId::Gui)]=ms(timing.postTicks[10],timing.postTicks[11]);
    }
    const float total=static_cast<float>(timing.frameMilliseconds);
    if(total<=budget_*1.25f)return;
    std::array<std::pair<float,RenderPassId>,RenderPassCount> ordered{};
    for(std::size_t index=0;index<RenderPassCount;++index)
        ordered[index]={stats.gpuPassMilliseconds[index],static_cast<RenderPassId>(index)};
    std::partial_sort(ordered.begin(),ordered.begin()+3,ordered.end(),
        [](const auto& left,const auto& right){return left.first>right.first;});
    for(std::size_t index=0;index<3;++index)
    {
        stats.topPasses[index]=ordered[index].second;
        stats.topPassMilliseconds[index]=ordered[index].first;
    }
    stats.longFrameReason=RenderPassName(ordered[0].second);
}

void RenderTelemetry::ObserveTimings(std::span<const rhi::GpuTiming> timings) noexcept
{
    for(const auto& timing:timings)
    {
        const auto found=std::find_if(frames_.begin(),frames_.end(),[&](const RenderStats& stats){return stats.submission==timing.submission;});
        if(found==frames_.end())continue;
        ApplyTiming(*found,timing);
        if(found->longFrameReason)++longFrames_;
    }
}

void RenderTelemetry::Publish(const RenderStats& stats)
{
    frames_.push_back(stats);
    while(frames_.size()>Capacity)frames_.pop_front();
}

void RenderTelemetry::Reset() noexcept
{
    frames_.clear();previous_={};longFrames_=0;frameOpen_=false;frameIndex_=0;cpuMilliseconds_=0;
}

bool RenderTelemetry::ExportCsv(const std::filesystem::path& path,std::string& error) const
{
    std::ostringstream text;
    text<<"frame,submission,cpu_ms,gpu_ms";
    for(std::size_t index=0;index<RenderPassCount;++index)text<<','<<RenderPassName(static_cast<RenderPassId>(index));
    text<<",draws,instances,triangles,state_changes,pso,upload_bytes,descriptors,vram_mib,long_frame,top1,top1_ms,top2,top2_ms,top3,top3_ms\n";
    text<<std::fixed<<std::setprecision(3);
    for(const auto& stats:frames_)
    {
        text<<stats.frameIndex<<','<<stats.submission<<','<<stats.cpuPrepareMilliseconds<<','<<stats.GpuTotalMilliseconds();
        for(const float value:stats.gpuPassMilliseconds)text<<','<<value;
        text<<','<<stats.drawCalls<<','<<stats.instances<<','<<stats.triangles<<','<<stats.stateChanges
            <<','<<stats.psoCreated<<','<<stats.uploadBytes<<','<<stats.descriptorUsed<<','<<stats.vramUsedMiB
            <<','<<(stats.longFrameReason?1:0);
        for(std::size_t index=0;index<3;++index)
            text<<','<<(stats.topPassMilliseconds[index]>0?RenderPassName(stats.topPasses[index]):"")<<','<<stats.topPassMilliseconds[index];
        text<<'\n';
    }
    const auto value=text.str();
    if(!asset_io::WriteAtomic(path,std::span<const std::uint8_t>(reinterpret_cast<const std::uint8_t*>(value.data()),value.size())))
    {
        error="Rendering.TelemetryUnavailable";
        return false;
    }
    return true;
}
}
