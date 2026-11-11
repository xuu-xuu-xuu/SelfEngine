#include "GpuVisibilityExperiment.h"
#include "PlatformLayer/Graphics/D3D12/D3D12Context.h"
#include "PlatformLayer/Graphics/D3D12/D3D12Visibility.h"
#include <d3d12sdklayers.h>
#include <wrl/wrappers/corewrappers.h>
#include <algorithm>
#include <chrono>
#include <cstring>
#include <stdexcept>

using Microsoft::WRL::ComPtr;
namespace se
{
namespace
{
using Clock=std::chrono::steady_clock;
double Ms(Clock::time_point start){return std::chrono::duration<double,std::milli>(Clock::now()-start).count();}
void Check(bool value,const char* message){if(!value)throw std::runtime_error(message);}
void Hr(HRESULT result,const char* message){if(FAILED(result))throw std::runtime_error(std::string(message)+": "+std::to_string(static_cast<unsigned long>(result)));}
D3D12_RESOURCE_DESC Buffer(UINT64 bytes)
{
    D3D12_RESOURCE_DESC d{};d.Dimension=D3D12_RESOURCE_DIMENSION_BUFFER;d.Width=bytes;d.Height=d.DepthOrArraySize=d.MipLevels=1;
    d.SampleDesc={1,0};d.Layout=D3D12_TEXTURE_LAYOUT_ROW_MAJOR;return d;
}
}
struct GpuVisibilityExperiment::Impl
{
    struct Slot
    {
        ComPtr<ID3D12CommandAllocator> allocator;
        ComPtr<ID3D12GraphicsCommandList> commands;
        ComPtr<ID3D12Resource> timings,readback;
        D3D12UploadArena uploads;
        D3D12VisibilityOutput output;
        GpuVisibilityMeasurement pending;
        UINT64 fence=0;
        bool occupied=false;
    };
    D3D12Context context;
    D3D12Visibility kernel;
    ComPtr<ID3D12CommandQueue> queue;
    ComPtr<ID3D12Fence> fence;
    ComPtr<ID3D12QueryHeap> queries;
    Microsoft::WRL::Wrappers::Event event;
    std::array<Slot,2> slots;
    std::vector<GpuVisibilityMeasurement> results;
    DWORD owner=GetCurrentThreadId();
    UINT cursor=0,storageCapacity=0;
    UINT64 nextFence=1,sequence=0,frequency=0,waits=0,resultReads=0,drains=0,readbackBytes=0;
    bool diagnostic=false,stopped=false,untracked=false;
    double initialization=0;
    std::string adapter;

