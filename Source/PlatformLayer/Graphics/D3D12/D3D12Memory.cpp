#include "D3D12Memory.h"
#include "D3D12StateTracker.h"
#include <algorithm>
#include <atomic>
#include <map>
#include <thread>
#include <utility>

namespace
{
constexpr GUID allocationId{0x40fa53c7,0xf887,0x4944,{0x9d,0x56,0x9f,0x9c,0x84,0x69,0xb5,0xa7}};
UINT64 Align(UINT64 value,UINT64 alignment){return (value+alignment-1)&~(alignment-1);}
D3D12MemoryCategory Category(const D3D12_RESOURCE_DESC& description)
{
    if(description.Dimension==D3D12_RESOURCE_DIMENSION_BUFFER)return D3D12MemoryCategory::Buffer;
    return description.Flags&(D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET|D3D12_RESOURCE_FLAG_ALLOW_DEPTH_STENCIL)?D3D12MemoryCategory::RenderTarget:D3D12MemoryCategory::Texture;
}
D3D12_HEAP_FLAGS Flags(D3D12MemoryCategory category)
{
    switch(category){case D3D12MemoryCategory::Buffer:return D3D12_HEAP_FLAG_ALLOW_ONLY_BUFFERS;
    case D3D12MemoryCategory::Texture:return D3D12_HEAP_FLAG_ALLOW_ONLY_NON_RT_DS_TEXTURES;default:return D3D12_HEAP_FLAG_ALLOW_ONLY_RT_DS_TEXTURES;}
}
template<class T> class Holder final : public IUnknown
{
public:
    explicit Holder(std::shared_ptr<T> value):value(std::move(value)){}
    HRESULT STDMETHODCALLTYPE QueryInterface(REFIID id,void** output) override
    {if(!output)return E_POINTER;*output=nullptr;if(id!=__uuidof(IUnknown))return E_NOINTERFACE;*output=static_cast<IUnknown*>(this);AddRef();return S_OK;}
    ULONG STDMETHODCALLTYPE AddRef() override{return ++references_;}
    ULONG STDMETHODCALLTYPE Release() override{const auto count=--references_;if(!count)delete this;return count;}
    std::shared_ptr<T> value;
private:
    std::atomic<ULONG> references_{1};
};
}
struct D3D12MemoryAllocator::Impl : public std::enable_shared_from_this<Impl>
{
    struct Block
    {
        Microsoft::WRL::ComPtr<ID3D12Heap> heap;
        UINT64 id=0,capacity=0,used=0,alignment=0;
        D3D12MemoryCategory category=D3D12MemoryCategory::Buffer;
        std::map<UINT64,UINT64> free;
    };
    struct Lease
    {
        std::shared_ptr<Impl> owner;
        std::shared_ptr<Block> block;
        D3D12AllocationInfo info;
        bool accounted=false;
        ~Lease(){if(accounted)owner->Free(*this);}
    };
    ID3D12Device* device;
    std::shared_ptr<D3D12StateRegistry> states;
    const std::thread::id owner=std::this_thread::get_id();
    se::rhi::MemorySettings settings;
    D3D12MemoryStatistics stats;
    std::vector<std::shared_ptr<Block>> blocks;
    std::map<UINT64,std::weak_ptr<Lease>> allocations;
    UINT64 nextAllocation=1,nextHeap=1;
    Impl(ID3D12Device& value,std::shared_ptr<D3D12StateRegistry> registry):device(&value),states(std::move(registry)){stats.budget=settings.budgetBytes;}
    void Free(const Lease& lease)
    {
        const auto& info=lease.info;
        if(lease.block)
        {
            auto& block=*lease.block;auto first=info.offset,count=info.bytes;auto next=block.free.lower_bound(first);
            if(next!=block.free.begin())
            {auto previous=std::prev(next);if(previous->first+previous->second==first){first=previous->first;count+=previous->second;block.free.erase(previous);}}
            if(next!=block.free.end()&&first+count==next->first){count+=next->second;block.free.erase(next);}
            block.free.emplace(first,count);block.used-=info.bytes;
        }
        else {stats.reserved-=info.bytes;--stats.committedResources;}
        stats.used-=info.bytes;--stats.resources;allocations.erase(info.serial);
    }
    void Trim()
    {
        std::erase_if(blocks,[&](const auto& block){if(block->used)return false;stats.reserved-=block->capacity;return true;});
    }
};
D3D12MemoryAllocator::D3D12MemoryAllocator(ID3D12Device& device,std::shared_ptr<D3D12StateRegistry> states)
    :impl_(std::make_shared<Impl>(device,std::move(states))){}
