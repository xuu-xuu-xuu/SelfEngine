#include "PlatformLayer/Graphics/D3D12/D3D12Indirect.h"
#include "PlatformLayer/Graphics/D3D12/D3D12Context.h"
#include "PlatformLayer/Graphics/D3D12/D3D12Memory.h"
#include "PlatformLayer/Graphics/D3D12/D3D12StateTracker.h"
#include "PlatformLayer/Windows/ExecutablePath.h"
#include "CoreLayer/Localization.h"
#include <d3d12sdklayers.h>
#include <nlohmann/json.hpp>
#include <wrl/wrappers/corewrappers.h>
#include <algorithm>
#include <atomic>
#include <cstring>
#include <fstream>
#include <iostream>
#include <limits>
#include <stdexcept>
#include <thread>

using namespace se;
using Microsoft::WRL::ComPtr;
namespace fs=std::filesystem;
namespace
{
void Check(bool value,const char* message){if(!value)throw std::runtime_error(message);}
void Hr(HRESULT result,const char* message){Check(SUCCEEDED(result),message);}
VisibilityFrustum FrustumData()
{
    VisibilityFrustum f;f.planes={std::array<float,4>{1,0,0,1},{-1,0,0,1},{0,1,0,1},{0,-1,0,1},{0,0,1,1},{0,0,-1,1}};return f;
}
std::vector<VisibilityItem> Items(UINT count,UINT rate)
{
    std::vector<VisibilityItem> items;items.reserve(count);
    for(UINT i=0;i<count;++i){const float x=rate==0||(rate==1&&i%10!=0)?10.0f:0.0f;items.push_back({{x-.05f,-.1f,-.1f},i,{x+.05f,.1f,.1f},0});}
    return items;
}
std::vector<rhi::InstanceGpuData> Data(UINT count)
{
    std::vector<rhi::InstanceGpuData> data(count);
    for(UINT i=0;i<count;++i)
    {
        // 只验证搬运，覆盖所有60个word（含任意浮点位型），没有第二套wire布局。
        std::array<UINT,60> words{};for(UINT j=0;j<60;++j)words[j]=(i+1)*0x601a9u+j*0x1357u;
        std::memcpy(&data[i],words.data(),sizeof(rhi::InstanceGpuData));
    }
    return data;
}
D3D12_RESOURCE_DESC Buffer(UINT64 bytes)
{
    D3D12_RESOURCE_DESC d{};d.Dimension=D3D12_RESOURCE_DIMENSION_BUFFER;d.Width=bytes;
    d.Height=d.DepthOrArraySize=d.MipLevels=1;d.SampleDesc={1,0};d.Layout=D3D12_TEXTURE_LAYOUT_ROW_MAJOR;return d;
}
struct Fixture
{
    struct Slot
    {
        ComPtr<ID3D12CommandAllocator> allocator;ComPtr<ID3D12GraphicsCommandList> commands;
        ComPtr<ID3D12Resource> readback,timings;D3D12UploadArena uploads;D3D12IndirectOutput output;
        UINT64 fence=0;UINT capacity=0,total=0;std::string name;std::vector<rhi::InstanceGpuData> expected;
    };
    D3D12Context context;D3D12Indirect kernel;ComPtr<ID3D12CommandQueue> queue;ComPtr<ID3D12Fence> fence;
    ComPtr<ID3D12QueryHeap> queries;Microsoft::WRL::Wrappers::Event event;std::array<Slot,2> slots;
    UINT cursor=0;UINT64 next=1,frequency=0,readbacks=0;bool stopped=false;
    nlohmann::json cases=nlohmann::json::array();
    explicit Fixture(const Localization& text)
    {
        Check(InitializeD3D12Context(context,text),"indirect device");
        D3D12_COMMAND_QUEUE_DESC q{};q.Type=D3D12_COMMAND_LIST_TYPE_DIRECT;
        Hr(context.device->CreateCommandQueue(&q,IID_PPV_ARGS(queue.GetAddressOf())),"indirect queue");Hr(context.states->BindQueue(*queue.Get()),"indirect queue ownership");
        Hr(context.device->CreateFence(0,D3D12_FENCE_FLAG_NONE,IID_PPV_ARGS(fence.GetAddressOf())),"indirect fence");
        event.Attach(CreateEventW(nullptr,FALSE,FALSE,nullptr));Check(event.IsValid(),"indirect event");
        Hr(queue->GetTimestampFrequency(&frequency),"indirect frequency");Check(frequency>0,"indirect nonzero frequency");
        Hr(kernel.Initialize(context,GetExecutableDirectory()/"Experiments/Indirect.hlsl",text),"indirect kernel");
        D3D12_QUERY_HEAP_DESC query{};query.Type=D3D12_QUERY_HEAP_TYPE_TIMESTAMP;query.Count=10;
        Hr(context.device->CreateQueryHeap(&query,IID_PPV_ARGS(queries.GetAddressOf())),"indirect queries");
        for(auto& slot:slots)
        {
            Hr(context.device->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT,IID_PPV_ARGS(slot.allocator.GetAddressOf())),"indirect allocator");
            Hr(context.device->CreateCommandList(0,D3D12_COMMAND_LIST_TYPE_DIRECT,slot.allocator.Get(),nullptr,IID_PPV_ARGS(slot.commands.GetAddressOf())),"indirect commands");
            Hr(slot.commands->Close(),"initial close");slot.uploads.Initialize(*context.device.Get(),context.states);
            Hr(kernel.CreateOutput(MaximumVisibilityItems,MaximumVisibilityItems,slot.output),"indirect outputs");
            Readback(48+UINT64{MaximumVisibilityItems}*rhi::InstanceGpuStride+8,slot.readback);Readback(40,slot.timings);
        }
    }
    ~Fixture(){if(!stopped){try{for(auto& slot:slots)if(slot.fence)Wait(slot.fence);}catch(...){std::terminate();}}}
    void Readback(UINT64 bytes,ComPtr<ID3D12Resource>& output)
    {
        D3D12_HEAP_PROPERTIES heap{};heap.Type=D3D12_HEAP_TYPE_READBACK;heap.CreationNodeMask=heap.VisibleNodeMask=1;const auto d=Buffer(bytes);
        Hr(context.device->CreateCommittedResource(&heap,D3D12_HEAP_FLAG_NONE,&d,D3D12_RESOURCE_STATE_COPY_DEST,nullptr,IID_PPV_ARGS(output.GetAddressOf())),"indirect readback");
        Hr(context.states->Register(*output.Get(),D3D12_RESOURCE_STATE_COPY_DEST,D3D12TrackedHeap::Readback,"Indirect.TestReadback"),"readback state");
    }
    void Wait(UINT64 target)
    {
        const auto completed=fence->GetCompletedValue();Check(completed!=UINT64_MAX,"indirect device removed");if(completed>=target)return;
        Hr(fence->SetEventOnCompletion(target,event.Get()),"indirect completion event");
        Check(WaitForSingleObject(event.Get(),10000)==WAIT_OBJECT_0,"indirect completion timeout");
        const auto after=fence->GetCompletedValue();Check(after!=UINT64_MAX&&after>=target,"indirect completion verified");
    }
    void Collect(Slot& slot)
    {
        if(!slot.fence)return;Wait(slot.fence);
        const SIZE_T bytes=48+SIZE_T{slot.capacity}*rhi::InstanceGpuStride+8;
        std::vector<std::byte> actual(bytes);void* mapped=nullptr;D3D12_RANGE range{0,bytes},none{0,0};
        Hr(slot.readback->Map(0,&range,&mapped),"map indirect validation");std::memcpy(actual.data(),mapped,bytes);slot.readback->Unmap(0,&none);
        std::array<UINT,12> a{};std::memcpy(a.data(),actual.data(),48);const auto written=static_cast<UINT>(slot.expected.size());
        const std::array<UINT,12> expected{36,written,0,0,0,written?1u:0u,slot.total,written,slot.total>slot.capacity?1u:0u,0,0x13579bdf,0x2468ace0};
        Check(a==expected,"all draw arguments/counts/overflow/guards match");
        if(!slot.expected.empty())Check(std::memcmp(actual.data()+48,slot.expected.data(),slot.expected.size()*rhi::InstanceGpuStride)==0,"stable compacted InstanceGpuData bytes match CPU prefix");
        std::array<UINT,2> guards{};std::memcpy(guards.data(),actual.data()+48+SIZE_T{slot.capacity}*rhi::InstanceGpuStride,8);
        Check(guards==std::array<UINT,2>{0x13579bdf,0x2468ace0},"compacted instance boundary guards");
        range={0,40};Hr(slot.timings->Map(0,&range,&mapped),"map indirect raw timestamps");std::array<UINT64,5> ticks{};
        std::memcpy(ticks.data(),mapped,40);slot.timings->Unmap(0,&none);Check(ticks[0]>0&&std::is_sorted(ticks.begin(),ticks.end()),"indirect timestamp order");
        cases.push_back({{"name",slot.name},{"capacity",slot.capacity},{"visible",slot.total},{"arguments",a},{"stable_bytes",true},{"guards",true},{"ticks",ticks},{"frequency_hz",frequency}});
        ++readbacks;slot.fence=0;slot.expected.clear();
    }
    void Submit(std::string name,const std::vector<VisibilityItem>& items,UINT capacity,const VisibilityFrustum& frustum=FrustumData())
    {
        auto data=Data(static_cast<UINT>(items.size()));std::vector<UINT> ids;Check(CullVisibilityReference(items,frustum,ids),"indirect CPU reference");
        auto& slot=slots[cursor];Collect(slot);slot.uploads.Reset();Hr(slot.allocator->Reset(),"reset indirect allocator");Hr(slot.commands->Reset(slot.allocator.Get(),nullptr),"reset indirect list");
        slot.name=std::move(name);slot.capacity=capacity;slot.total=static_cast<UINT>(ids.size());
        for(size_t i=0;i<std::min<size_t>(capacity,ids.size());++i)slot.expected.push_back(data.at(ids[i]));
        D3D12UploadSlice input;
        if(!data.empty()){Hr(slot.uploads.Allocate(data.size()*rhi::InstanceGpuStride,16,input),"test instance upload");std::memcpy(input.cpu,data.data(),data.size()*rhi::InstanceGpuStride);}
        D3D12IndirectPrepared prepared;Hr(kernel.Prepare(slot.uploads,items,frustum,input,36,capacity,prepared),"prepare indirect scan");
        D3D12StateRecording states(*context.states);const UINT first=cursor*5;
        Hr(kernel.Record(*slot.commands.Get(),states,prepared,slot.output,{queries.Get(),10},first),"record indirect scan/gather");
        // 先走真实消费者状态，再转诊断复制；间接参数/实例读取状态受同一表约束。
        Hr(states.Use(*slot.commands.Get(),*slot.output.arguments.Get(),D3D12_RESOURCE_STATE_INDIRECT_ARGUMENT,"Indirect.TestDrawState"),"argument consumption state");
        Hr(states.Use(*slot.commands.Get(),*slot.output.instances.Get(),D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE,"Indirect.TestInstanceState"),"instance consumption state");
        for(auto* resource:{slot.output.arguments.Get(),slot.output.instances.Get()})Hr(states.Use(*slot.commands.Get(),*resource,D3D12_RESOURCE_STATE_COPY_SOURCE,"Indirect.TestCopy"),"indirect copy source");
        Hr(states.Use(*slot.commands.Get(),*slot.readback.Get(),D3D12_RESOURCE_STATE_COPY_DEST,"Indirect.TestCopy"),"indirect copy destination");
        slot.commands->CopyBufferRegion(slot.readback.Get(),0,slot.output.arguments.Get(),0,48);
        slot.commands->CopyBufferRegion(slot.readback.Get(),48,slot.output.instances.Get(),0,UINT64{capacity}*rhi::InstanceGpuStride+8);
        slot.commands->EndQuery(queries.Get(),D3D12_QUERY_TYPE_TIMESTAMP,first+4);
        Hr(states.Use(*slot.commands.Get(),*slot.timings.Get(),D3D12_RESOURCE_STATE_COPY_DEST,"Indirect.TestTiming"),"indirect timing state");
        slot.commands->ResolveQueryData(queries.Get(),D3D12_QUERY_TYPE_TIMESTAMP,first,5,slot.timings.Get(),0);Hr(slot.commands->Close(),"close indirect list");
        ID3D12CommandList* lists[]{slot.commands.Get()};queue->ExecuteCommandLists(1,lists);states.Commit();slot.fence=next;
        Hr(queue->Signal(fence.Get(),next++),"indirect submit signal");cursor=(cursor+1)%2;
    }
    void Drain(){for(auto& slot:slots)Collect(slot);}
    void Shutdown()
    {
        Drain();for(auto& slot:slots){slot.commands.Reset();slot.allocator.Reset();slot.output={};slot.timings.Reset();slot.readback.Reset();slot.uploads.Release();}
        queries.Reset();kernel={};context.states->UnbindQueue(queue.Get());queue.Reset();fence.Reset();event.Close();context.states->Collect();context.memory->Trim();stopped=true;
        Check(context.states->Stats().resources==0&&context.memory->Stats().used==0&&context.memory->Stats().reserved==0,"all indirect output/upload/readback leases released");
    }
};
struct Gate
{
    ComPtr<ID3D12Fence> fence;Microsoft::WRL::Wrappers::Event stop;std::thread watchdog;std::atomic<bool> expired=false;
    explicit Gate(Fixture& f)
    {
        Hr(f.context.device->CreateFence(0,D3D12_FENCE_FLAG_NONE,IID_PPV_ARGS(fence.GetAddressOf())),"gate fence");stop.Attach(CreateEventW(nullptr,TRUE,FALSE,nullptr));Check(stop.IsValid(),"gate stop");
        Hr(f.queue->Wait(fence.Get(),1),"gate queue wait");
        try{watchdog=std::thread([this]{if(WaitForSingleObject(stop.Get(),3000)==WAIT_TIMEOUT){expired=true;fence->Signal(1);}});}
        catch(...){fence->Signal(1);throw;}
    }
    void Release(){Hr(fence->Signal(1),"gate release");SetEvent(stop.Get());}
    ~Gate(){fence->Signal(1);SetEvent(stop.Get());if(watchdog.joinable())watchdog.join();}
};
void Contracts(Fixture& f)
{
    auto& slot=f.slots[0];D3D12IndirectOutput output;
    Hr(f.kernel.CreateOutput(0,0,output),"zero-capacity indirect buffers exist");const auto* original=output.arguments.Get();
    Check(f.kernel.CreateOutput(MaximumVisibilityItems+1,0,output)==E_INVALIDARG&&output.arguments.Get()==original,"invalid capacity preserves outputs");
    D3D12IndirectPrepared prepared;auto invalid=FrustumData();invalid.planes[0]={0,0,0,1};const auto allocations=slot.uploads.Stats().allocations;
    Check(f.kernel.Prepare(slot.uploads,{},invalid,{},36,0,prepared)==E_INVALIDARG&&slot.uploads.Stats().allocations==allocations,"bad frustum rejected before allocation");
    Check(f.kernel.Prepare(slot.uploads,Items(1,2),FrustumData(),{},36,1,prepared)==E_INVALIDARG,"missing source instance rejected");
    Check(f.kernel.Prepare(slot.uploads,{},FrustumData(),{},0,0,prepared)==E_INVALIDARG,"zero mesh index count rejected");
    Hr(slot.commands->Reset(slot.allocator.Get(),nullptr),"contract commands");D3D12StateRecording states(*f.context.states);
    Check(f.kernel.Record(*slot.commands.Get(),states,prepared,output)==E_INVALIDARG,"unprepared input rejected");
    Hr(f.kernel.Prepare(slot.uploads,{},FrustumData(),{},36,1,prepared),"prepare over-physical capacity");
    Check(f.kernel.Record(*slot.commands.Get(),states,prepared,output)==E_INVALIDARG,"physical output capacity protected");
    Hr(f.kernel.Prepare(slot.uploads,{},FrustumData(),{},36,0,prepared),"prepare zero capacity");
    Check(f.kernel.Record(*slot.commands.Get(),states,prepared,output,{f.queries.Get(),3})==E_INVALIDARG&&states.PendingTransitions()==0,"insufficient query range rejected before commands");
    Hr(slot.commands->Close(),"close unsubmitted contract commands");
}
}
int wmain(int argc,wchar_t** argv)
{
    try
    {
        Check(argc==2,"supply indirect evidence directory");const fs::path output=fs::absolute(argv[1]);fs::create_directories(output);
        Localization text;std::string error;Check(text.Load(GetExecutableDirectory()/"Resources/Localization/en-US.txt",error),"indirect localization");
        Fixture f(text);Contracts(f);
        {Gate gate(f);f.Submit("two-in-flight-all",Items(65,2),65);f.Submit("two-in-flight-sparse",Items(63,1),63);
            Check(f.slots[0].fence&&f.slots[1].fence&&f.readbacks==0&&!gate.expired,"both indirect submissions return before completion");gate.Release();}
        for(UINT count:{0u,1u,63u,64u,65u,1023u,1024u,1025u,65535u,65536u})for(UINT rate:{0u,1u,2u})
            for(UINT capacity:{0u,1u,count/2,count})f.Submit(std::to_string(count)+"-"+std::to_string(rate)+"-"+std::to_string(capacity),Items(count,rate),capacity);
        const float nan=std::numeric_limits<float>::quiet_NaN(),inf=std::numeric_limits<float>::infinity(),huge=std::numeric_limits<float>::max();
        std::vector<VisibilityItem> unusual{{{nan,0,0},0,{0,0,0},0},{{2,2,2},1,{1,1,1},0},{{-inf,0,0},2,{inf,0,0},0},
            {{1,0,0},3,{2,0,0},0},{{-huge,-huge,-huge},4,{huge,huge,huge},0},{{2,0,0},5,{3,0,0},0}};
        f.Submit("invalid-and-boundary-bounds",unusual,6);
        f.Submit("reset-full",Items(65536,2),65536);f.Submit("reset-empty",{},0);f.Submit("reset-sparse",Items(65536,1),65536);f.Drain();
        Check(f.cases.size()==126,"all indirect kernel cases collected");
        ComPtr<ID3D12InfoQueue> info;f.context.device.As(&info);UINT64 warnings=0,errors=0;
#ifndef NDEBUG
        Check(bool(info),"Debug InfoQueue present");
#endif
        if(info)for(UINT64 i=0;i<info->GetNumStoredMessages();++i)
        {
            SIZE_T bytes=0;Hr(info->GetMessage(i,nullptr,&bytes),"GPU diagnostic size");std::vector<std::byte> data(bytes);auto* m=reinterpret_cast<D3D12_MESSAGE*>(data.data());
            Hr(info->GetMessage(i,m,&bytes),"GPU diagnostic");warnings+=m->Severity==D3D12_MESSAGE_SEVERITY_WARNING;errors+=m->Severity<D3D12_MESSAGE_SEVERITY_WARNING;
            if(m->Severity<=D3D12_MESSAGE_SEVERITY_WARNING)std::cerr<<m->pDescription<<'\n';
        }
        Check(warnings==0&&errors==0,"indirect GPU diagnostics clean");f.Shutdown();
        std::ofstream file(output/"verification.json",std::ios::binary);
        file<<nlohmann::json{{"success",true},{"cases",f.cases},{"two_in_flight",true},{"stable_instance_bytes",true},{"debug_info_queue",bool(info)},
            {"warnings",warnings},{"errors",errors},{"resources_after_shutdown",f.context.states->Stats().resources},{"default_bytes_after_shutdown",f.context.memory->Stats().used},
            {"draw_execution_tested",false}}.dump(2)<<'\n';file.close();Check(bool(file),"write indirect verification");
        std::cout<<"Indirect compaction: "<<f.cases.size()<<" GPU cases; stable bytes, arguments, guards and two slots passed\n";return 0;
    }
    catch(const std::exception& error){std::cerr<<error.what()<<'\n';return 1;}
}
