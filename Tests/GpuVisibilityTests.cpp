#include "ToolLayer/Tools/GpuVisibilityExperiment.h"
#include "PlatformLayer/Graphics/D3D12/D3D12Context.h"
#include "PlatformLayer/Graphics/D3D12/D3D12Visibility.h"
#include "PlatformLayer/Windows/ExecutablePath.h"
#include "CoreLayer/Localization.h"
#include <nlohmann/json.hpp>
#include <immintrin.h>
#include <algorithm>
#include <atomic>
#include <fstream>
#include <iostream>
#include <limits>
#include <map>
#include <random>
#include <stdexcept>
#include <thread>

using namespace se;
using Microsoft::WRL::ComPtr;
namespace fs=std::filesystem;
namespace
{
void Check(bool value,const char* message){if(!value)throw std::runtime_error(message);}
void Hr(HRESULT result,const char* message){Check(SUCCEEDED(result),message);}
VisibilityFrustum CubeFrustum()
{
    VisibilityFrustum f;f.planes={std::array<float,4>{1,0,0,1},{-1,0,0,1},{0,1,0,1},{0,-1,0,1},{0,0,1,1},{0,0,-1,1}};return f;
}
bool Oracle(const VisibilityItem& item,const VisibilityFrustum& f)
{
    if(!IsValid(Aabb{item.minimum,item.maximum}))return true;
    for(const auto& p:f.planes)
    {
        double maximum=-std::numeric_limits<double>::infinity();
        for(unsigned corner=0;corner<8;++corner)
        {
            const double x=corner&1?item.maximum.x:item.minimum.x,y=corner&2?item.maximum.y:item.minimum.y,z=corner&4?item.maximum.z:item.minimum.z;
            maximum=std::max(maximum,double(p[0])*x+double(p[1])*y+double(p[2])*z+double(p[3]));
        }
        if(maximum<0)return false;
    }
    return true;
}
std::vector<VisibilityItem> Layout(UINT count,UINT rate)
{
    std::vector<VisibilityItem> items;items.reserve(count);
    for(UINT i=0;i<count;++i)
    {
        const float x=rate==0||(rate==1&&i%10!=0)?10+float(i%19):float(i%11)*.05f-.25f;
        items.push_back({{x-.05f,-.1f,-.1f},i,{x+.05f,.1f,.1f},0});
    }
    return items;
}
struct Gate
{
    ComPtr<ID3D12Fence> fence;HANDLE cancelled=nullptr;std::thread watchdog;std::atomic<bool> timedOut=false;
    explicit Gate(GpuVisibilityExperiment& experiment)
    {
        Hr(experiment.NativeDevice().CreateFence(0,D3D12_FENCE_FLAG_NONE,IID_PPV_ARGS(fence.GetAddressOf())),"create controlled gate");
        cancelled=CreateEventW(nullptr,TRUE,FALSE,nullptr);Check(cancelled!=nullptr,"gate cancellation event");
        watchdog=std::thread([this]{if(WaitForSingleObject(cancelled,3000)==WAIT_TIMEOUT){timedOut=true;fence->Signal(1);}});
        Hr(experiment.Queue().Wait(fence.Get(),1),"queue waits on controlled gate");
    }
    void Release(){Hr(fence->Signal(1),"release controlled gate");SetEvent(cancelled);}
    ~Gate(){fence->Signal(1);SetEvent(cancelled);if(watchdog.joinable())watchdog.join();CloseHandle(cancelled);}
};
void Contracts(const Localization& text,const fs::path& shader)
{
    const auto f=CubeFrustum();auto invalid=f;invalid.planes[2][0]=std::numeric_limits<float>::quiet_NaN();
    std::vector<UINT> result{77};Check(!CullVisibilityReference({},invalid,result)&&result==std::vector<UINT>{77},"invalid plane preserves CPU output");
    auto large=Layout(MaximumVisibilityItems+1,2);Check(!CullVisibilityReference(large,f,result)&&result==std::vector<UINT>{77},"CPU input limit preserves output");
    const auto original=_mm_getcsr();const auto altered=(original&~_MM_ROUND_MASK)|_MM_ROUND_DOWN;
    _mm_setcsr(altered);const bool valid=CullVisibilityReference(Layout(65,2),f,result);const auto restored=_mm_getcsr();_mm_setcsr(original);
    Check(valid&&result.size()==65&&restored==altered,"CPU reference restores floating-point environment");
    Frustum source{};VisibilityFrustum quantized=f;
    Check(!QuantizeVisibilityFrustum(source,quantized)&&quantized.planes==f.planes,"invalid quantization preserves output");
    const auto real=BuildFrustum(*PerspectiveRH(Pi/3,16.0f/9,.1f,100));Check(real&&QuantizeVisibilityFrustum(*real,quantized),"existing RH frustum quantization");
    D3D12Context context;Check(InitializeD3D12Context(context,text),"contract device");
    D3D12Visibility kernel;D3D12VisibilityOutput output;
    Check(FAILED(kernel.CreateOutput(0,output)),"uninitialized kernel rejected");Hr(kernel.Initialize(context,shader,text),"contract kernel");
    Check(FAILED(kernel.Initialize(context,shader,text)),"reinitialization rejected");Hr(kernel.CreateOutput(0,output),"zero-capacity physical guards");
    const auto* old=output.ids.Get();Check(FAILED(kernel.CreateOutput(MaximumVisibilityItems+1,output))&&output.ids.Get()==old,"invalid capacity preserves output");
    D3D12UploadArena arena;arena.Initialize(*context.device.Get(),context.states);D3D12VisibilityPrepared prepared;
    Check(FAILED(kernel.Prepare(arena,{},invalid,0,prepared))&&arena.Stats().allocations==0,"invalid planes allocate nothing");
    Check(FAILED(kernel.Prepare(arena,large,f,0,prepared))&&arena.Stats().allocations==0,"large input allocates nothing");
    Check(FAILED(kernel.Prepare(arena,{},f,MaximumVisibilityItems+1,prepared)),"large requested capacity rejected");
    ComPtr<ID3D12CommandAllocator> allocator;ComPtr<ID3D12GraphicsCommandList> commands;ComPtr<ID3D12QueryHeap> queries;
    Hr(context.device->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT,IID_PPV_ARGS(allocator.GetAddressOf())),"contract allocator");
    Hr(context.device->CreateCommandList(0,D3D12_COMMAND_LIST_TYPE_DIRECT,allocator.Get(),nullptr,IID_PPV_ARGS(commands.GetAddressOf())),"contract list");
    D3D12StateRecording states(*context.states);
    Check(kernel.Record(*commands.Get(),states,prepared,output)==E_INVALIDARG,"unprepared data rejected before recording");
    Hr(kernel.Prepare(arena,{},f,1,prepared),"prepare capacity beyond physical output");
    Check(kernel.Record(*commands.Get(),states,prepared,output)==E_INVALIDARG,"requested output exceeds physical capacity");
    Hr(kernel.Prepare(arena,{},f,0,prepared),"prepare zero input and capacity");
    D3D12_QUERY_HEAP_DESC query{};query.Type=D3D12_QUERY_HEAP_TYPE_TIMESTAMP;query.Count=2;
    Hr(context.device->CreateQueryHeap(&query,IID_PPV_ARGS(queries.GetAddressOf())),"contract query heap");
    Check(kernel.Record(*commands.Get(),states,prepared,output,{queries.Get(),2})==E_INVALIDARG&&states.PendingTransitions()==0,"insufficient query capacity rejected before recording");
    Hr(commands->Close(),"close unsubmitted contract list");
}
}
int wmain(int argc,wchar_t** argv)
{
    try
    {
        Check(argc==2,"supply GPU visibility evidence directory");const auto output=fs::absolute(argv[1]);fs::create_directories(output);
        Localization text;std::string error;Check(text.Load(GetExecutableDirectory()/"Resources/Localization/en-US.txt",error),"visibility localization");
        const auto shader=GetExecutableDirectory()/"Experiments/Visibility.hlsl";Contracts(text,shader);
        const auto f=CubeFrustum();GpuVisibilityExperiment experiment(text,shader,true);
        struct Expected {std::string name;std::vector<UINT> ids;UINT capacity;};std::map<UINT64,Expected> expected;
        const auto submit=[&](std::string name,const std::vector<VisibilityItem>& items,const VisibilityFrustum& frustum,UINT capacity)
        {
            std::vector<UINT> reference,oracle;Check(CullVisibilityReference(items,frustum,reference),"CPU reference accepted");
            for(const auto& item:items)if(Oracle(item,frustum))oracle.push_back(item.id);
            std::sort(reference.begin(),reference.end());std::sort(oracle.begin(),oracle.end());
            Check(std::includes(reference.begin(),reference.end(),oracle.begin(),oracle.end()),"CPU conservative against independent double corner oracle");
            const auto sequence=experiment.Submit(items,frustum,capacity);expected.emplace(sequence,Expected{std::move(name),std::move(reference),capacity});
        };
        {
            Gate gate(experiment);submit("two-in-flight-a",Layout(65,2),f,65);submit("two-in-flight-b",Layout(63,1),f,63);
            const auto pending=experiment.Statistics();Check(pending.pending==2&&pending.resultReadbacks==0&&!gate.timedOut,"two submissions accepted before gate completion or readback");
            gate.Release();
        }
        for(UINT count:{0u,1u,63u,64u,65u,1024u,65536u})for(UINT rate:{0u,1u,2u})
        {
            const auto items=Layout(count,rate);
            for(UINT capacity:{0u,1u,count/2,count})submit(std::to_string(count)+"-"+std::to_string(rate)+"-"+std::to_string(capacity),items,f,capacity);
        }
        const auto inf=std::numeric_limits<float>::infinity(),nan=std::numeric_limits<float>::quiet_NaN(),maximum=std::numeric_limits<float>::max(),tiny=std::numeric_limits<float>::denorm_min();
        std::vector<VisibilityItem> boundaries{
            {{1,-.1f,-.1f},1,{2,.1f,.1f},0},{{-2,-.1f,-.1f},2,{-1,.1f,.1f},0},
            {{0,0,0},3,{0,0,0},0},{{2,2,2},4,{1,1,1},0},{{nan,0,0},5,{0,0,0},0},
            {{-inf,0,0},6,{inf,0,0},0},{{-maximum,-maximum,-maximum},7,{maximum,maximum,maximum},0},
            {{-tiny,-tiny,-tiny},8,{tiny,tiny,tiny},0}};
        for(UINT i=0;i<64;++i)
        {
            const float x=1+1e-5f+(float(i)-32)*std::numeric_limits<float>::epsilon();
            boundaries.push_back({{x,-.01f,-.01f},100+i,{x+.1f,.01f,.01f},0});
        }
        submit("invalid-degenerate-touching-float-boundaries",boundaries,f,static_cast<UINT>(boundaries.size()));
        auto huge=f;for(auto& p:huge.planes)p={maximum,maximum,maximum,maximum};
        submit("finite-input-arithmetic-overflow",boundaries,huge,static_cast<UINT>(boundaries.size()));
        auto duplicate=Layout(1024,1);for(auto& item:duplicate)item.id%=17;
        submit("duplicate-ids-multiset",duplicate,f,1024);submit("duplicate-ids-overflow",duplicate,f,7);
        std::mt19937 random(0x601a);std::uniform_real_distribution<float> coordinate(-200,200),extent(0,.8f);
        std::vector<VisibilityItem> randomItems;
        for(UINT i=0;i<8192;++i){Vec3 p{coordinate(random),coordinate(random),coordinate(random)},e{extent(random),extent(random),extent(random)};randomItems.push_back({p-e,i,p+e,0});}
        VisibilityFrustum camera;const auto real=BuildFrustum(*PerspectiveRH(Pi/3,16.0f/9,.1f,100));Check(real&&QuantizeVisibilityFrustum(*real,camera),"camera planes");
        submit("random-camera-frustum",randomItems,camera,8192);
        submit("reset-all",Layout(65536,2),f,65536);submit("reset-empty",{},f,0);submit("reset-none",Layout(65536,0),f,65536);
        experiment.Drain();auto measurements=experiment.TakeResults();Check(measurements.size()==expected.size(),"every source submission completed");
        nlohmann::json cases=nlohmann::json::array();
        for(auto& m:measurements)
        {
            const auto& e=expected.at(m.sequence);std::sort(m.ids.begin(),m.ids.end());
            Check(m.total==e.ids.size()&&m.written==std::min<std::size_t>(e.ids.size(),e.capacity)&&m.overflow==UINT(e.ids.size()>e.capacity)&&m.guards,"GPU counters and guards match CPU expectation");
            Check(std::includes(e.ids.begin(),e.ids.end(),m.ids.begin(),m.ids.end()),"bounded GPU multiset is a subset including duplicate counts");
            if(e.capacity>=e.ids.size())Check(m.ids==e.ids,"full CPU/GPU visible multiset equality");
            cases.push_back({{"name",e.name},{"sequence",m.sequence},{"input",m.inputCount},{"visible",m.total},{"written",m.written},{"capacity",m.capacity},{"overflow",m.overflow},{"guards",m.guards},{"frequency_hz",m.frequency},{"ticks",m.ticks},{"gpu_setup_reset_ms",m.setupResetMs},{"gpu_dependency_cull_ms",m.dependencyCullMs},{"gpu_readback_copy_ms",m.readbackCopyMs},{"cpu_result_read_ms",m.readMs}});
        }
        const auto before=experiment.Statistics();Check(before.warnings==0&&before.errors==0&&before.pending==0&&before.resultReadbacks==expected.size(),"validation GPU diagnostics and readback count");
#ifndef NDEBUG
        Check(before.debugInfoQueue,"Debug InfoQueue available");
#endif
        experiment.Shutdown();const auto after=experiment.Statistics();
        Check(after.states.resources==0&&after.memory.used==0&&after.memory.reserved==0&&after.uploadReservedBytes==0&&after.readbackAllocationBytes==0,"completed experiment returns all registered resources and allocation leases");
        GpuVisibilityExperiment normal(text,shader,false,1024);
        for(unsigned i=0;i<20;++i)normal.Submit(Layout(1024,i%3),f,1024);
        normal.Drain();const auto ordinary=normal.TakeResults();const auto stats=normal.Statistics();
        Check(ordinary.size()==20&&stats.resultReadbacks==0&&stats.resultReadbackResources==0&&stats.warnings==0&&stats.errors==0,"normal path creates no visibility-count readback");
        for(const auto& m:ordinary)Check(!m.diagnostic&&m.ids.empty()&&m.frequency>0,"normal timing only");
        normal.Shutdown();Check(normal.Statistics().states.resources==0,"normal slots released after completion");
        std::ofstream file(output/"verification.json",std::ios::binary);
        file<<nlohmann::json{{"success",true},{"cases",cases},{"adapter",before.adapter},{"two_in_flight_gate_passed",true},{"debug_info_queue",before.debugInfoQueue},{"warnings",before.warnings},{"errors",before.errors},{"uav_barriers",before.states.uavBarriers},{"normal_submissions",20},{"normal_result_readbacks",stats.resultReadbacks},{"resources_after_shutdown",after.states.resources},{"memory_used_after_shutdown",after.memory.used}}.dump(2)<<'\n';file.close();Check(bool(file),"write GPU visibility verification");
        std::cout<<"GPU visibility: "<<cases.size()<<" cases, CPU/GPU multiset equality, guards, two slots and normal no-count-readback passed\n";return 0;
    }
    catch(const std::exception& error){std::cerr<<error.what()<<'\n';return 1;}
}