    void Owner() const{Check(GetCurrentThreadId()==owner,"visibility experiment wrong thread");}
    void Readback(UINT64 bytes,ComPtr<ID3D12Resource>& output,const char* name)
    {
        D3D12_HEAP_PROPERTIES heap{};heap.Type=D3D12_HEAP_TYPE_READBACK;heap.CreationNodeMask=heap.VisibleNodeMask=1;
        const auto description=Buffer(bytes);
        Hr(context.device->CreateCommittedResource(&heap,D3D12_HEAP_FLAG_NONE,&description,D3D12_RESOURCE_STATE_COPY_DEST,nullptr,IID_PPV_ARGS(output.GetAddressOf())),"visibility readback allocation");
        Hr(context.states->Register(*output.Get(),D3D12_RESOURCE_STATE_COPY_DEST,D3D12TrackedHeap::Readback,name),"visibility readback state");
        readbackBytes+=context.device->GetResourceAllocationInfo(0,1,&description).SizeInBytes;
    }
    Impl(const Localization& text,const std::filesystem::path& shader,bool diagnostics,UINT capacity):storageCapacity(capacity),diagnostic(diagnostics)
    {
        const auto start=Clock::now();Check(capacity<=MaximumVisibilityItems,"visibility storage capacity");
        Check(InitializeD3D12Context(context,text),"visibility device initialization");
        D3D12_COMMAND_QUEUE_DESC q{};q.Type=D3D12_COMMAND_LIST_TYPE_DIRECT;
        Hr(context.device->CreateCommandQueue(&q,IID_PPV_ARGS(queue.GetAddressOf())),"visibility direct queue");
        Hr(context.states->BindQueue(*queue.Get()),"visibility single queue registry");
        Hr(context.device->CreateFence(0,D3D12_FENCE_FLAG_NONE,IID_PPV_ARGS(fence.GetAddressOf())),"visibility fence");
        event.Attach(CreateEventW(nullptr,FALSE,FALSE,nullptr));Check(event.IsValid(),"visibility event");
        Hr(queue->GetTimestampFrequency(&frequency),"visibility timestamp frequency");Check(frequency!=0,"visibility nonzero frequency");
        D3D12_QUERY_HEAP_DESC query{};query.Type=D3D12_QUERY_HEAP_TYPE_TIMESTAMP;query.Count=8;
        Hr(context.device->CreateQueryHeap(&query,IID_PPV_ARGS(queries.GetAddressOf())),"visibility timestamp heap");
        Hr(kernel.Initialize(context,shader,text),"visibility kernel initialization");
        for(auto& slot:slots)
        {
            Hr(context.device->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT,IID_PPV_ARGS(slot.allocator.GetAddressOf())),"visibility allocator");
            Hr(context.device->CreateCommandList(0,D3D12_COMMAND_LIST_TYPE_DIRECT,slot.allocator.Get(),nullptr,IID_PPV_ARGS(slot.commands.GetAddressOf())),"visibility command list");
            Hr(slot.commands->Close(),"visibility initial close");slot.uploads.Initialize(*context.device.Get(),context.states);
            Hr(kernel.CreateOutput(capacity,slot.output),"visibility output allocation");Readback(32,slot.timings,"Visibility.Timestamps");
            if(diagnostic)Readback(16+(UINT64{capacity}+2)*4,slot.readback,"Visibility.ValidationReadback");
        }
        DXGI_ADAPTER_DESC1 description{};Hr(context.adapter->GetDesc1(&description),"visibility adapter description");
        const int bytes=WideCharToMultiByte(CP_UTF8,WC_ERR_INVALID_CHARS,description.Description,-1,nullptr,0,nullptr,nullptr);
        Check(bytes>0,"visibility adapter UTF-8");adapter.resize(static_cast<std::size_t>(bytes));
        Check(WideCharToMultiByte(CP_UTF8,WC_ERR_INVALID_CHARS,description.Description,-1,adapter.data(),bytes,nullptr,nullptr)!=0,"visibility adapter UTF-8 conversion");adapter.pop_back();
        initialization=Ms(start);
    }
    bool Wait(UINT64 target)
    {
        auto completed=fence->GetCompletedValue();Check(completed!=UINT64_MAX,"visibility device removed");
        if(completed>=target)return false;
        const auto deadline=GetTickCount64()+10000;
        while(completed<target)
        {
            Hr(fence->SetEventOnCompletion(target,event.Get()),"visibility fence event");
            const auto now=GetTickCount64();Check(now<deadline,"visibility fence timeout");
            const auto status=WaitForSingleObject(event.Get(),static_cast<DWORD>(deadline-now));
            Check(status==WAIT_OBJECT_0||status==WAIT_TIMEOUT,"visibility fence wait");
            completed=fence->GetCompletedValue();Check(completed!=UINT64_MAX,"visibility device removed");
            Check(status!=WAIT_TIMEOUT||completed>=target,"visibility fence timeout");
        }
        return true;
    }
    double Collect(Slot& slot,bool reuse)
    {
        if(!slot.occupied)return 0;
        const auto waitStart=Clock::now();if(Wait(slot.fence)&&reuse)++waits;const auto waitMs=Ms(waitStart);
        const auto readStart=Clock::now();auto& m=slot.pending;m.frequency=frequency;
        D3D12_RANGE range{0,32};void* mapped=nullptr;
        Hr(slot.timings->Map(0,&range,&mapped),"visibility timestamps map");std::memcpy(m.ticks.data(),mapped,32);
        D3D12_RANGE none{0,0};slot.timings->Unmap(0,&none);
        Check(std::is_sorted(m.ticks.begin(),m.ticks.end()),"visibility timestamp order");
        m.setupResetMs=double(m.ticks[1]-m.ticks[0])*1000/double(frequency);
        m.dependencyCullMs=double(m.ticks[2]-m.ticks[1])*1000/double(frequency);
        m.readbackCopyMs=diagnostic?double(m.ticks[3]-m.ticks[2])*1000/double(frequency):0;
        if(diagnostic)
        {
            range={0,static_cast<SIZE_T>(16+(UINT64{m.capacity}+2)*4)};
            std::vector<UINT> values(range.End/4);
            Hr(slot.readback->Map(0,&range,&mapped),"visibility results map");
            // 先复制，再解除Map；后续诊断异常不会遗留映射。
            std::memcpy(values.data(),mapped,range.End);slot.readback->Unmap(0,&none);
            m.total=values[0];m.written=values[1];m.overflow=values[2];
            Check(m.total<=m.inputCount&&m.written==std::min(m.total,m.capacity)&&m.overflow==UINT(m.total>m.capacity)&&values[3]==0,"visibility counter contract");
            m.guards=values[4+m.capacity]==0x13579bdfu&&values[5+m.capacity]==0x2468ace0u;
            Check(m.guards,"visibility capacity guard");m.ids.assign(values.begin()+4,values.begin()+4+m.written);++resultReads;
        }
        m.readMs=Ms(readStart);results.push_back(std::move(m));slot.occupied=false;return waitMs;
    }
    UINT64 Submit(std::span<const VisibilityItem> items,const VisibilityFrustum& frustum,UINT capacity)
    {
        const auto frameStart=Clock::now();
        Owner();Check(!stopped&&!untracked,"visibility experiment not accepting submissions");
        Check(items.size()<=MaximumVisibilityItems&&capacity<=storageCapacity&&ValidVisibilityFrustum(frustum),"visibility invalid input");
        auto& slot=slots[cursor];const auto reuse=Collect(slot,true);
        slot.uploads.Reset();Hr(slot.allocator->Reset(),"visibility allocator reset");Hr(slot.commands->Reset(slot.allocator.Get(),nullptr),"visibility list reset");
        GpuVisibilityMeasurement m;m.sequence=++sequence;m.inputCount=static_cast<UINT>(items.size());m.capacity=capacity;m.diagnostic=diagnostic;m.reuseWaitMs=reuse;
        auto start=Clock::now();D3D12VisibilityPrepared prepared;
        Hr(kernel.Prepare(slot.uploads,items,frustum,capacity,prepared),"visibility input preparation");m.prepareMs=Ms(start);
        start=Clock::now();D3D12StateRecording states(*context.states);const UINT first=cursor*4;
        Hr(kernel.Record(*slot.commands.Get(),states,prepared,slot.output,{queries.Get(),8},first),"visibility dispatch recording");
        if(diagnostic)
        {
            Hr(states.Use(*slot.commands.Get(),*slot.output.ids.Get(),D3D12_RESOURCE_STATE_COPY_SOURCE,"Visibility.ValidationCopy"),"visibility id copy state");
            Hr(states.Use(*slot.commands.Get(),*slot.output.counters.Get(),D3D12_RESOURCE_STATE_COPY_SOURCE,"Visibility.ValidationCopy"),"visibility counter copy state");
            Hr(states.Use(*slot.commands.Get(),*slot.readback.Get(),D3D12_RESOURCE_STATE_COPY_DEST,"Visibility.ValidationCopy"),"visibility readback copy state");
            slot.commands->CopyBufferRegion(slot.readback.Get(),0,slot.output.counters.Get(),0,16);
            slot.commands->CopyBufferRegion(slot.readback.Get(),16,slot.output.ids.Get(),0,(UINT64{capacity}+2)*4);
        }
        slot.commands->EndQuery(queries.Get(),D3D12_QUERY_TYPE_TIMESTAMP,first+3);
        Hr(states.Use(*slot.commands.Get(),*slot.timings.Get(),D3D12_RESOURCE_STATE_COPY_DEST,"Visibility.TimestampResolve"),"visibility timestamp state");
        slot.commands->ResolveQueryData(queries.Get(),D3D12_QUERY_TYPE_TIMESTAMP,first,4,slot.timings.Get(),0);
        Hr(slot.commands->Close(),"visibility list close");m.recordMs=Ms(start);
        Check(nextFence<UINT64_MAX,"visibility fence exhausted");slot.fence=nextFence;slot.pending=std::move(m);slot.occupied=true;
        start=Clock::now();ID3D12CommandList* lists[]{slot.commands.Get()};untracked=true;queue->ExecuteCommandLists(1,lists);states.Commit();
        Hr(queue->Signal(fence.Get(),nextFence),"visibility queue signal");++nextFence;untracked=false;
        slot.pending.submitMs=Ms(start);slot.pending.cpuFrameMs=Ms(frameStart);cursor=(cursor+1)%2;return sequence;
    }
    void Drain()
    {
        Owner();if(stopped)return;++drains;
        if(untracked)
        {
            Check(nextFence<UINT64_MAX,"visibility recovery fence exhausted");
            Hr(queue->Signal(fence.Get(),nextFence),"visibility recovery signal");
            for(auto& slot:slots)if(slot.occupied)slot.fence=nextFence;
            ++nextFence;untracked=false;
        }
        for(auto& slot:slots)Collect(slot,false);
    }
    GpuVisibilityStatistics Statistics() const
    {
        Owner();GpuVisibilityStatistics s;s.memory=context.memory->Stats();s.states=context.states->Stats();s.adapter=adapter;
        s.submissions=sequence;s.reuseWaits=waits;s.resultReadbacks=resultReads;s.drains=drains;s.stopped=stopped;s.initializationMs=initialization;
        s.readbackAllocationBytes=stopped?0:readbackBytes;
        for(const auto& slot:slots){s.uploadReservedBytes+=slot.uploads.Stats().reservedBytes;s.pending+=slot.occupied;s.resultReadbackResources+=bool(slot.readback);}
        ComPtr<ID3D12InfoQueue> info;context.device.As(&info);s.debugInfoQueue=bool(info);
        if(info)for(UINT64 index=0;index<info->GetNumStoredMessages();++index)
        {
            SIZE_T bytes=0;Hr(info->GetMessage(index,nullptr,&bytes),"visibility diagnostic length");std::vector<std::byte> data(bytes);
            auto* message=reinterpret_cast<D3D12_MESSAGE*>(data.data());Hr(info->GetMessage(index,message,&bytes),"visibility diagnostic message");
            s.warnings+=message->Severity==D3D12_MESSAGE_SEVERITY_WARNING;s.errors+=message->Severity<D3D12_MESSAGE_SEVERITY_WARNING;
        }
        return s;
    }
    void Shutdown()
    {
        Owner();if(stopped)return;Drain();
        for(auto& slot:slots){slot.commands.Reset();slot.allocator.Reset();slot.output={};slot.readback.Reset();slot.timings.Reset();slot.uploads.Release();}
        queries.Reset();kernel={};context.states->UnbindQueue(queue.Get());queue.Reset();fence.Reset();event.Close();
        context.states->Collect();context.memory->Trim();stopped=true;
    }
};
GpuVisibilityExperiment::GpuVisibilityExperiment(const Localization& text,const std::filesystem::path& shader,bool diagnostic,UINT capacity)
    :impl_(std::make_unique<Impl>(text,shader,diagnostic,capacity)){}
