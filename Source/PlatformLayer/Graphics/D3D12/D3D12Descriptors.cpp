#include "D3D12Descriptors.h"
#include <algorithm>
#include <unordered_map>
#include <string>

D3D12DescriptorPool::D3D12DescriptorPool(ID3D12Device& device,D3D12_DESCRIPTOR_HEAP_TYPE type,UINT pageSize,UINT maximum)
    :device_(&device),type_(type),increment_(device.GetDescriptorHandleIncrementSize(type)),pageSize_(pageSize),maximum_(maximum){}
D3D12DescriptorRange::~D3D12DescriptorRange(){owner_->Free(page_,first_,count_);}
D3D12_CPU_DESCRIPTOR_HANDLE D3D12DescriptorRange::Cpu(UINT index) const
{
    if(index>=count_)return {};
    auto handle=owner_->pages_[page_].heap->GetCPUDescriptorHandleForHeapStart();handle.ptr+=SIZE_T(first_+index)*owner_->increment_;return handle;
}
D3D12_DESCRIPTOR_HEAP_TYPE D3D12DescriptorRange::Type() const{return owner_->type_;}
ID3D12Device* D3D12DescriptorRange::Device() const{return owner_->device_;}
bool D3D12DescriptorPool::Configure(UINT pageSize,UINT maximum)
{
    if(owner_!=std::this_thread::get_id()||stats_.used||!pageSize||pageSize>maximum||maximum>65536)return false;
    pages_.clear();stats_.capacity=0;pageSize_=pageSize;maximum_=maximum;return true;
}
HRESULT D3D12DescriptorPool::Allocate(UINT count,std::shared_ptr<D3D12DescriptorRange>& output)
{
    if(owner_!=std::this_thread::get_id()||weak_from_this().expired())return E_UNEXPECTED;
    if(output||!count||!pageSize_||pageSize_>maximum_||maximum_>65536||count>maximum_
        ||(type_!=D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV&&type_!=D3D12_DESCRIPTOR_HEAP_TYPE_RTV&&type_!=D3D12_DESCRIPTOR_HEAP_TYPE_DSV))
    {++stats_.failures;return E_INVALIDARG;}
    const auto take=[&](UINT pageIndex,std::map<UINT,UINT>::iterator free){
        auto& page=pages_[pageIndex];const auto first=free->first,remaining=free->second-count;page.free.erase(free);
        if(remaining)page.free.emplace(first+count,remaining);
        output=std::shared_ptr<D3D12DescriptorRange>(new D3D12DescriptorRange(shared_from_this(),pageIndex,first,count));
        stats_.used+=count;stats_.peak=std::max(stats_.peak,stats_.used);++stats_.allocations;
    };
    for(UINT index=0;index<pages_.size();++index)
        for(auto free=pages_[index].free.begin();free!=pages_[index].free.end();++free)if(free->second>=count){take(index,free);return S_OK;}
    const auto remaining=maximum_-stats_.capacity;
    if(remaining<count){++stats_.failures;return E_OUTOFMEMORY;}
    const auto capacity=std::min(remaining,std::max(count,pages_.empty()?pageSize_:pages_.back().capacity*2));
    D3D12_DESCRIPTOR_HEAP_DESC desc{};desc.Type=type_;desc.NumDescriptors=capacity;
    Page page;const auto result=device_->CreateDescriptorHeap(&desc,IID_PPV_ARGS(page.heap.GetAddressOf()));
    if(FAILED(result)){++stats_.failures;return result;}
    page.heap->SetName(L"SelfEngine.CpuDescriptorPage");page.capacity=capacity;page.free.emplace(0,capacity);
    pages_.push_back(std::move(page));stats_.capacity+=capacity;++stats_.heapsCreated;
    const auto index=static_cast<UINT>(pages_.size()-1);take(index,pages_[index].free.begin());return S_OK;
}
bool D3D12DescriptorPool::CanAllocate(UINT count) const
{
    if(!count||count>maximum_)return false;
    for(const auto& page:pages_)for(const auto& [_,length]:page.free)if(length>=count)return true;
    return count<=maximum_-stats_.capacity;
}
void D3D12DescriptorPool::Free(UINT pageIndex,UINT first,UINT count)
{
    const auto released=count;
    auto& page=pages_.at(pageIndex);auto next=page.free.lower_bound(first);
    if(next!=page.free.begin())
    {
        auto previous=std::prev(next);
        if(previous->first+previous->second==first){first=previous->first;count+=previous->second;page.free.erase(previous);}
    }
    if(next!=page.free.end()&&first+count==next->first){count+=next->second;page.free.erase(next);}
    page.free.emplace(first,count);
    stats_.used-=released;
}
bool D3D12FrameDescriptors::Configure(UINT initial,UINT maximum)
{if(!initial||initial>maximum||maximum>65536)return false;Release();initial_=initial;maximum_=maximum;return true;}
void D3D12FrameDescriptors::Release(){draws_.clear();heap_.Reset();stats_.used=stats_.capacity=0;}
HRESULT D3D12FrameDescriptors::Prepare(ID3D12Device& device,std::span<const D3D12_CPU_DESCRIPTOR_HANDLE> sources,UINT tableWidth)
{
    if(!tableWidth||tableWidth>7||sources.size()%tableWidth){++stats_.failures;return E_INVALIDARG;}
    std::vector<D3D12_CPU_DESCRIPTOR_HANDLE> unique;std::vector<UINT> offsets;offsets.reserve(sources.size()/tableWidth);
    std::unordered_map<std::string,UINT> lookup;
    for(size_t first=0;first<sources.size();first+=tableWidth)
    {
        std::string key;
        for(UINT index=0;index<tableWidth;++index)
        {if(!sources[first+index].ptr){++stats_.failures;return E_INVALIDARG;}key+=std::to_string(sources[first+index].ptr)+":";}
        auto found=lookup.find(key);
        if(found==lookup.end())
        {
            if(unique.size()+tableWidth>maximum_){++stats_.failures;return E_OUTOFMEMORY;}
            const auto index=static_cast<UINT>(unique.size());lookup.emplace(std::move(key),index);
            unique.insert(unique.end(),sources.begin()+first,sources.begin()+first+tableWidth);offsets.push_back(index);
        }
        else offsets.push_back(found->second);
    }
    const auto needed=static_cast<UINT>(unique.size());
    if(needed>stats_.capacity)
    {
        UINT capacity=std::max(initial_,stats_.capacity);while(capacity<needed)capacity=std::min(maximum_,capacity*2);
        D3D12_DESCRIPTOR_HEAP_DESC desc{};desc.Type=D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV;
        desc.NumDescriptors=capacity;desc.Flags=D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE;
        Microsoft::WRL::ComPtr<ID3D12DescriptorHeap> candidate;
        const auto result=device.CreateDescriptorHeap(&desc,IID_PPV_ARGS(candidate.GetAddressOf()));
        if(FAILED(result)){++stats_.failures;return result;}
        candidate->SetName(L"SelfEngine.FrameDescriptorTable");heap_=std::move(candidate);stats_.capacity=capacity;++stats_.heapsCreated;
    }
    stats_.used=needed;stats_.peak=std::max(stats_.peak,needed);stats_.allocations+=needed;draws_.clear();draws_.reserve(sources.size());
    if(!needed)return S_OK;
    const auto increment=device.GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);
    auto target=heap_->GetCPUDescriptorHandleForHeapStart();
    for(const auto source:unique){device.CopyDescriptorsSimple(1,target,source,D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);target.ptr+=increment;}
    const auto base=heap_->GetGPUDescriptorHandleForHeapStart();for(const auto offset:offsets)draws_.push_back({base.ptr+UINT64(offset)*increment});return S_OK;
}
