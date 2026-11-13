#include "D3D12StateTracker.h"
#include <algorithm>
#include <atomic>
#include <limits>
#include <utility>
#include <type_traits>

namespace
{
constexpr GUID registryId{0x59cc2253,0x8dfa,0x4b72,{0xa5,0x61,0x2c,0xbe,0xad,0x72,0x18,0x05}};
constexpr GUID resourceId{0x29b7cd40,0xa064,0x45ab,{0x8e,0xcc,0xbc,0x37,0xa4,0x20,0x76,0xc8}};
template<class T> class PrivateOwner final : public IUnknown
{
public:
    explicit PrivateOwner(std::shared_ptr<T> value):value(std::move(value)){}
    HRESULT STDMETHODCALLTYPE QueryInterface(REFIID id,void** output) override
    {
        if(!output)return E_POINTER;*output=nullptr;
        if(id!=__uuidof(IUnknown))return E_NOINTERFACE;
        *output=static_cast<IUnknown*>(this);AddRef();return S_OK;
    }
    ULONG STDMETHODCALLTYPE AddRef() override {return ++references_;}
    ULONG STDMETHODCALLTYPE Release() override {const auto count=--references_;if(!count)delete this;return count;}
    std::shared_ptr<T> value;
private:
    ~PrivateOwner()
    {
        if constexpr(std::is_same_v<T,D3D12StateRecord>)value->attached.store(false,std::memory_order_release);
    }
    std::atomic<ULONG> references_{1};
};
constexpr UINT readStates=D3D12_RESOURCE_STATE_VERTEX_AND_CONSTANT_BUFFER|D3D12_RESOURCE_STATE_INDEX_BUFFER
    |D3D12_RESOURCE_STATE_DEPTH_READ|D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE|D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE
    |D3D12_RESOURCE_STATE_INDIRECT_ARGUMENT|D3D12_RESOURCE_STATE_COPY_SOURCE|D3D12_RESOURCE_STATE_RESOLVE_SOURCE;
bool Compatible(D3D12_RESOURCE_STATES before,D3D12_RESOURCE_STATES after)
{
    const auto desired=static_cast<UINT>(after),current=static_cast<UINT>(before);
    return before==after||(desired&&(desired&~readStates)==0&&(current&desired)==desired);
}
bool ValidUsage(const D3D12StateRecord& record,D3D12_RESOURCE_STATES state)
{
    const UINT bits=static_cast<UINT>(state);
    constexpr UINT writes=D3D12_RESOURCE_STATE_RENDER_TARGET|D3D12_RESOURCE_STATE_DEPTH_WRITE|D3D12_RESOURCE_STATE_COPY_DEST|D3D12_RESOURCE_STATE_RESOLVE_DEST|D3D12_RESOURCE_STATE_UNORDERED_ACCESS;
    if(bits&~(readStates|writes))return false;
    const auto write=bits&writes;if(write&&((write&(write-1))||bits!=write))return false;
    if(record.heap==D3D12TrackedHeap::Upload)return bits&&(bits&~static_cast<UINT>(D3D12_RESOURCE_STATE_GENERIC_READ))==0;
    if(record.heap==D3D12TrackedHeap::Readback)return state==D3D12_RESOURCE_STATE_COPY_DEST;
    const auto& description=record.description;
    if((bits&D3D12_RESOURCE_STATE_UNORDERED_ACCESS)&&!(description.Flags&D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS))return false;
    if((bits&D3D12_RESOURCE_STATE_RENDER_TARGET)&&!(description.Flags&D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET))return false;
    if((bits&(D3D12_RESOURCE_STATE_DEPTH_WRITE|D3D12_RESOURCE_STATE_DEPTH_READ))&&!(description.Flags&D3D12_RESOURCE_FLAG_ALLOW_DEPTH_STENCIL))return false;
    if(description.Dimension!=D3D12_RESOURCE_DIMENSION_BUFFER&&(bits&(D3D12_RESOURCE_STATE_VERTEX_AND_CONSTANT_BUFFER|D3D12_RESOURCE_STATE_INDEX_BUFFER)))return false;
    return true;
}
}
std::shared_ptr<D3D12StateRegistry> D3D12StateRegistry::ForDevice(ID3D12Device& device)
{
    Microsoft::WRL::ComPtr<IUnknown> existing;UINT bytes=sizeof(IUnknown*);
    if(SUCCEEDED(device.GetPrivateData(registryId,&bytes,existing.GetAddressOf()))&&existing)
        return static_cast<PrivateOwner<D3D12StateRegistry>*>(existing.Get())->value;
    auto registry=std::shared_ptr<D3D12StateRegistry>(new D3D12StateRegistry(device));
    auto* holder=new PrivateOwner<D3D12StateRegistry>(registry);
    const auto result=device.SetPrivateDataInterface(registryId,holder);holder->Release();
    return SUCCEEDED(result)?registry:nullptr;
}
HRESULT D3D12StateRegistry::BindQueue(ID3D12CommandQueue& queue)
{
    if(owner_!=std::this_thread::get_id()||(queue_&&queue_!=&queue))return E_UNEXPECTED;
    if(queue.GetDesc().Type!=D3D12_COMMAND_LIST_TYPE_DIRECT)return E_INVALIDARG;
    Microsoft::WRL::ComPtr<ID3D12Device> device;
    if(FAILED(queue.GetDevice(IID_PPV_ARGS(device.GetAddressOf())))||device.Get()!=device_)return E_INVALIDARG;
    queue_=&queue;++queueUsers_;return S_OK;
}
void D3D12StateRegistry::UnbindQueue(ID3D12CommandQueue* queue) noexcept
{if(queue_==queue&&queueUsers_){--queueUsers_;if(!queueUsers_)queue_=nullptr;}}
HRESULT D3D12StateRegistry::Register(ID3D12Resource& resource,D3D12_RESOURCE_STATES initial,D3D12TrackedHeap heap,std::string name)
{
    if(owner_!=std::this_thread::get_id())return E_UNEXPECTED;
    if(heap!=D3D12TrackedHeap::Default&&heap!=D3D12TrackedHeap::Upload&&heap!=D3D12TrackedHeap::Readback)return E_INVALIDARG;
    Microsoft::WRL::ComPtr<ID3D12Device> device;
    if(FAILED(resource.GetDevice(IID_PPV_ARGS(device.GetAddressOf())))||device.Get()!=device_)return E_INVALIDARG;
    if(const auto previous=Find(resource))return previous->heap==heap?S_OK:E_INVALIDARG;
    auto record=std::make_shared<D3D12StateRecord>();record->description=resource.GetDesc();record->heap=heap;record->name=std::move(name);
    if(!ValidUsage(*record,initial))return E_INVALIDARG;
    UINT64 count=1;
    if(record->description.Dimension!=D3D12_RESOURCE_DIMENSION_BUFFER)
    {
        D3D12_FEATURE_DATA_FORMAT_INFO format{record->description.Format,0};
        if(FAILED(device_->CheckFeatureSupport(D3D12_FEATURE_FORMAT_INFO,&format,sizeof(format)))||!format.PlaneCount)return E_INVALIDARG;
        count=UINT64{record->description.MipLevels}*format.PlaneCount
            *(record->description.Dimension==D3D12_RESOURCE_DIMENSION_TEXTURE3D?1:record->description.DepthOrArraySize);
    }
    if(!count||count>1024*1024||nextResource_==UINT64_MAX)return E_INVALIDARG;
    record->states.assign(static_cast<size_t>(count),initial);record->serial=nextResource_++;
    record->attached.store(true,std::memory_order_release);
    auto* holder=new PrivateOwner<D3D12StateRecord>(record);
    const auto result=resource.SetPrivateDataInterface(resourceId,holder);holder->Release();
    if(FAILED(result))return result;
    records_.insert_or_assign(&resource,record);return S_OK;
}
std::shared_ptr<D3D12StateRecord> D3D12StateRegistry::Lookup(ID3D12Resource& resource) const
{
    const auto found=records_.find(&resource);if(found==records_.end())return {};
    auto record=found->second.lock();if(!record||!record->attached.load(std::memory_order_acquire))return {};
    Microsoft::WRL::ComPtr<IUnknown> holder;UINT bytes=sizeof(IUnknown*);
    if(FAILED(resource.GetPrivateData(resourceId,&bytes,holder.GetAddressOf()))||!holder)return {};
    return static_cast<PrivateOwner<D3D12StateRecord>*>(holder.Get())->value==record?record:nullptr;
}
std::shared_ptr<const D3D12StateRecord> D3D12StateRegistry::Find(ID3D12Resource& resource) const{return Lookup(resource);}
HRESULT D3D12StateRegistry::RequireActivation(ID3D12Resource& resource)
{
    if(owner_!=std::this_thread::get_id())return E_UNEXPECTED;
    const auto record=Lookup(resource);if(!record||record->heap!=D3D12TrackedHeap::Default||record->lastSubmission)return E_INVALIDARG;
    record->activationPending=true;return S_OK;
}
void D3D12StateRegistry::SetName(ID3D12Resource& resource,const std::string& name)
{const auto found=records_.find(&resource);if(found!=records_.end())if(auto value=found->second.lock())value->name=name;}
void D3D12StateRegistry::Collect()
{std::erase_if(records_,[](const auto& entry){const auto value=entry.second.lock();return !value||!value->attached.load(std::memory_order_acquire);});}
D3D12StateStatistics D3D12StateRegistry::Stats() const
{
    D3D12StateStatistics stats;stats.submissions=submissions_;stats.transitions=transitions_;stats.bufferDecays=decays_;stats.activations=activations_;stats.uavBarriers=uavBarriers_;
    for(const auto& [_,weak]:records_)if(const auto record=weak.lock();record&&record->attached.load(std::memory_order_acquire))
    {++stats.resources;stats.subresources+=record->states.size();}
    return stats;
}
HRESULT D3D12StateRecording::Use(ID3D12GraphicsCommandList& commands,ID3D12Resource& resource,D3D12_RESOURCE_STATES desired,
    const char* pass,UINT first,UINT count)
{
    if(committed_||registry_.owner_!=std::this_thread::get_id()||!registry_.queue_||!pass)return E_UNEXPECTED;
    auto found=local_.find(&resource);
    if(found==local_.end())
    {
        auto record=registry_.Lookup(resource);
        if(!record||!ValidUsage(*record,desired))return E_INVALIDARG;
        Local next;next.record=record;next.resource=&resource;next.states=record->states;
        found=local_.emplace(&resource,std::move(next)).first;
    }
    auto& local=found->second;if(!ValidUsage(*local.record,desired))return E_INVALIDARG;
    if(count==UINT_MAX){if(first)return E_INVALIDARG;count=static_cast<UINT>(local.states.size());}
    if(!count||first>=local.states.size()||count>local.states.size()-first)return E_INVALIDARG;
    if(local.record->activationPending&&!local.activate)
    {
        D3D12_RESOURCE_BARRIER barrier{};barrier.Type=D3D12_RESOURCE_BARRIER_TYPE_ALIASING;barrier.Aliasing={nullptr,&resource};
        commands.ResourceBarrier(1,&barrier);local.activate=true;
        events_.push_back({local.record->serial,0,local.record->name,pass,D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES,
            local.states[0],local.states[0],false,true});
    }
    std::vector<D3D12_RESOURCE_BARRIER> barriers;
    const bool uniform=first==0&&count==local.states.size()&&std::all_of(local.states.begin(),local.states.end(),[&](auto state){return state==local.states[0];});
    for(UINT index=first;index<first+count;++index)
    {
        if(Compatible(local.states[index],desired))continue;
        const auto subresource=uniform?D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES:index;
        D3D12_RESOURCE_BARRIER barrier{};barrier.Type=D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
        barrier.Transition={&resource,subresource,local.states[index],desired};barriers.push_back(barrier);
        events_.push_back({local.record->serial,0,local.record->name,pass,subresource,local.states[index],desired,false});
        if(uniform){std::fill(local.states.begin(),local.states.end(),desired);break;}
        local.states[index]=desired;
    }
    if(!barriers.empty())commands.ResourceBarrier(static_cast<UINT>(barriers.size()),barriers.data());return S_OK;
}
HRESULT D3D12StateRecording::UavBarrier(ID3D12GraphicsCommandList& commands,ID3D12Resource& resource,const char* pass)
{
    if(committed_||registry_.owner_!=std::this_thread::get_id()||!registry_.queue_||!pass)return E_UNEXPECTED;
    const auto found=local_.find(&resource);if(found==local_.end())return E_INVALIDARG;
    const auto& local=found->second;
    if(!ValidUsage(*local.record,D3D12_RESOURCE_STATE_UNORDERED_ACCESS)
        ||!std::all_of(local.states.begin(),local.states.end(),[](auto state){return state==D3D12_RESOURCE_STATE_UNORDERED_ACCESS;}))return E_INVALIDARG;
    D3D12_RESOURCE_BARRIER barrier{};barrier.Type=D3D12_RESOURCE_BARRIER_TYPE_UAV;barrier.UAV.pResource=&resource;
    commands.ResourceBarrier(1,&barrier);
    events_.push_back({local.record->serial,0,local.record->name,pass,D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES,
        D3D12_RESOURCE_STATE_UNORDERED_ACCESS,D3D12_RESOURCE_STATE_UNORDERED_ACCESS,false,false,true});return S_OK;
}
void D3D12StateRecording::Commit()
{
    if(committed_)return;
    const auto submission=++registry_.submissions_;
    std::vector<D3D12StateEvent> decays;
    for(auto& [_,local]:local_)
    {
        local.record->states=std::move(local.states);local.record->lastSubmission=submission;
        if(local.activate)local.record->activationPending=false;
        if(local.record->heap==D3D12TrackedHeap::Default&&local.record->description.Dimension==D3D12_RESOURCE_DIMENSION_BUFFER)
        {
            const auto before=local.record->states[0];
            if(before!=D3D12_RESOURCE_STATE_COMMON)
            {
                decays.push_back({local.record->serial,submission,local.record->name,"ExecuteBoundary",0,before,D3D12_RESOURCE_STATE_COMMON,true});
                ++registry_.decays_;
            }
            std::fill(local.record->states.begin(),local.record->states.end(),D3D12_RESOURCE_STATE_COMMON);
        }
    }
    for(const auto& event:events_)if(event.activation)++registry_.activations_;else if(event.uav)++registry_.uavBarriers_;else ++registry_.transitions_;
    for(auto& event:events_)
    {event.submission=submission;registry_.events_.push_back(std::move(event));if(registry_.events_.size()>256)registry_.events_.pop_front();}
    for(auto& event:decays)
    {registry_.events_.push_back(std::move(event));if(registry_.events_.size()>256)registry_.events_.pop_front();}
    committed_=true;
}
