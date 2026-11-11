#pragma once
#include <d3d12.h>
#include <wrl/client.h>
#include <cstddef>
#include <memory>
#include <vector>
class D3D12StateRegistry;
struct D3D12UploadSlice
{
    ID3D12Resource* resource=nullptr;
    std::byte* cpu=nullptr;
    UINT64 offset=0,size=0;
    D3D12_GPU_VIRTUAL_ADDRESS Gpu() const {return resource->GetGPUVirtualAddress()+offset;}
};
struct D3D12UploadStatistics
{
    UINT64 allocations=0,pagesCreated=0,reservedBytes=0,usedBytes=0,peakUsedBytes=0,failures=0;
};
// 仅后端 FrameContext 拥有。Reset/Release 的调用者必须先证明该帧 Fence 已完成。
class D3D12UploadArena
{
public:
    static constexpr UINT64 pageBytes=64*1024,defaultBudget=256ull*1024*1024;
    D3D12UploadArena()=default;
    ~D3D12UploadArena();
    D3D12UploadArena(D3D12UploadArena&&) noexcept;
    D3D12UploadArena& operator=(D3D12UploadArena&&) noexcept;
    D3D12UploadArena(const D3D12UploadArena&)=delete;
    D3D12UploadArena& operator=(const D3D12UploadArena&)=delete;
    void Initialize(ID3D12Device& device,std::shared_ptr<D3D12StateRegistry> states,UINT64 pageCapacity=pageBytes);
    HRESULT Allocate(UINT64 bytes,UINT64 alignment,D3D12UploadSlice& output);
    void Reset();
    void Release();
    void SetDedicated(bool dedicated); // 与 Reset 相同的完成前置。
    bool SetBudget(UINT64 bytes); // 只允许不低于当前保留量的预算。
    D3D12UploadStatistics Stats() const {return stats_;}
private:
    struct Page
    {
        Microsoft::WRL::ComPtr<ID3D12Resource> resource;
        std::byte* mapped=nullptr;UINT64 capacity=0,cursor=0;
    };
    void ClearPages();
    ID3D12Device* device_=nullptr;
    std::shared_ptr<D3D12StateRegistry> states_;
    std::vector<Page> pages_;
    UINT64 budget_=defaultBudget;
    UINT64 pageCapacity_=pageBytes;
    bool dedicated_=false;
    D3D12UploadStatistics stats_;
};
