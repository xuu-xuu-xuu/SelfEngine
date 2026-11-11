#include "FunctionLayer/Rendering/PipelineWarmup.h"
#include <chrono>

namespace se
{
PipelineWarmupStats WarmupPipelines(rhi::Device& device,
    std::span<const rhi::PipelineDescription> descriptions,
    std::span<rhi::Pipeline> outputs)
{
    PipelineWarmupStats stats;
    stats.requested=static_cast<std::uint32_t>(descriptions.size());
    if(descriptions.size()!=outputs.size()){stats.failed=stats.requested;return stats;}
    const auto before=device.Stats();
    const auto start=std::chrono::steady_clock::now();
    for(std::size_t index=0;index<descriptions.size();++index)
    {
        rhi::Pipeline pipeline;
        if(!device.CreatePipeline(descriptions[index],pipeline)){++stats.failed;continue;}
        outputs[index]=std::move(pipeline);
        ++stats.created;
    }
    stats.milliseconds=std::chrono::duration<double,std::milli>(std::chrono::steady_clock::now()-start).count();
    const auto after=device.Stats();
    stats.created=static_cast<std::uint32_t>(after.pipelineCreations-before.pipelineCreations);
    stats.cached=static_cast<std::uint32_t>(after.pipelineCacheHits-before.pipelineCacheHits);
    return stats;
}
}