bool D3D12MemoryAllocator::Configure(se::rhi::MemorySettings settings)
{
    auto& p=*impl_;
    constexpr UINT64 alignment=D3D12_DEFAULT_RESOURCE_PLACEMENT_ALIGNMENT;
    if(p.owner!=std::this_thread::get_id()||p.stats.resources||settings.heapPageBytes<alignment||settings.heapPageBytes%alignment
        ||settings.heapPageBytes>settings.budgetBytes||settings.budgetBytes>4ull*1024*1024*1024||settings.budgetBytes%alignment
        ||(settings.mode!=se::rhi::MemoryMode::Placed&&settings.mode!=se::rhi::MemoryMode::CommittedBaseline))return false;
    p.Trim();p.settings=settings;p.stats.budget=settings.budgetBytes;return true;
}
HRESULT D3D12MemoryAllocator::Create(const D3D12_RESOURCE_DESC& description,D3D12_RESOURCE_STATES initial,
    const D3D12_CLEAR_VALUE* clear,Microsoft::WRL::ComPtr<ID3D12Resource>& output,const std::string& name)
{
    auto& p=*impl_;
    if(p.owner!=std::this_thread::get_id()||!p.states)return E_UNEXPECTED;
    if(output||description.Dimension==D3D12_RESOURCE_DIMENSION_UNKNOWN||(description.Flags&D3D12_RESOURCE_FLAG_ALLOW_SIMULTANEOUS_ACCESS)
        ||p.nextAllocation==UINT64_MAX)return E_INVALIDARG;
    const auto allocation=p.device->GetResourceAllocationInfo(0,1,&description);
    if(!allocation.SizeInBytes||allocation.SizeInBytes==UINT64_MAX||!allocation.Alignment||(allocation.Alignment&(allocation.Alignment-1)))return E_INVALIDARG;
    auto lease=std::make_shared<Impl::Lease>();lease->owner=impl_;
    lease->info={p.nextAllocation++,0,0,allocation.SizeInBytes,allocation.Alignment,Category(description),p.settings.mode==se::rhi::MemoryMode::Placed,name};
    const auto budgetFailure=[&]{++p.stats.budgetFailures;return D3D12MemoryBudgetExceeded;};
    if(allocation.SizeInBytes>p.settings.budgetBytes)return budgetFailure();
    Microsoft::WRL::ComPtr<ID3D12Resource> candidate;
    if(!lease->info.placed)
    {
        if(allocation.SizeInBytes>p.settings.budgetBytes-p.stats.reserved)p.Trim();
        if(allocation.SizeInBytes>p.settings.budgetBytes-p.stats.reserved)return budgetFailure();
        D3D12_HEAP_PROPERTIES heap{};heap.Type=D3D12_HEAP_TYPE_DEFAULT;heap.CreationNodeMask=heap.VisibleNodeMask=1;
        const auto result=p.device->CreateCommittedResource(&heap,D3D12_HEAP_FLAG_NONE,&description,initial,clear,IID_PPV_ARGS(candidate.GetAddressOf()));
        if(FAILED(result))return result;
    }
    else
    {
        UINT64 chosenOffset=0;
        for(const auto& block:p.blocks)
        {
            if(block->category!=lease->info.category||block->alignment<allocation.Alignment)continue;
            for(const auto& [offset,size]:block->free)
            {
                const auto aligned=Align(offset,allocation.Alignment);
                if(aligned-offset<=size&&allocation.SizeInBytes<=size-(aligned-offset)){lease->block=block;chosenOffset=aligned;break;}
            }
            if(lease->block)break;
        }
        if(!lease->block)
        {
            const auto alignment=std::max(UINT64{D3D12_DEFAULT_RESOURCE_PLACEMENT_ALIGNMENT},allocation.Alignment);
            auto capacity=Align(std::max(p.settings.heapPageBytes,allocation.SizeInBytes),alignment);
            if(capacity>p.settings.budgetBytes-p.stats.reserved)p.Trim();
            const auto remaining=(p.settings.budgetBytes-p.stats.reserved)/alignment*alignment;
            capacity=std::min(capacity,remaining);
            if(capacity<allocation.SizeInBytes)return budgetFailure();
            auto block=std::make_shared<Impl::Block>();block->id=p.nextHeap++;block->capacity=capacity;block->alignment=alignment;block->category=lease->info.category;
            D3D12_HEAP_DESC heap{};heap.SizeInBytes=capacity;heap.Alignment=alignment;heap.Properties.Type=D3D12_HEAP_TYPE_DEFAULT;
            heap.Properties.CreationNodeMask=heap.Properties.VisibleNodeMask=1;heap.Flags=Flags(block->category);
            const auto result=p.device->CreateHeap(&heap,IID_PPV_ARGS(block->heap.GetAddressOf()));if(FAILED(result))return result;
            block->heap->SetName(L"SelfEngine.DefaultMemoryHeap");block->free.emplace(0,capacity);p.blocks.push_back(block);lease->block=std::move(block);
            p.stats.reserved+=capacity;++p.stats.heapCreations;p.stats.peakReserved=std::max(p.stats.peakReserved,p.stats.reserved);
        }
        lease->info.heap=lease->block->id;lease->info.offset=chosenOffset;
        const auto result=p.device->CreatePlacedResource(lease->block->heap.Get(),chosenOffset,&description,initial,clear,IID_PPV_ARGS(candidate.GetAddressOf()));
        if(FAILED(result))return result;
    }
    auto result=p.states->Register(*candidate.Get(),initial,D3D12TrackedHeap::Default,name);
    if(SUCCEEDED(result)&&lease->info.placed)result=p.states->RequireActivation(*candidate.Get());
    if(FAILED(result))return result;
    if(lease->block)
    {
        auto& block=*lease->block;const auto first=lease->info.offset,bytes=lease->info.bytes;
        auto range=std::prev(block.free.upper_bound(first));const auto begin=range->first,end=range->first+range->second;block.free.erase(range);
        if(first>begin)block.free.emplace(begin,first-begin);if(first+bytes<end)block.free.emplace(first+bytes,end-first-bytes);
        block.used+=bytes;
    }
    else {p.stats.reserved+=allocation.SizeInBytes;++p.stats.committedResources;++p.stats.committedCreations;}
    p.stats.used+=allocation.SizeInBytes;++p.stats.resources;++p.stats.resourceCreations;
    p.stats.peakUsed=std::max(p.stats.peakUsed,p.stats.used);p.stats.peakReserved=std::max(p.stats.peakReserved,p.stats.reserved);
    lease->accounted=true;p.allocations.emplace(lease->info.serial,lease);
    auto* holder=new Holder<Impl::Lease>(lease);result=candidate->SetPrivateDataInterface(allocationId,holder);holder->Release();
    if(FAILED(result))return result;
    output=std::move(candidate);return S_OK;
}
void D3D12MemoryAllocator::Trim(){if(impl_->owner==std::this_thread::get_id())impl_->Trim();}
void D3D12MemoryAllocator::SetName(ID3D12Resource& resource,const std::string& name)
{
    Microsoft::WRL::ComPtr<IUnknown> holder;UINT bytes=sizeof(IUnknown*);
    if(FAILED(resource.GetPrivateData(allocationId,&bytes,holder.GetAddressOf()))||!holder)return;
    const auto& lease=static_cast<Holder<Impl::Lease>*>(holder.Get())->value;if(lease->owner==impl_)lease->info.name=name;
}
D3D12MemoryStatistics D3D12MemoryAllocator::Stats() const
{
    auto stats=impl_->stats;stats.free=stats.reserved-stats.used;stats.placedHeaps=impl_->blocks.size();
    for(const auto& block:impl_->blocks)for(const auto& [_,size]:block->free)stats.largestFreeRange=std::max(stats.largestFreeRange,size);
    return stats;
}
std::optional<D3D12AllocationInfo> D3D12MemoryAllocator::Allocation(ID3D12Resource& resource) const
{
    Microsoft::WRL::ComPtr<IUnknown> holder;UINT bytes=sizeof(IUnknown*);
    if(FAILED(resource.GetPrivateData(allocationId,&bytes,holder.GetAddressOf()))||!holder)return {};
    const auto& lease=static_cast<Holder<Impl::Lease>*>(holder.Get())->value;
    return lease->owner==impl_?std::optional(lease->info):std::nullopt;
}
std::vector<D3D12AllocationInfo> D3D12MemoryAllocator::Allocations() const
{
    std::vector<D3D12AllocationInfo> result;result.reserve(impl_->allocations.size());
    for(const auto& [_,weak]:impl_->allocations)if(const auto lease=weak.lock())result.push_back(lease->info);return result;
}
