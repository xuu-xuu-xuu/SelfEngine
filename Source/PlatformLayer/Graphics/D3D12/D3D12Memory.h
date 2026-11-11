#pragma once
#include "PlatformLayer/Graphics/Rhi.h"
#include <d3d12.h>
#include <wrl/client.h>
#include <optional>
class D3D12StateRegistry;
inline constexpr HRESULT D3D12MemoryBudgetExceeded=HRESULT_FROM_WIN32(ERROR_NOT_ENOUGH_QUOTA);
enum class D3D12MemoryCategory {Buffer,Texture,RenderTarget};
struct D3D12AllocationInfo
{
    UINT64 serial=0,heap=0,offset=0,bytes=0,alignment=0;
    D3D12MemoryCategory category=D3D12MemoryCategory::Buffer;
    bool placed=false;
    std::string name;
};
struct D3D12MemoryStatistics
{
    UINT64 budget=0,reserved=0,used=0,free=0,peakReserved=0,peakUsed=0,largestFreeRange=0;
    UINT64 resources=0,placedHeaps=0,committedResources=0,heapCreations=0,committedCreations=0,resourceCreations=0,budgetFailures=0;
};
// DEFAULT 存储；实际资源私有元数据拥有 allocation lease，表中不持资源 COM 引用。
class D3D12MemoryAllocator
{
public:
    D3D12MemoryAllocator(ID3D12Device& device,std::shared_ptr<D3D12StateRegistry> states);
    bool Configure(se::rhi::MemorySettings settings); // 无活跃资源时，创建 Presentation 前配置。
    HRESULT Create(const D3D12_RESOURCE_DESC& description,D3D12_RESOURCE_STATES initial,
        const D3D12_CLEAR_VALUE* clear,Microsoft::WRL::ComPtr<ID3D12Resource>& output,const std::string& name);
    void Trim(); // 仅释放已无活跃 lease 的 heap，不搬迁资源。
    void SetName(ID3D12Resource& resource,const std::string& name);
    D3D12MemoryStatistics Stats() const;
    std::optional<D3D12AllocationInfo> Allocation(ID3D12Resource& resource) const;
    std::vector<D3D12AllocationInfo> Allocations() const;
private:
    struct Impl;std::shared_ptr<Impl> impl_;
};
