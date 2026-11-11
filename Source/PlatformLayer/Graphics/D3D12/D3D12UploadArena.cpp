#include "D3D12UploadArena.h"
#include "D3D12StateTracker.h"
#include <algorithm>
#include <utility>

namespace
{
UINT64 Align(UINT64 value,UINT64 alignment){return (value+alignment-1)&~(alignment-1);}
}
D3D12UploadArena::~D3D12UploadArena(){ClearPages();}
D3D12UploadArena::D3D12UploadArena(D3D12UploadArena&& other) noexcept {*this=std::move(other);}
D3D12UploadArena& D3D12UploadArena::operator=(D3D12UploadArena&& other) noexcept
{
    if(this==&other)return *this;
    ClearPages();device_=std::exchange(other.device_,nullptr);states_=std::move(other.states_);
    pages_=std::move(other.pages_);other.pages_.clear();budget_=other.budget_;pageCapacity_=other.pageCapacity_;dedicated_=other.dedicated_;
    stats_=std::exchange(other.stats_,{});return *this;
}
void D3D12UploadArena::Initialize(ID3D12Device& device,std::shared_ptr<D3D12StateRegistry> states,UINT64 pageCapacity)
{Release();device_=&device;states_=std::move(states);pageCapacity_=pageCapacity;}
HRESULT D3D12UploadArena::Allocate(UINT64 bytes,UINT64 alignment,D3D12UploadSlice& output)
{
    output={};
    if(!device_||!states_||!bytes||!alignment||(alignment&(alignment-1))||alignment>pageBytes||bytes>budget_
        ||pageCapacity_<256||pageCapacity_>pageBytes||(pageCapacity_&(pageCapacity_-1)))
    {++stats_.failures;return E_INVALIDARG;}
    auto allocate=[&](Page& page,UINT64 offset){
        const auto previous=page.cursor;page.cursor=offset+bytes;
        stats_.usedBytes+=page.cursor-previous;stats_.peakUsedBytes=std::max(stats_.peakUsedBytes,stats_.usedBytes);
        ++stats_.allocations;output={page.resource.Get(),page.mapped+offset,offset,bytes};
    };
    if(!dedicated_)for(auto& page:pages_)
    {
        const auto offset=Align(page.cursor,alignment);
        if(offset<=page.capacity&&bytes<=page.capacity-offset){allocate(page,offset);return S_OK;}
    }
    const auto capacity=Align(bytes,dedicated_?alignment:pageCapacity_);
    if(capacity>budget_-stats_.reservedBytes){++stats_.failures;return E_OUTOFMEMORY;}
    Page page;page.capacity=capacity;
    D3D12_HEAP_PROPERTIES heap{};heap.Type=D3D12_HEAP_TYPE_UPLOAD;heap.CreationNodeMask=heap.VisibleNodeMask=1;
    D3D12_RESOURCE_DESC description{};description.Dimension=D3D12_RESOURCE_DIMENSION_BUFFER;description.Width=capacity;
    description.Height=description.DepthOrArraySize=description.MipLevels=1;description.SampleDesc={1,0};description.Layout=D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
    auto result=device_->CreateCommittedResource(&heap,D3D12_HEAP_FLAG_NONE,&description,D3D12_RESOURCE_STATE_GENERIC_READ,nullptr,IID_PPV_ARGS(page.resource.GetAddressOf()));
    if(SUCCEEDED(result))result=states_->Register(*page.resource.Get(),D3D12_RESOURCE_STATE_GENERIC_READ,D3D12TrackedHeap::Upload,"FrameUploadPage");
    void* mapped=nullptr;const D3D12_RANGE noRead{0,0};
    if(SUCCEEDED(result))result=page.resource->Map(0,&noRead,&mapped);
    if(FAILED(result)){++stats_.failures;return result;}
    page.mapped=static_cast<std::byte*>(mapped);page.resource->SetName(L"SelfEngine.FrameUploadPage");
    pages_.push_back(std::move(page));stats_.reservedBytes+=capacity;++stats_.pagesCreated;
    allocate(pages_.back(),0);return S_OK;
}
void D3D12UploadArena::ClearPages()
{
    for(auto& page:pages_)if(page.resource&&page.mapped)page.resource->Unmap(0,nullptr);
    pages_.clear();stats_.reservedBytes=stats_.usedBytes=0;
}
void D3D12UploadArena::Reset()
{if(dedicated_)ClearPages();else{for(auto& page:pages_)page.cursor=0;stats_.usedBytes=0;}}
void D3D12UploadArena::Release(){ClearPages();states_.reset();device_=nullptr;}
void D3D12UploadArena::SetDedicated(bool dedicated){ClearPages();dedicated_=dedicated;}
bool D3D12UploadArena::SetBudget(UINT64 bytes)
{if(bytes<pageBytes||bytes>defaultBudget||bytes<stats_.reservedBytes)return false;budget_=bytes;return true;}
