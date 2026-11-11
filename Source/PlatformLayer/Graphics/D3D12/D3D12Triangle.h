#pragma once

#include <d3d12.h>
#include <wrl/client.h>
#include <filesystem>
#include "CoreLayer/Math/Math.h"
#include <memory>
#include <array>

class Localization;
struct D3D12Context;
struct D3D12PreparedPipeline;
namespace se::rhi {struct PipelineDescription;}
struct D3D12TriangleVertex {std::array<float,3> position;std::array<float,4> color;};
static_assert(sizeof(D3D12TriangleVertex)==28);

// 样例绘制资源；顶点初始化后只读，最后一次 GPU 使用完成后才能释放。
struct D3D12Triangle
{
    Microsoft::WRL::ComPtr<ID3D12RootSignature> rootSignature;
    Microsoft::WRL::ComPtr<ID3D12PipelineState> pipeline;
    Microsoft::WRL::ComPtr<ID3D12Resource> vertices;
    D3D12_VERTEX_BUFFER_VIEW vertexView{};
    std::shared_ptr<const D3D12PreparedPipeline> prepared;
};

bool InitializeD3D12Triangle(D3D12Triangle& triangle, const D3D12Context& context,
    const std::filesystem::path& shaderPath, const Localization& localization);
bool InitializeD3D12Triangle(D3D12Triangle& triangle, const D3D12Context& context,
    const se::rhi::PipelineDescription& description, const Localization& localization);

// 目标已处于 RENDER_TARGET；仅记录，不提交或等待。
void RecordD3D12Triangle(const D3D12Triangle& triangle, ID3D12GraphicsCommandList& commands,
    UINT width, UINT height, const se::Mat4& clipFromWorld = {});
