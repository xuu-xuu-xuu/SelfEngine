#include "PlatformLayer/Graphics/D3D12/D3D12Triangle.h"
#include "PlatformLayer/Graphics/D3D12/D3D12Context.h"
#include "PlatformLayer/Graphics/D3D12/D3D12Presentation.h"
#include "PlatformLayer/Graphics/D3D12/D3D12Result.h"
#include "D3D12PipelineCache.h"
#include "D3D12StateTracker.h"
#include "CoreLayer/Localization.h"
#include "CoreLayer/Log.h"
#include <d3dcompiler.h>
#include <cstddef>
#include <cstring>
#include <string>
#include <utility>

using Microsoft::WRL::ComPtr;

namespace
{
using Vertex=D3D12TriangleVertex;

}

bool InitializeD3D12Triangle(D3D12Triangle& triangle,const D3D12Context& context,
    const std::filesystem::path& shaderPath,const Localization& localization)
{
    se::rhi::PipelineDescription description;description.kind=se::rhi::PipelineKind::Triangle;description.shader=shaderPath;
    return InitializeD3D12Triangle(triangle,context,description,localization);
}
bool InitializeD3D12Triangle(D3D12Triangle& triangle,const D3D12Context& context,
    const se::rhi::PipelineDescription& description,const Localization& localization)
{
    if(triangle.rootSignature||triangle.pipeline||triangle.vertices||triangle.prepared)
    {Log(LogLevel::Error,localization.GetText("D3D12.TriangleNotEmpty"));return false;}
    if(!context.device||!context.states||!context.pipelines||description.shader.empty()||description.kind!=se::rhi::PipelineKind::Triangle)
    {Log(LogLevel::Error,localization.GetText("D3D12.TriangleInputsInvalid"));return false;}
    D3D12Triangle initialized;
    initialized.prepared=context.pipelines->Prepare(description,localization);if(!initialized.prepared)return false;
    initialized.rootSignature=initialized.prepared->rootSignature;initialized.pipeline=initialized.prepared->pipeline;

    // CPU 只在首次提交前写入；整个样例运行期间由 GPU 只读，关闭先等待再释放。
    constexpr Vertex vertices[] = {
        { { 0.0f, 0.65f, 0.0f }, { 1, 0, 0, 1 } },
        { { 0.65f, -0.65f, 0.0f }, { 0, 1, 0, 1 } },
        { { -0.65f, -0.65f, 0.0f }, { 0, 0, 1, 1 } }
    };
    D3D12_HEAP_PROPERTIES heap{};
    heap.Type = D3D12_HEAP_TYPE_UPLOAD;
    heap.CreationNodeMask = heap.VisibleNodeMask = 1;
    D3D12_RESOURCE_DESC buffer{};
    buffer.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
    buffer.Width = sizeof(vertices);
    buffer.Height = buffer.DepthOrArraySize = buffer.MipLevels = 1;
    buffer.SampleDesc = { 1, 0 };
    buffer.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
    if (!CheckGraphicsResult(context.device->CreateCommittedResource(&heap, D3D12_HEAP_FLAG_NONE,
        &buffer, D3D12_RESOURCE_STATE_GENERIC_READ, nullptr,
        IID_PPV_ARGS(initialized.vertices.GetAddressOf())), localization,
        "D3D12.CreateVertexBufferFailed")) return false;
    if(!CheckGraphicsResult(context.states->Register(*initialized.vertices.Get(),D3D12_RESOURCE_STATE_GENERIC_READ,D3D12TrackedHeap::Upload,"Triangle.vertices"),
        localization,"D3D12.StateTrackingFailed"))return false;
    void* mapped = nullptr;
    const D3D12_RANGE noReads{ 0, 0 };
    if (!CheckGraphicsResult(initialized.vertices->Map(0, &noReads, &mapped), localization,
        "D3D12.MapVertexBufferFailed")) return false;
    std::memcpy(mapped, vertices, sizeof(vertices));
    const D3D12_RANGE written{ 0, sizeof(vertices) };
    initialized.vertices->Unmap(0, &written);
    initialized.vertexView = { initialized.vertices->GetGPUVirtualAddress(), sizeof(vertices), sizeof(Vertex) };
    triangle = std::move(initialized);
    Log(LogLevel::Info, localization.GetText("D3D12.TriangleReady"));
    return true;
}

void RecordD3D12Triangle(const D3D12Triangle& triangle, ID3D12GraphicsCommandList& commands,
    UINT width, UINT height, const se::Mat4& clipFromWorld)
{
    const D3D12_VIEWPORT viewport{ 0, 0, static_cast<float>(width), static_cast<float>(height), 0, 1 };
    const D3D12_RECT scissor{ 0, 0, static_cast<LONG>(width), static_cast<LONG>(height) };
    commands.SetPipelineState(triangle.pipeline.Get());
    commands.SetGraphicsRootSignature(triangle.rootSignature.Get());
    // 此 API 在记录时复制原始值，GPU 不借用调用者矩阵；不做转置。
    commands.SetGraphicsRoot32BitConstants(0, 16, clipFromWorld.values.data(), 0);
    commands.RSSetViewports(1, &viewport);
    commands.RSSetScissorRects(1, &scissor);
    commands.IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
    commands.IASetVertexBuffers(0, 1, &triangle.vertexView);
    commands.DrawInstanced(3, 1, 0, 0);
}
