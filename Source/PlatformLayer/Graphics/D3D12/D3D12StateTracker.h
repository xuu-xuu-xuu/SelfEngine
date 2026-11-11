#pragma once
#include <d3d12.h>
#include <wrl/client.h>
#include <cstdint>
#include <atomic>
#include <climits>
#include <deque>
#include <memory>
#include <string>
#include <thread>
#include <unordered_map>
#include <vector>
#include <algorithm>

enum class D3D12TrackedHeap { Default, Upload, Readback };
struct D3D12StateRecord
{
    std::uint64_t serial=0,lastSubmission=0;
    std::atomic<bool> attached{false};
    std::string name;
    D3D12_RESOURCE_DESC description{};
    D3D12TrackedHeap heap=D3D12TrackedHeap::Default;
    bool activationPending=false;
    std::vector<D3D12_RESOURCE_STATES> states;
};
struct D3D12StateEvent
{
    std::uint64_t resource=0,submission=0;
    std::string name,pass;
    UINT subresource=0;
    D3D12_RESOURCE_STATES before{},after{};
    bool implicitDecay=false;
    bool activation=false;
    bool uav=false;
};
struct D3D12StateStatistics
{
    std::size_t resources=0,subresources=0;
    std::uint64_t submissions=0,transitions=0,bufferDecays=0,activations=0,uavBarriers=0;
};
class D3D12StateRecording;
// 每设备一个实例；资源的私有 IUnknown 持有状态，Registry 只保留弱引用。
class D3D12StateRegistry
{
public:
    static std::shared_ptr<D3D12StateRegistry> ForDevice(ID3D12Device& device);
    HRESULT BindQueue(ID3D12CommandQueue& queue);
    void UnbindQueue(ID3D12CommandQueue* queue) noexcept;
    HRESULT Register(ID3D12Resource& resource,D3D12_RESOURCE_STATES initial,D3D12TrackedHeap heap,std::string name);
    HRESULT RequireActivation(ID3D12Resource& resource);
    std::shared_ptr<const D3D12StateRecord> Find(ID3D12Resource& resource) const;
    void SetName(ID3D12Resource& resource,const std::string& name);
    void Collect();
    D3D12StateStatistics Stats() const;
    const std::deque<D3D12StateEvent>& Events() const {return events_;}
private:
    friend class D3D12StateRecording;
    explicit D3D12StateRegistry(ID3D12Device& device):device_(&device),owner_(std::this_thread::get_id()){}
    std::shared_ptr<D3D12StateRecord> Lookup(ID3D12Resource& resource) const;
    ID3D12Device* device_; // 不持有 COM，避免 Device 私有元数据形成循环引用。
    const std::thread::id owner_;
    ID3D12CommandQueue* queue_=nullptr; // 绑定者在 drain 后解除，当前只允许一条活动队列。
    std::unordered_map<ID3D12Resource*,std::weak_ptr<D3D12StateRecord>> records_;
    std::deque<D3D12StateEvent> events_;
    std::uint64_t nextResource_=1,submissions_=0,transitions_=0,decays_=0,activations_=0,uavBarriers_=0;
};
class D3D12StateRecording
{
public:
    explicit D3D12StateRecording(D3D12StateRegistry& registry):registry_(registry){}
    D3D12StateRecording(const D3D12StateRecording&)=delete;
    D3D12StateRecording& operator=(const D3D12StateRecording&)=delete;
    HRESULT Use(ID3D12GraphicsCommandList& commands,ID3D12Resource& resource,D3D12_RESOURCE_STATES desired,
        const char* pass,UINT first=0,UINT count=UINT_MAX);
    HRESULT UavBarrier(ID3D12GraphicsCommandList& commands,ID3D12Resource& resource,const char* pass);
    // 仅在 ExecuteCommandLists 之后调用；未提交的析构自动丢弃候选状态。
    void Commit();
    std::size_t PendingTransitions() const noexcept {return static_cast<std::size_t>(std::count_if(events_.begin(),events_.end(),[](const auto& event){return !event.activation&&!event.uav;}));}
    std::size_t PendingActivations() const noexcept {return static_cast<std::size_t>(std::count_if(events_.begin(),events_.end(),[](const auto& event){return event.activation;}));}
    std::size_t PendingUavBarriers() const noexcept {return static_cast<std::size_t>(std::count_if(events_.begin(),events_.end(),[](const auto& event){return event.uav;}));}
private:
    struct Local
    {
        std::shared_ptr<D3D12StateRecord> record;
        Microsoft::WRL::ComPtr<ID3D12Resource> resource;
        std::vector<D3D12_RESOURCE_STATES> states;
        bool activate=false;
    };
    D3D12StateRegistry& registry_;
    std::unordered_map<ID3D12Resource*,Local> local_;
    std::vector<D3D12StateEvent> events_;
    bool committed_=false;
};
