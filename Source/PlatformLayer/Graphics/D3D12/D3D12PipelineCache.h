#pragma once
#include "D3D12ShaderProgram.h"
#include <d3d12.h>

struct D3D12PreparedPipeline
{
    Microsoft::WRL::ComPtr<ID3D12RootSignature> rootSignature;
    Microsoft::WRL::ComPtr<ID3D12PipelineState> pipeline;
    std::shared_ptr<const D3D12ShaderProgram> program;
    se::rhi::DepthMode depth=se::rhi::DepthMode::Disabled;
    se::rhi::BlendMode blend=se::rhi::BlendMode::Opaque;
    se::rhi::ColorFormat colorFormat=se::rhi::ColorFormat::Rgba8Unorm;
    std::string key;
};
struct D3D12PipelineCacheStatistics
{
    D3D12ShaderLoadStatistics shaders;
    UINT64 programHits=0,pipelineHits=0,pipelineMisses=0,pipelineCreations=0,rootCreations=0;
    UINT64 libraryHits=0,libraryStores=0;
    bool libraryLoaded=false;
    size_t livePrograms=0,livePipelines=0;
};
class D3D12PipelineCache
{
public:
    explicit D3D12PipelineCache(ID3D12Device& device);
    ~D3D12PipelineCache();
    D3D12PipelineCache(const D3D12PipelineCache&)=delete;
    D3D12PipelineCache& operator=(const D3D12PipelineCache&)=delete;
    std::shared_ptr<const D3D12PreparedPipeline> Prepare(const se::rhi::PipelineDescription& description,const Localization& text);
    D3D12PipelineCacheStatistics Stats() const;
private:
    struct Impl;std::unique_ptr<Impl> impl_;
};
