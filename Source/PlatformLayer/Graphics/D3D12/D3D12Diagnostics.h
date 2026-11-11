#pragma once
#include "PlatformLayer/Graphics/Rhi.h"
#include "D3D12StateTracker.h"
struct D3D12Presentation;
class D3D12MemoryAllocator;
struct D3D12PreparedPipeline;

namespace se::rhi
{
// 平台专项诊断/验证的借用端口，不属于上层 RHI 契约；设备关闭后指针失效。
struct D3D12DiagnosticsAccess
{
    static ID3D12Device* NativeDevice(Device& device);
    static ID3D12CommandQueue* Queue(Device& device);
    static D3D12Presentation* Presentation(Device& device);
    static std::shared_ptr<D3D12MemoryAllocator> Memory(Device& device);
    static ID3D12PipelineState* PipelineState(Device& device,PipelineHandle handle);
    static std::weak_ptr<const D3D12PreparedPipeline> Prepared(Device& device,PipelineHandle handle);
    static D3D12_CPU_DESCRIPTOR_HANDLE SrvCpu(Device& device,TextureHandle handle,UINT view=0);
    static ID3D12Resource* Resource(Device& device,BufferHandle handle);
    static ID3D12Resource* Resource(Device& device,TextureHandle handle);
    static std::weak_ptr<void> Lifetime(Device& device,BufferHandle handle);
    static std::weak_ptr<void> Lifetime(Device& device,TextureHandle handle);
    static std::weak_ptr<void> Lifetime(Device& device,PipelineHandle handle);
    static std::shared_ptr<D3D12StateRegistry> States(Device& device);
};
}
