#pragma once
#include <d3d12.h>
#include <wrl/client.h>
#include <map>
#include <memory>
#include <span>
#include <thread>
#include <vector>

struct D3D12DescriptorStatistics
{UINT used=0,capacity=0,peak=0;UINT64 heapsCreated=0,allocations=0,failures=0;};
class D3D12DescriptorPool;
// 仅呈现线程；Range 最后一个共享所有者退出时才归还持久 CPU 槽。
class D3D12DescriptorRange
{
public:
    ~D3D12DescriptorRange();
    D3D12DescriptorRange(const D3D12DescriptorRange&)=delete;
    D3D12DescriptorRange& operator=(const D3D12DescriptorRange&)=delete;
    D3D12_CPU_DESCRIPTOR_HANDLE Cpu(UINT index=0) const;
    UINT Count() const{return count_;}
    D3D12_DESCRIPTOR_HEAP_TYPE Type() const;
    ID3D12Device* Device() const;
private:
    friend class D3D12DescriptorPool;
    D3D12DescriptorRange(std::shared_ptr<D3D12DescriptorPool> owner,UINT page,UINT first,UINT count)
        :owner_(std::move(owner)),page_(page),first_(first),count_(count){}
    std::shared_ptr<D3D12DescriptorPool> owner_;
    UINT page_,first_,count_;
};
class D3D12DescriptorPool : public std::enable_shared_from_this<D3D12DescriptorPool>
{
public:
    D3D12DescriptorPool(ID3D12Device& device,D3D12_DESCRIPTOR_HEAP_TYPE type,UINT pageSize=64,UINT maximum=65536);
    D3D12DescriptorPool(const D3D12DescriptorPool&)=delete;
    D3D12DescriptorPool& operator=(const D3D12DescriptorPool&)=delete;
    HRESULT Allocate(UINT count,std::shared_ptr<D3D12DescriptorRange>& output);
    bool CanAllocate(UINT count) const;
    bool Configure(UINT pageSize,UINT maximum); // 仅无活跃 Range 时改变容量，不使旧地址失效。
    D3D12DescriptorStatistics Stats() const{return stats_;}
private:
    friend class D3D12DescriptorRange;
    struct Page{Microsoft::WRL::ComPtr<ID3D12DescriptorHeap> heap;UINT capacity=0;std::map<UINT,UINT> free;};
    void Free(UINT page,UINT first,UINT count);
    ID3D12Device* device_;
    D3D12_DESCRIPTOR_HEAP_TYPE type_;
    UINT increment_,pageSize_,maximum_;
    const std::thread::id owner_=std::this_thread::get_id();
    std::vector<Page> pages_;
    D3D12DescriptorStatistics stats_;
};
// FrameContext 独占，Prepare/Configure/Release 之前必须完成该帧的 Fence。
class D3D12FrameDescriptors
{
public:
    D3D12FrameDescriptors()=default;
    D3D12FrameDescriptors(D3D12FrameDescriptors&&) noexcept=default;
    D3D12FrameDescriptors& operator=(D3D12FrameDescriptors&&) noexcept=default;
    D3D12FrameDescriptors(const D3D12FrameDescriptors&)=delete;
    D3D12FrameDescriptors& operator=(const D3D12FrameDescriptors&)=delete;
    HRESULT Prepare(ID3D12Device& device,std::span<const D3D12_CPU_DESCRIPTOR_HANDLE> sources,UINT tableWidth=1);
    bool Configure(UINT initial,UINT maximum);
    void Reset(){draws_.clear();stats_.used=0;}
    void Release();
    ID3D12DescriptorHeap* Heap() const{return heap_.Get();}
    D3D12_GPU_DESCRIPTOR_HANDLE Draw(UINT index) const{return draws_.at(index);}
    UINT Maximum() const{return maximum_;}
    D3D12DescriptorStatistics Stats() const{return stats_;}
private:
    Microsoft::WRL::ComPtr<ID3D12DescriptorHeap> heap_;
    std::vector<D3D12_GPU_DESCRIPTOR_HANDLE> draws_;
    UINT initial_=64,maximum_=65536;
    D3D12DescriptorStatistics stats_;
};
