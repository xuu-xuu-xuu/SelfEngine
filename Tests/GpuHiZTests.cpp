// G06.03：GPU Hi-Z 构建验证。逐级把 GPU 结果与 CPU HiZBuffer 参考逐位比较，
// 并覆盖契约、屏障/状态终态、时间戳、双输出同列表与资源回收。
#include "PlatformLayer/Graphics/D3D12/D3D12Context.h"
#include "PlatformLayer/Graphics/D3D12/D3D12HiZ.h"
#include "PlatformLayer/Graphics/D3D12/D3D12Memory.h"
#include "PlatformLayer/Graphics/D3D12/D3D12StateTracker.h"
#include "PlatformLayer/Graphics/D3D12/D3D12UploadArena.h"
#include "PlatformLayer/Windows/ExecutablePath.h"
#include "FunctionLayer/Rendering/HiZ.h"
#include "CoreLayer/Localization.h"
#include <nlohmann/json.hpp>
#include <wrl/wrappers/corewrappers.h>
#include <algorithm>
#include <array>
#include <cstring>
#include <fstream>
#include <iostream>
#include <random>
#include <stdexcept>
#include <utility>
#include <vector>

using namespace se;
using Microsoft::WRL::ComPtr;
namespace fs=std::filesystem;
namespace
{
void Check(bool value,const char* message){if(!value)throw std::runtime_error(message);}
void Hr(HRESULT result,const char* message){Check(SUCCEEDED(result),message);}
UINT RowPitch(UINT width){return (width*4+255)&~255u;}
UINT64 Align512(UINT64 value){return (value+511)&~UINT64{511};}
std::vector<float> Pattern(UINT width,UINT height,UINT kind)
{
    std::vector<float> depth(std::size_t{width}*height);
    std::mt19937 random(0x601au+height*131+kind);
    std::uniform_real_distribution<float> uniform(0,1);
    for(UINT y=0;y<height;++y)for(UINT x=0;x<width;++x)
    {
        float value=0;
        switch(kind%5)
        {
        case 0:value=0.25f;break;
        case 1:value=float(y)/float(std::max(1u,height));break;
        case 2:value=uniform(random);break;
        case 3:value=(x/3+y/5)%2?0.9f:0.1f;break;
        case 4:value=float((x*7+y*13)%97)/96.0f;break;
        case 5:value=x<width/2?0.4995f:1.0f;break;
        default:value=(x<width/2&&!(x>=10&&x<14&&y>=10&&y<14))?0.4995f:1.0f;break;
        }
        depth[std::size_t{y}*width+x]=value;
    }
    return depth;
}
struct Fixture
{
    D3D12Context context;
    D3D12HiZ kernel;
    ComPtr<ID3D12CommandQueue> queue;
    ComPtr<ID3D12Fence> fence;
    ComPtr<ID3D12QueryHeap> queries;
    ComPtr<ID3D12CommandAllocator> allocator;
    ComPtr<ID3D12GraphicsCommandList> commands;
    Microsoft::WRL::Wrappers::Event event;
    UINT64 next=1,frequency=0;
    bool stopped=false;
    explicit Fixture(const Localization& text)
    {
        Check(InitializeD3D12Context(context,text),"HZB device");
        D3D12_COMMAND_QUEUE_DESC description{};description.Type=D3D12_COMMAND_LIST_TYPE_DIRECT;
        Hr(context.device->CreateCommandQueue(&description,IID_PPV_ARGS(queue.GetAddressOf())),"HZB queue");
        Hr(context.states->BindQueue(*queue.Get()),"HZB queue ownership");
        Hr(context.device->CreateFence(0,D3D12_FENCE_FLAG_NONE,IID_PPV_ARGS(fence.GetAddressOf())),"HZB fence");
        event.Attach(CreateEventW(nullptr,FALSE,FALSE,nullptr));Check(event.IsValid(),"HZB event");
        Hr(queue->GetTimestampFrequency(&frequency),"HZB frequency");Check(frequency>0,"HZB nonzero frequency");
        D3D12_QUERY_HEAP_DESC query{};query.Type=D3D12_QUERY_HEAP_TYPE_TIMESTAMP;query.Count=D3D12HiZ::queryCount;
        Hr(context.device->CreateQueryHeap(&query,IID_PPV_ARGS(queries.GetAddressOf())),"HZB query heap");
        Hr(context.device->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT,IID_PPV_ARGS(allocator.GetAddressOf())),"HZB allocator");
        Hr(context.device->CreateCommandList(0,D3D12_COMMAND_LIST_TYPE_DIRECT,allocator.Get(),nullptr,
            IID_PPV_ARGS(commands.GetAddressOf())),"HZB commands");
        Hr(commands->Close(),"HZB initial close");
    }
    ~Fixture(){if(!stopped){try{Wait();}catch(...){std::terminate();}}}
    void Readback(UINT64 bytes,ComPtr<ID3D12Resource>& output,const char* name)
    {
        D3D12_RESOURCE_DESC description{};description.Dimension=D3D12_RESOURCE_DIMENSION_BUFFER;description.Width=bytes;
        description.Height=description.DepthOrArraySize=description.MipLevels=1;description.SampleDesc={1,0};
        description.Layout=D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
        D3D12_HEAP_PROPERTIES heap{};heap.Type=D3D12_HEAP_TYPE_READBACK;heap.CreationNodeMask=heap.VisibleNodeMask=1;
        Hr(context.device->CreateCommittedResource(&heap,D3D12_HEAP_FLAG_NONE,&description,D3D12_RESOURCE_STATE_COPY_DEST,
            nullptr,IID_PPV_ARGS(output.GetAddressOf())),name);
        Hr(context.states->Register(*output.Get(),D3D12_RESOURCE_STATE_COPY_DEST,D3D12TrackedHeap::Readback,name),name);
    }
    ComPtr<ID3D12Resource> Depth(UINT width,UINT height)
    {
        D3D12_RESOURCE_DESC description{};
        description.Dimension=D3D12_RESOURCE_DIMENSION_TEXTURE2D;description.Width=width;description.Height=height;
        description.DepthOrArraySize=1;description.MipLevels=1;description.Format=DXGI_FORMAT_R32_FLOAT;
        description.SampleDesc={1,0};description.Layout=D3D12_TEXTURE_LAYOUT_UNKNOWN;
        ComPtr<ID3D12Resource> resource;
        Hr(context.memory->Create(description,D3D12_RESOURCE_STATE_COPY_DEST,nullptr,resource,"HiZTest.Depth"),"HZB depth allocation");
        return resource;
    }
    void Wait()
    {
        const auto target=fenceValue;if(!target)return;
        const auto completed=fence->GetCompletedValue();Check(completed!=UINT64_MAX,"HZB device removed");
        if(completed>=target)return;
        Hr(fence->SetEventOnCompletion(target,event.Get()),"HZB completion event");
        Check(WaitForSingleObject(event.Get(),20000)==WAIT_OBJECT_0,"HZB completion timeout");
        const auto after=fence->GetCompletedValue();Check(after!=UINT64_MAX&&after>=target,"HZB completion verified");
    }
    UINT64 fenceValue=0;
    void Submit(D3D12StateRecording& states)
    {
        Hr(commands->Close(),"HZB close list");
        ID3D12CommandList* lists[]{commands.Get()};
        queue->ExecuteCommandLists(1,lists);states.Commit();fenceValue=next;
        Hr(queue->Signal(fence.Get(),next++),"HZB submit signal");Wait();
    }
    void Begin()
    {
        Wait();Hr(allocator->Reset(),"HZB allocator reset");Hr(commands->Reset(allocator.Get(),nullptr),"HZB list reset");
    }
    void Shutdown()
    {
        Wait();commands.Reset();allocator.Reset();queries.Reset();kernel={};
        context.states->UnbindQueue(queue.Get());queue.Reset();fence.Reset();event.Close();
        context.states->Collect();context.memory->Trim();stopped=true;
        Check(context.states->Stats().resources==0&&context.memory->Stats().used==0
            &&context.memory->Stats().reserved==0,"all HZB allocations released");
    }
};
struct Case
{
    std::string name;UINT width=0,height=0;std::vector<float> depth;
};
void DumpMessages(D3D12Context& context)
{
    ComPtr<ID3D12InfoQueue> queue;if(FAILED(context.device.As(&queue)))return;
    const auto count=queue->GetNumStoredMessages();
    for(UINT64 index=0;index<count;++index)
    {
        SIZE_T size=0;if(FAILED(queue->GetMessage(index,nullptr,&size)))continue;
        std::vector<std::byte> bytes(size);auto* message=reinterpret_cast<D3D12_MESSAGE*>(bytes.data());
        if(FAILED(queue->GetMessage(index,message,&size)))continue;
        if(message->Severity<=D3D12_MESSAGE_SEVERITY_WARNING)
            std::cerr<<"D3D12["<<int(message->Severity)<<"] "<<message->pDescription<<'\n';
    }
}
void Verify(Fixture& fixture,const Case& testCase,nlohmann::json& cases)
{
    HiZBuffer cpu;Check(cpu.Build(testCase.width,testCase.height,testCase.depth),"CPU HZB build");
    auto depth=fixture.Depth(testCase.width,testCase.height);
    D3D12HiZOutput output;
    Hr(fixture.kernel.CreateOutput(*depth.Get(),testCase.width,testCase.height,output),"HZB output allocation");
    Check(output.mipCount==cpu.MipCount()&&output.mipCount==D3D12HiZ::Levels(testCase.width,testCase.height),"HZB mip chain matches CPU chain");
    Check(output.width==testCase.width&&output.height==testCase.height&&output.texture&&output.descriptors,"HZB output description");
    D3D12UploadArena uploads;uploads.Initialize(*fixture.context.device.Get(),fixture.context.states);
    const UINT pitch=RowPitch(testCase.width);
    D3D12UploadSlice slice;Hr(uploads.Allocate(UINT64{pitch}*testCase.height,512,slice),"HZB depth upload");
    for(UINT y=0;y<testCase.height;++y)
        std::memcpy(slice.cpu+UINT64{y}*pitch,testCase.depth.data()+std::size_t{y}*testCase.width,std::size_t{testCase.width}*4);
    std::vector<UINT64> offsets(cpu.MipCount());std::vector<UINT> pitches(cpu.MipCount());
    UINT64 total=0;
    for(UINT mip=0;mip<cpu.MipCount();++mip)
    {
        pitches[mip]=RowPitch(cpu.Mip(mip).width);offsets[mip]=Align512(total);
        total=offsets[mip]+UINT64{pitches[mip]}*cpu.Mip(mip).height;
    }
    ComPtr<ID3D12Resource> readback;fixture.Readback(total,readback,"HiZTest.Readback");
    fixture.Begin();
    D3D12StateRecording states(*fixture.context.states);
    const auto barriersBefore=fixture.context.states->Stats().uavBarriers;
    Hr(states.Use(*fixture.commands.Get(),*depth.Get(),D3D12_RESOURCE_STATE_COPY_DEST,"HiZTest.Upload"),"HZB upload state");
    D3D12_TEXTURE_COPY_LOCATION source{};
    source.pResource=slice.resource;source.Type=D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
    source.PlacedFootprint.Offset=slice.offset;
    source.PlacedFootprint.Footprint={DXGI_FORMAT_R32_FLOAT,testCase.width,testCase.height,1,pitch};
    D3D12_TEXTURE_COPY_LOCATION destination{};
    destination.pResource=depth.Get();destination.Type=D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
    fixture.commands->CopyTextureRegion(&destination,0,0,0,&source,nullptr);
    Hr(fixture.kernel.Record(*fixture.commands.Get(),states,output),"HZB record");
    Hr(states.Use(*fixture.commands.Get(),*output.texture.Get(),D3D12_RESOURCE_STATE_COPY_SOURCE,"HiZTest.Readback"),"HZB readback state");
    Hr(states.Use(*fixture.commands.Get(),*readback.Get(),D3D12_RESOURCE_STATE_COPY_DEST,"HiZTest.Readback"),"HZB readback destination");
    for(UINT mip=0;mip<output.mipCount;++mip)
    {
        D3D12_TEXTURE_COPY_LOCATION target{};
        target.pResource=readback.Get();target.Type=D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
        target.PlacedFootprint.Offset=offsets[mip];
        target.PlacedFootprint.Footprint={DXGI_FORMAT_R32_FLOAT,cpu.Mip(mip).width,cpu.Mip(mip).height,1,pitches[mip]};
        D3D12_TEXTURE_COPY_LOCATION level{};
        level.pResource=output.texture.Get();level.Type=D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;level.SubresourceIndex=mip;
        fixture.commands->CopyTextureRegion(&target,0,0,0,&level,nullptr);
    }
    fixture.Submit(states);
    const auto barrierDelta=fixture.context.states->Stats().uavBarriers-barriersBefore;
    if(barrierDelta!=output.mipCount)std::cerr<<testCase.name<<" barriers "<<barrierDelta<<" expected "<<output.mipCount<<'\n';
    Check(barrierDelta==output.mipCount,"one UAV barrier per HZB level");
    const auto record=fixture.context.states->Find(*output.texture.Get());
    Check(record&&record->states.size()==output.mipCount,"HZB state record covers every level");
    for(const auto state:record->states)Check(state==D3D12_RESOURCE_STATE_COPY_SOURCE,"HZB state follows the readback copy");
    void* mapped=nullptr;D3D12_RANGE range{0,static_cast<SIZE_T>(total)},none{0,0};
    Hr(readback->Map(0,&range,&mapped),"HZB readback map");
    for(UINT mip=0;mip<cpu.MipCount();++mip)
    {
        const auto& reference=cpu.Mip(mip);
        for(UINT y=0;y<reference.height;++y)
        {
            const auto* gpu=static_cast<const std::byte*>(mapped)+offsets[mip]+UINT64{y}*pitches[mip];
            const auto* expected=reinterpret_cast<const std::byte*>(reference.depth.data()+std::size_t{y}*reference.width);
            if(std::memcmp(gpu,expected,std::size_t{reference.width}*4)!=0)
            {
                for(UINT x=0;x<reference.width;++x)
                {
                    float actual=0;std::memcpy(&actual,gpu+UINT64{x}*4,4);
                    const float wanted=reference.depth[std::size_t{y}*reference.width+x];
                    if(std::memcmp(&actual,&wanted,4)!=0)
                    {
                        std::cerr<<testCase.name<<" mip "<<mip<<" ("<<x<<","<<y<<") GPU "<<actual<<" CPU "<<wanted<<'\n';
                        DumpMessages(fixture.context);
                        for(UINT row=0;row<reference.height;++row)
                        {
                            std::cerr<<"gpu row "<<row<<":";
                            for(UINT column=0;column<reference.width;++column)
                            {
                                float value=0;std::memcpy(&value,gpu+UINT64{row}*pitches[mip]+UINT64{column}*4,4);
                                std::cerr<<' '<<value;
                            }
                            std::cerr<<'\n';
                        }
                        throw std::runtime_error("GPU HZB level is bit-identical to CPU reference");
                    }
                }
            }
        }
    }
    readback->Unmap(0,&none);
    cases.push_back({{"name",testCase.name},{"width",testCase.width},{"height",testCase.height},
        {"mips",output.mipCount},{"levels",output.mipCount},{"uav_barriers",barrierDelta},{"bit_exact",true}});
}
void Contracts(Fixture& fixture,const Localization& text)
{
    D3D12HiZOutput output;
    D3D12HiZ uninitialized;
    auto depth=fixture.Depth(8,8);
    fixture.Begin();
    Check(uninitialized.CreateOutput(*depth.Get(),8,8,output)==E_UNEXPECTED,"uninitialized HZB kernel rejected");
    Hr(fixture.kernel.Initialize(fixture.context,GetExecutableDirectory()/"Experiments/HiZ.hlsl",text),"HZB kernel");
    Check(fixture.kernel.Initialize(fixture.context,GetExecutableDirectory()/"Experiments/HiZ.hlsl",text)==E_UNEXPECTED,"HZB reinitialization rejected");
    Check(D3D12HiZ::Levels(0,8)==0&&D3D12HiZ::Levels(8,0)==0&&D3D12HiZ::Levels(8193,1)==0,"invalid HZB levels");
    Check(D3D12HiZ::Levels(1,1)==1&&D3D12HiZ::Levels(1920,1080)==11,"HZB level count");
    Check(fixture.kernel.CreateOutput(*depth.Get(),4,4,output)==E_INVALIDARG,"mismatched depth size rejected");
    Check(!output.texture,"failed HZB creation leaves output empty");
    Hr(fixture.kernel.CreateOutput(*depth.Get(),8,8,output),"valid HZB output");
    const auto* old=output.texture.Get();
    Check(fixture.kernel.CreateOutput(*depth.Get(),4,4,output)==E_INVALIDARG&&output.texture.Get()==old,"failed replacement preserves output");
    D3D12StateRecording states(*fixture.context.states);
    Check(fixture.kernel.Record(*fixture.commands.Get(),states,{})==E_INVALIDARG&&states.PendingTransitions()==0,"empty HZB record rejected before transitions");
    Check(fixture.kernel.Record(*fixture.commands.Get(),states,output,{fixture.queries.Get(),2})==E_INVALIDARG
        &&states.PendingTransitions()==0,"insufficient HZB timestamp capacity rejected");
    Hr(fixture.kernel.Record(*fixture.commands.Get(),states,output,{fixture.queries.Get(),D3D12HiZ::queryCount}),"valid HZB record");
    Check(states.PendingTransitions()>0&&states.PendingUavBarriers()>0,"valid HZB record emits transitions and UAV barriers");
    fixture.Submit(states);
    const auto built=fixture.context.states->Find(*output.texture.Get());
    Check(built&&built->states.size()==output.mipCount,"HZB committed state record");
    for(const auto state:built->states)Check(state==D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE,"HZB ends in shader-resource state");
}
void TimestampsAndTwoOutputs(Fixture& fixture,nlohmann::json& cases)
{
    auto depth=fixture.Depth(64,48);
    D3D12HiZOutput first,second;
    Hr(fixture.kernel.CreateOutput(*depth.Get(),64,48,first),"first HZB output");
    Hr(fixture.kernel.CreateOutput(*depth.Get(),64,48,second),"second HZB output");
    D3D12UploadArena uploads;uploads.Initialize(*fixture.context.device.Get(),fixture.context.states);
    const UINT pitch=RowPitch(64);D3D12UploadSlice slice;
    Hr(uploads.Allocate(UINT64{pitch}*48,512,slice),"two-output depth upload");
    for(UINT y=0;y<48;++y)for(UINT x=0;x<64;++x)
        reinterpret_cast<float*>(slice.cpu+UINT64{y}*pitch)[x]=float(x+y)/112.0f;
    const UINT64 readbackBytes=Align512(UINT64{pitch}*48);
    ComPtr<ID3D12Resource> images,timings;
    fixture.Readback(readbackBytes*2,images,"HiZTest.TwoImages");
    fixture.Readback(24,timings,"HiZTest.Timestamps");
    fixture.Begin();
    D3D12StateRecording states(*fixture.context.states);
    Hr(states.Use(*fixture.commands.Get(),*depth.Get(),D3D12_RESOURCE_STATE_COPY_DEST,"HiZTest.Upload"),"two-output upload state");
    D3D12_TEXTURE_COPY_LOCATION source{};
    source.pResource=slice.resource;source.Type=D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
    source.PlacedFootprint.Offset=slice.offset;
    source.PlacedFootprint.Footprint={DXGI_FORMAT_R32_FLOAT,64,48,1,pitch};
    D3D12_TEXTURE_COPY_LOCATION destination{};
    destination.pResource=depth.Get();destination.Type=D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
    fixture.commands->CopyTextureRegion(&destination,0,0,0,&source,nullptr);
    Hr(fixture.kernel.Record(*fixture.commands.Get(),states,first,{fixture.queries.Get(),D3D12HiZ::queryCount}),"first timed HZB record");
    Hr(fixture.kernel.Record(*fixture.commands.Get(),states,second),"second HZB record");
    Hr(states.Use(*fixture.commands.Get(),*timings.Get(),D3D12_RESOURCE_STATE_COPY_DEST,"HiZTest.Timestamps"),"timestamp resolve state");
    fixture.commands->ResolveQueryData(fixture.queries.Get(),D3D12_QUERY_TYPE_TIMESTAMP,0,D3D12HiZ::queryCount,timings.Get(),0);
    Hr(states.Use(*fixture.commands.Get(),*first.texture.Get(),D3D12_RESOURCE_STATE_COPY_SOURCE,"HiZTest.TwoImages"),"first image state");
    Hr(states.Use(*fixture.commands.Get(),*second.texture.Get(),D3D12_RESOURCE_STATE_COPY_SOURCE,"HiZTest.TwoImages"),"second image state");
    Hr(states.Use(*fixture.commands.Get(),*images.Get(),D3D12_RESOURCE_STATE_COPY_DEST,"HiZTest.TwoImages"),"image destination state");
    for(UINT index=0;index<2;++index)
    {
        D3D12_TEXTURE_COPY_LOCATION target{};
        target.pResource=images.Get();target.Type=D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
        target.PlacedFootprint.Offset=readbackBytes*index;
        target.PlacedFootprint.Footprint={DXGI_FORMAT_R32_FLOAT,64,48,1,pitch};
        D3D12_TEXTURE_COPY_LOCATION level{};
        level.pResource=(index?second:first).texture.Get();level.Type=D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
        fixture.commands->CopyTextureRegion(&target,0,0,0,&level,nullptr);
    }
    fixture.Submit(states);
    void* mapped=nullptr;D3D12_RANGE range{0,static_cast<SIZE_T>(readbackBytes*2)},none{0,0};
    Hr(images->Map(0,&range,&mapped),"two image map");
    Check(std::memcmp(mapped,static_cast<const std::byte*>(mapped)+readbackBytes,static_cast<std::size_t>(readbackBytes))==0,
        "two HZB outputs from one depth are identical");
    images->Unmap(0,&none);
    D3D12_RANGE ticksRange{0,24};void* tickMapped=nullptr;
    Hr(timings->Map(0,&ticksRange,&tickMapped),"timestamp map");
    std::array<UINT64,3> ticks{};std::memcpy(ticks.data(),tickMapped,24);timings->Unmap(0,&none);
    Check(ticks[0]>0&&std::is_sorted(ticks.begin(),ticks.end())&&ticks[0]<=ticks[1]&&ticks[1]<=ticks[2],"HZB timestamps ordered");
    cases.push_back({{"name","two-outputs-timestamps"},{"mips",first.mipCount},{"ticks",ticks},{"frequency_hz",fixture.frequency},{"bit_exact",true}});
}
}
int wmain(int argc,wchar_t** argv)
{
    try
    {
        Check(argc==2,"supply GPU HZB evidence directory");
        const auto output=fs::absolute(argv[1]);fs::create_directories(output);
        Localization text;std::string error;
        Check(text.Load(GetExecutableDirectory()/"Resources/Localization/en-US.txt",error),"HZB localization");
        Fixture fixture(text);
        Contracts(fixture,text);
        nlohmann::json cases=nlohmann::json::array();
        const std::array<std::pair<UINT,UINT>,8> sizes{{{1,1},{2,1},{3,3},{37,23},{64,64},{256,144},{1023,577},{1920,1080}}};
        for(const auto& [width,height]:sizes)
        {
            for(UINT pattern=0;pattern<2;++pattern)
            {
                Case testCase{std::to_string(width)+"x"+std::to_string(height)+"-p"+std::to_string(pattern),
                    width,height,Pattern(width,height,pattern)};
                Verify(fixture,testCase,cases);
            }
        }
        TimestampsAndTwoOutputs(fixture,cases);
        Verify(fixture,Case{"wall-64",64,64,Pattern(64,64,5)},cases);
        Verify(fixture,Case{"wall-hole-64",64,64,Pattern(64,64,6)},cases);
        const auto statistics=fixture.context.states->Stats();
        const auto memory=fixture.context.memory->Stats();
        nlohmann::json report={{"success",true},{"cases",cases},
            {"frequency_hz",fixture.frequency},{"uav_barriers",statistics.uavBarriers},
            {"transitions",statistics.transitions},{"resources_after_cases",statistics.resources},
            {"memory_used_after_cases",memory.used}};
        fixture.Shutdown();report["resources_after_shutdown"]=fixture.context.states->Stats().resources;
        report["memory_used_after_shutdown"]=fixture.context.memory->Stats().used;
        std::ofstream file(output/"verification.json",std::ios::binary);
        file<<report.dump(2)<<'\n';file.close();Check(bool(file),"write HZB verification");
        std::cout<<"GPU HZB: "<<cases.size()<<" cases, per-level CPU bit equality, timestamps, two outputs and release passed\n";
        return 0;
    }
    catch(const std::exception& exception){std::cerr<<exception.what()<<'\n';return 1;}
}
