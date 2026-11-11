#pragma once
#include "PlatformLayer/Graphics/Rhi.h"
#include <span>

namespace se
{
// G02：PSO 预热的统计。cached 由 Device 的 pipelineCacheHits 差值得到，
// created 由 pipelineCreations 差值得到，失败数只统计本次请求。
struct PipelineWarmupStats
{
    std::uint32_t requested=0,created=0,cached=0,failed=0;
    double milliseconds=0;
};

// 在呈现线程、上传/绘制之前调用；成功创建的 Pipeline 写入 outputs。
PipelineWarmupStats WarmupPipelines(rhi::Device& device,
    std::span<const rhi::PipelineDescription> descriptions,
    std::span<rhi::Pipeline> outputs);
}