GpuVisibilityExperiment::~GpuVisibilityExperiment()
{
    if(!impl_)return;
    try{impl_->Shutdown();}
    catch(...)
    {
        // 无法证明完成且设备未移除时，保留整组在途资源至进程退出，不能强行释放。
        if(impl_->context.device&&impl_->context.device->GetDeviceRemovedReason()==S_OK)(void)impl_.release();
    }
}
std::uint64_t GpuVisibilityExperiment::Submit(std::span<const VisibilityItem> items,const VisibilityFrustum& frustum,UINT capacity){return impl_->Submit(items,frustum,capacity);}
void GpuVisibilityExperiment::Drain(){impl_->Drain();}
void GpuVisibilityExperiment::Shutdown(){impl_->Shutdown();}
std::vector<GpuVisibilityMeasurement> GpuVisibilityExperiment::TakeResults()
{
    impl_->Owner();auto results=std::move(impl_->results);impl_->results.clear();
    std::sort(results.begin(),results.end(),[](const auto& a,const auto& b){return a.sequence<b.sequence;});return results;
}
GpuVisibilityStatistics GpuVisibilityExperiment::Statistics() const{return impl_->Statistics();}
ID3D12Device& GpuVisibilityExperiment::NativeDevice() const{return *impl_->context.device.Get();}
ID3D12CommandQueue& GpuVisibilityExperiment::Queue() const{return *impl_->queue.Get();}
}
