#pragma once

#include "CoreLayer/Math/Projection.h"
#include "D3D12Texture.h"
#include "PlatformLayer/Graphics/Rhi.h"
#include "PlatformLayer/Graphics/MaterialLayout.h"
#include <d3d12.h>
#include <wrl/client.h>
#include <array>
#include <cstdint>
#include <filesystem>
#include <span>

class Localization;
struct D3D12Context;
struct D3D12Presentation;
struct D3D12FrameContext;
struct D3D12PreparedPipeline;
struct D3D12IndirectCommand;
inline constexpr UINT D3D12MeshConstantStride=se::rhi::MaterialConstantAlignment;
inline constexpr UINT D3D12MeshMaxDraws=65536;

using D3D12MeshVertex=se::rhi::Vertex;
struct D3D12MeshPipeline
{
    Microsoft::WRL::ComPtr<ID3D12RootSignature> rootSignature;
    Microsoft::WRL::ComPtr<ID3D12PipelineState> pipeline;
    std::shared_ptr<const D3D12PreparedPipeline> prepared;
};
struct D3D12Mesh
{
    Microsoft::WRL::ComPtr<ID3D12Resource> vertices,indices,texture;
    std::shared_ptr<D3D12DescriptorRange> textureViews;
    D3D12_VERTEX_BUFFER_VIEW vertexView{};
    D3D12_INDEX_BUFFER_VIEW indexView{};
    UINT indexCount=0,textureWidth=0,textureHeight=0;
    std::array<D3D12Texture,2> pbrTextures; // 法线、金属粗糙度；布局 1 不使用。
};
struct D3D12MeshDraw
{
    const D3D12Mesh* mesh=nullptr; // 调用者保持至 GPU 最后使用完成。
    se::Mat4 world;
    se::Vec4 tint{1,1,1,1};
    bool textureIsData=false,unlit=false;
    se::rhi::DrawIndexed::Pbr pbr;
    std::span<const se::rhi::InstanceParameters> instances;
    const D3D12MeshPipeline* pipeline=nullptr; // 单绘制 Pipeline；空表示使用该批次默认 Pipeline。
    float impostorView=0,impostorNext=0,impostorBlend=0; // 仅 MeshDrawConstants 的 Impostor 编码。
    // G14：速度 Pass 的上一帧世界矩阵与顶点布局；由 RHI 绘制路径填充。
    bool hasPreviousWorld=false;
    se::Mat4 previousWorld;
    bool compressedVertices=false;
};

bool InitializeD3D12MeshPipeline(D3D12MeshPipeline& pipeline,const D3D12Context& context,
    const std::filesystem::path& shader,const Localization& localization);
bool InitializeD3D12MeshPipeline(D3D12MeshPipeline& pipeline,const D3D12Context& context,
    const se::rhi::PipelineDescription& description,const Localization& localization);
bool InitializeD3D12Mesh(D3D12Mesh& mesh,const D3D12Context& context,D3D12Presentation& presentation,
    std::span<const D3D12MeshVertex> vertices,std::span<const std::uint32_t> indices,
    D3D12TexturePixels texture,const Localization& localization);
bool InitializeD3D12Mesh(D3D12Mesh& mesh,const D3D12Context& context,D3D12Presentation& presentation,
    std::span<const D3D12MeshVertex> vertices,std::span<const std::uint32_t> indices,
    const D3D12Texture& sharedTexture,const Localization& localization);
bool ValidateD3D12MeshDraws(const D3D12MeshPipeline& pipeline,std::span<const D3D12MeshDraw> draws,
    const se::Mat4& viewProjection,const Localization& localization,bool allowEmptyInstanced=false);
// 仅在关联 FrameContext 已完成复用等待且分配 Draw 常量后记录。
// indirect 为空表示普通绘制；非空时与 draws 一一对应（G06.10 多批次）。
void RecordD3D12Meshes(const D3D12MeshPipeline& pipeline,std::span<const D3D12MeshDraw> draws,
    const se::Mat4& viewProjection,D3D12FrameContext& frame,ID3D12GraphicsCommandList& commands,UINT width,UINT height,
    bool cacheBindings=false,se::rhi::DrawBindingStatistics* statistics=nullptr,
    std::span<const D3D12IndirectCommand> indirect={});
