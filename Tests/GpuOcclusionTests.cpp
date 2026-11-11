// G06.03：GPU 遮挡剔除验证。用真实 GPU HZB 与 D3D12Indirect 的遮挡谓词，
// 与 CPU HiZBuffer/TestOcclusion 参考在同一深度、同一投影下比较可见集合。
#include "PlatformLayer/Graphics/D3D12/D3D12Context.h"
#include "PlatformLayer/Graphics/D3D12/D3D12HiZ.h"
#include "PlatformLayer/Graphics/D3D12/D3D12Indirect.h"
#include "PlatformLayer/Graphics/D3D12/D3D12Memory.h"
#include "PlatformLayer/Graphics/D3D12/D3D12StateTracker.h"
#include "PlatformLayer/Graphics/D3D12/D3D12UploadArena.h"
#include "PlatformLayer/Windows/ExecutablePath.h"
#include "FunctionLayer/Rendering/HiZ.h"
#include "CoreLayer/Localization.h"
#include "CoreLayer/Math/Projection.h"
#include <nlohmann/json.hpp>
#include <wrl/wrappers/corewrappers.h>
#include <algorithm>
#include <array>
#include <cstring>
#include <fstream>
#include <iostream>
#include <limits>
#include <random>
#include <stdexcept>
#include <vector>

using namespace se;
using Microsoft::WRL::ComPtr;
namespace fs=std::filesystem;
namespace
{
constexpr UINT kSize=64,kIndexCount=36,kCapacity=1024;
void Check(bool value,const char* message){if(!value)throw std::runtime_error(message);}
void Hr(HRESULT result,const char* message){Check(SUCCEEDED(result),message);}
UINT RowPitch(UINT width){return (width*4+255)&~255u;}
struct Fixture
{
    D3D12Context context;D3D12HiZ hiz;D3D12Indirect indirect;
    ComPtr<ID3D12CommandQueue> queue;ComPtr<ID3D12Fence> fence;
    ComPtr<ID3D12CommandAllocator> allocator;ComPtr<ID3D12GraphicsCommandList> commands;
    Microsoft::WRL::Wrappers::Event event;ComPtr<ID3D12Resource> depth,readback;
    D3D12HiZOutput hzb;D3D12IndirectOutput output;D3D12UploadArena uploads;
    UINT64 next=1,fenceValue=0;bool stopped=false;
    explicit Fixture(const Localization& text,const fs::path& shaderRoot)
    {
        Check(InitializeD3D12Context(context,text),"occlusion device");
        D3D12_COMMAND_QUEUE_DESC description{};description.Type=D3D12_COMMAND_LIST_TYPE_DIRECT;
        Hr(context.device->CreateCommandQueue(&description,IID_PPV_ARGS(queue.GetAddressOf())),"occlusion queue");
        Hr(context.states->BindQueue(*queue.Get()),"occlusion queue ownership");
        Hr(context.device->CreateFence(0,D3D12_FENCE_FLAG_NONE,IID_PPV_ARGS(fence.GetAddressOf())),"occlusion fence");
        event.Attach(CreateEventW(nullptr,FALSE,FALSE,nullptr));Check(event.IsValid(),"occlusion event");
        Hr(context.device->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT,IID_PPV_ARGS(allocator.GetAddressOf())),"occlusion allocator");
        Hr(context.device->CreateCommandList(0,D3D12_COMMAND_LIST_TYPE_DIRECT,allocator.Get(),nullptr,IID_PPV_ARGS(commands.GetAddressOf())),"occlusion commands");
        Hr(commands->Close(),"occlusion initial close");
        Hr(hiz.Initialize(context,shaderRoot/"HiZ.hlsl",text),"occlusion HiZ kernel");
        Hr(indirect.Initialize(context,shaderRoot/"Indirect.hlsl",text),"occlusion indirect kernel");
        D3D12_RESOURCE_DESC depthDescription{};
        depthDescription.Dimension=D3D12_RESOURCE_DIMENSION_TEXTURE2D;depthDescription.Width=kSize;depthDescription.Height=kSize;
        depthDescription.DepthOrArraySize=1;depthDescription.MipLevels=1;depthDescription.Format=DXGI_FORMAT_R32_FLOAT;
        depthDescription.SampleDesc={1,0};depthDescription.Layout=D3D12_TEXTURE_LAYOUT_UNKNOWN;
        Hr(context.memory->Create(depthDescription,D3D12_RESOURCE_STATE_COPY_DEST,nullptr,depth,"Occlusion.Depth"),"occlusion depth");
        Hr(hiz.CreateOutput(*depth.Get(),kSize,kSize,hzb),"occlusion HZB output");
        Hr(indirect.CreateOutput(kCapacity,kCapacity,output),"occlusion indirect output");
        uploads.Initialize(*context.device.Get(),context.states);
        const UINT64 bytes=48+UINT64{kCapacity}*rhi::InstanceGpuStride+8;
        D3D12_RESOURCE_DESC readbackDescription{};
        readbackDescription.Dimension=D3D12_RESOURCE_DIMENSION_BUFFER;readbackDescription.Width=bytes;
        readbackDescription.Height=readbackDescription.DepthOrArraySize=readbackDescription.MipLevels=1;
        readbackDescription.SampleDesc={1,0};readbackDescription.Layout=D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
        D3D12_HEAP_PROPERTIES heap{};heap.Type=D3D12_HEAP_TYPE_READBACK;heap.CreationNodeMask=heap.VisibleNodeMask=1;
        Hr(context.device->CreateCommittedResource(&heap,D3D12_HEAP_FLAG_NONE,&readbackDescription,D3D12_RESOURCE_STATE_COPY_DEST,
            nullptr,IID_PPV_ARGS(readback.GetAddressOf())),"occlusion readback");
        Hr(context.states->Register(*readback.Get(),D3D12_RESOURCE_STATE_COPY_DEST,D3D12TrackedHeap::Readback,"Occlusion.Readback"),"occlusion readback state");
    }
    ~Fixture(){if(!stopped){try{Wait();}catch(...){std::terminate();}}}
    void Wait()
    {
        if(!fenceValue)return;
        const auto completed=fence->GetCompletedValue();Check(completed!=UINT64_MAX,"occlusion device removed");
        if(completed<fenceValue)
        {
            Hr(fence->SetEventOnCompletion(fenceValue,event.Get()),"occlusion completion event");
            Check(WaitForSingleObject(event.Get(),20000)==WAIT_OBJECT_0,"occlusion completion timeout");
            Check(fence->GetCompletedValue()>=fenceValue,"occlusion completion verified");
        }
        fenceValue=0;
    }
    void Submit(D3D12StateRecording& states)
    {
        Hr(commands->Close(),"occlusion close list");
        ID3D12CommandList* lists[]{commands.Get()};
        queue->ExecuteCommandLists(1,lists);states.Commit();fenceValue=next;
        Hr(queue->Signal(fence.Get(),next++),"occlusion signal");Wait();
    }
    void Begin()
    {
        Wait();Hr(allocator->Reset(),"occlusion allocator reset");Hr(commands->Reset(allocator.Get(),nullptr),"occlusion list reset");
    }
    void Shutdown()
    {
        Wait();commands.Reset();allocator.Reset();hzb={};output={};depth.Reset();readback.Reset();uploads.Release();
        hiz={};indirect={};context.states->UnbindQueue(queue.Get());queue.Reset();fence.Reset();event.Close();
        context.states->Collect();context.memory->Trim();stopped=true;
        Check(context.states->Stats().resources==0&&context.memory->Stats().used==0&&context.memory->Stats().reserved==0,
            "occlusion allocations released");
    }
};
Mat4 ClipMatrix(){return *OrthographicRH(8,6,0.1f,100.0f);}
float DepthAt(const Mat4& clip,float z)
{
    const Vec4 value=clip*Vec4{0,0,z,1};return value.z/value.w;
}
struct Scene
{
    std::vector<VisibilityItem> items;
    std::vector<float> depth;
    VisibilityFrustum frustum;
    Mat4 clip;
};
VisibilityItem Box(UINT id,Vec3 center,Vec3 half)
{return VisibilityItem{center-half,id,center+half,0};}
VisibilityItem Box(UINT id,float x,float y,float z,float halfX,float halfY,float halfZ)
{return Box(id,Vec3{x,y,z},Vec3{halfX,halfY,halfZ});}
Scene BuildScene(float wallDepth,bool hole)
{
    Scene scene;scene.clip=ClipMatrix();
    const auto frustum=BuildFrustum(scene.clip);Check(bool(frustum),"occlusion frustum");
    Check(QuantizeVisibilityFrustum(*frustum,scene.frustum),"occlusion frustum quantization");
    scene.depth.assign(std::size_t{kSize}*kSize,1.0f);
    for(UINT y=0;y<kSize;++y)for(UINT x=0;x<kSize;++x)
        if(x<kSize/2)scene.depth[std::size_t{y}*kSize+x]=wallDepth;
    if(hole)for(UINT y=10;y<14;++y)for(UINT x=10;x<14;++x)scene.depth[std::size_t{y}*kSize+x]=1.0f;
    UINT id=0;
    for(UINT i=0;i<8;++i)scene.items.push_back(Box(id++,-3.4f+float(i)*0.35f,0,-80,0.12f,0.12f,0.05f));
    for(UINT i=0;i<4;++i)scene.items.push_back(Box(id++,-3.0f+float(i)*0.5f,0,-20,0.12f,0.12f,0.05f));
    for(UINT i=0;i<4;++i)scene.items.push_back(Box(id++,1.0f+float(i)*0.5f,0,-80,0.12f,0.12f,0.05f));
    scene.items.push_back(Box(id++,0,0,-80,0.2f,0.2f,0.05f));
    scene.items.push_back(Box(id++,-2.56f,1.92f,-80,0.08f,0.08f,0.05f));
    scene.items.push_back(Box(id++,-2.0f,-1.0f,-80,0.2f,0.2f,0.01f));
    scene.items.push_back(Box(id++,-1.0f,1.0f,-1.0f,0.1f,0.1f,0.02f));
    std::mt19937 random(0x6030u);std::uniform_real_distribution<float> x(-3.9f,3.9f),y(-2.9f,2.9f),z(-90,-2),half(0.02f,0.18f);
    for(UINT i=0;i<384;++i)
    {
        const float h=half(random);scene.items.push_back(Box(id++,x(random),y(random),z(random),h,h,0.05f));
    }
    return scene;
}
std::vector<UINT> CpuVisible(const Scene& scene,bool occlusion,const char* name)
{
    std::vector<UINT> frustum;Check(CullVisibilityReference(scene.items,scene.frustum,frustum),name);
    if(!occlusion)return frustum;
    HiZBuffer cpu;Check(cpu.Build(kSize,kSize,scene.depth),"CPU HZB build");
    std::vector<UINT> visible;
    for(const auto id:frustum)
    {
        const auto& item=scene.items[id];
        const auto test=TestOcclusion(cpu,Aabb{item.minimum,item.maximum},scene.clip,kSize,kSize);
        if(!test.occluded)visible.push_back(id);
    }
    return visible;
}
void UploadDepth(Fixture& fixture,const std::vector<float>& depth,D3D12UploadSlice& slice)
{
    const UINT pitch=RowPitch(kSize);
    Hr(fixture.uploads.Allocate(UINT64{pitch}*kSize,512,slice),"occlusion depth upload");
    for(UINT y=0;y<kSize;++y)std::memcpy(slice.cpu+UINT64{y}*pitch,depth.data()+std::size_t{y}*kSize,std::size_t{kSize}*4);
}
struct Measurement {UINT count=0,written=0;std::vector<UINT> ids;};
Measurement ReadResult(Fixture& fixture)
{
    void* mapped=nullptr;D3D12_RANGE range{0,static_cast<SIZE_T>(48+UINT64{kCapacity}*rhi::InstanceGpuStride+8)},none{0,0};
    Hr(fixture.readback->Map(0,&range,&mapped),"occlusion readback map");
    Measurement result;const auto* words=static_cast<const UINT*>(mapped);
    result.written=words[1];result.count=words[6];
    Check(result.written<=kCapacity&&words[0]==kIndexCount,"occlusion arguments shape");
    const auto* records=static_cast<const std::byte*>(mapped)+48;
    for(UINT index=0;index<result.written;++index)
    {
        UINT id=0;std::memcpy(&id,records+UINT64{index}*rhi::InstanceGpuStride,4);result.ids.push_back(id);
    }
    fixture.readback->Unmap(0,&none);return result;
}
void Run(Fixture& fixture,const Scene& scene,bool occlusion,nlohmann::json& cases,const char* name)
{
    fixture.Begin();fixture.uploads.Reset();
    D3D12UploadSlice depthSlice;
    UploadDepth(fixture,scene.depth,depthSlice);
    std::vector<UINT> words(scene.items.size()*60);
    for(UINT index=0;index<scene.items.size();++index)words[std::size_t{index}*60]=index;
    D3D12UploadSlice instances;
    if(!words.empty())
    {
        const UINT64 bytes=UINT64{words.size()}*4;
        Hr(fixture.uploads.Allocate(bytes,16,instances),"occlusion instance upload");
        std::memcpy(instances.cpu,words.data(),bytes);
    }
    else Hr(fixture.uploads.Allocate(rhi::InstanceGpuStride,16,instances),"occlusion empty instance upload");
    D3D12StateRecording states(*fixture.context.states);
    Hr(states.Use(*fixture.commands.Get(),*fixture.depth.Get(),D3D12_RESOURCE_STATE_COPY_DEST,"Occlusion.Upload"),"occlusion depth state");
    D3D12_TEXTURE_COPY_LOCATION source{};
    source.pResource=depthSlice.resource;source.Type=D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
    source.PlacedFootprint.Offset=depthSlice.offset;
    source.PlacedFootprint.Footprint={DXGI_FORMAT_R32_FLOAT,kSize,kSize,1,RowPitch(kSize)};
    D3D12_TEXTURE_COPY_LOCATION destination{};
    destination.pResource=fixture.depth.Get();destination.Type=D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
    fixture.commands->CopyTextureRegion(&destination,0,0,0,&source,nullptr);
    Hr(fixture.hiz.Record(*fixture.commands.Get(),states,fixture.hzb),"occlusion HZB record");
    D3D12IndirectOcclusion view{};
    view.heap=fixture.hzb.descriptors.Get();view.srv=fixture.hzb.hzbSrv;
    view.clipFromWorld=scene.clip;view.width=kSize;view.height=kSize;view.mipCount=fixture.hzb.mipCount;view.depthBias=1e-6f;
    D3D12IndirectPrepared prepared;
    const auto capacity=static_cast<UINT>(scene.items.size());
    Hr(fixture.indirect.Prepare(fixture.uploads,scene.items,scene.frustum,instances,kIndexCount,capacity,prepared,
        occlusion?&view:nullptr),"occlusion indirect prepare");
    Hr(fixture.indirect.Record(*fixture.commands.Get(),states,prepared,fixture.output,{},0,occlusion?&view:nullptr),"occlusion indirect record");
    Hr(states.Use(*fixture.commands.Get(),*fixture.output.arguments.Get(),D3D12_RESOURCE_STATE_INDIRECT_ARGUMENT,"Occlusion.Draw"),"occlusion argument state");
    Hr(states.Use(*fixture.commands.Get(),*fixture.output.instances.Get(),D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE,"Occlusion.Draw"),"occlusion instance state");
    for(auto* resource:{fixture.output.arguments.Get(),fixture.output.instances.Get()})
        Hr(states.Use(*fixture.commands.Get(),*resource,D3D12_RESOURCE_STATE_COPY_SOURCE,"Occlusion.Read"),"occlusion copy source");
    Hr(states.Use(*fixture.commands.Get(),*fixture.readback.Get(),D3D12_RESOURCE_STATE_COPY_DEST,"Occlusion.Read"),"occlusion copy destination");
    fixture.commands->CopyBufferRegion(fixture.readback.Get(),0,fixture.output.arguments.Get(),0,48);
    fixture.commands->CopyBufferRegion(fixture.readback.Get(),48,fixture.output.instances.Get(),0,UINT64{kCapacity}*rhi::InstanceGpuStride+8);
    fixture.Submit(states);
    const auto measurement=ReadResult(fixture);
    const auto expected=CpuVisible(scene,occlusion,name);
    std::vector<UINT> actual=measurement.ids;std::sort(actual.begin(),actual.end());
    auto sorted=expected;std::sort(sorted.begin(),sorted.end());
    if(measurement.count!=sorted.size())
        std::cerr<<name<<" GPU count "<<measurement.count<<" CPU visible "<<sorted.size()<<" written "<<measurement.written<<'\n';
    Check(measurement.count==sorted.size(),"GPU visible count matches CPU reference");
    if(occlusion&&std::string(name).find("designed")!=std::string::npos)
        Check(actual==sorted,"GPU occlusion matches CPU visible set");
    else
        Check(std::includes(actual.begin(),actual.end(),sorted.begin(),sorted.end()),"GPU visible set is conservative against CPU");
    cases.push_back({{"name",name},{"items",scene.items.size()},{"frustum_visible",CpuVisible(scene,false,name).size()},
        {"expected",sorted.size()},{"gpu_count",measurement.count},{"written",measurement.written},{"occlusion",occlusion}});
}
void Contracts(Fixture& fixture,const Scene& scene)
{
    fixture.Begin();fixture.uploads.Reset();
    D3D12UploadSlice depthSlice,instances;
    Hr(fixture.uploads.Allocate(UINT64{RowPitch(kSize)}*kSize,512,depthSlice),"occlusion contract depth upload");
    Hr(fixture.uploads.Allocate(UINT64{scene.items.size()}*rhi::InstanceGpuStride,16,instances),"occlusion contract instance upload");
    D3D12IndirectPrepared prepared;
    D3D12IndirectOcclusion view{};
    view.heap=fixture.hzb.descriptors.Get();view.srv=fixture.hzb.hzbSrv;view.clipFromWorld=scene.clip;
    view.width=kSize;view.height=kSize;view.mipCount=fixture.hzb.mipCount;
    auto invalid=view;invalid.width=0;
    Check(fixture.indirect.Prepare(fixture.uploads,scene.items,scene.frustum,instances,kIndexCount,
        static_cast<UINT>(scene.items.size()),prepared,&invalid)==E_INVALIDARG,
        "invalid occlusion view rejected before allocation");
    invalid=view;invalid.mipCount=0;
    Check(fixture.indirect.Prepare(fixture.uploads,scene.items,scene.frustum,instances,kIndexCount,
        static_cast<UINT>(scene.items.size()),prepared,&invalid)==E_INVALIDARG,
        "zero HZB mip count rejected");
    invalid=view;invalid.clipFromWorld.values[0]=std::numeric_limits<float>::quiet_NaN();
    Check(fixture.indirect.Prepare(fixture.uploads,scene.items,scene.frustum,instances,kIndexCount,
        static_cast<UINT>(scene.items.size()),prepared,&invalid)==E_INVALIDARG,
        "non-finite occlusion matrix rejected");
    Check(fixture.indirect.Prepare(fixture.uploads,scene.items,scene.frustum,instances,kIndexCount,
        static_cast<UINT>(scene.items.size())-1,prepared,&view)==E_INVALIDARG,
        "occlusion capacity below candidate count rejected without truncation");
    Hr(fixture.indirect.Prepare(fixture.uploads,scene.items,scene.frustum,instances,kIndexCount,
        static_cast<UINT>(scene.items.size()),prepared,&view),"occlusion contract prepare");
    D3D12StateRecording states(*fixture.context.states);
    Check(fixture.indirect.Record(*fixture.commands.Get(),states,prepared,fixture.output,{},0,nullptr)==E_INVALIDARG,
        "record rejects dropping occlusion after prepare");
    Hr(fixture.commands->Close(),"occlusion contract close");
    fixture.Wait();
}
}
int wmain(int argc,wchar_t** argv)
{
    try
    {
        Check(argc==2,"supply GPU occlusion evidence directory");
        const auto output=fs::absolute(argv[1]);fs::create_directories(output);
        Localization text;std::string error;
        Check(text.Load(GetExecutableDirectory()/"Resources/Localization/en-US.txt",error),"occlusion localization");
        Fixture fixture(text,GetExecutableDirectory()/"Experiments");
        const auto wallDepth=DepthAt(ClipMatrix(),-50);
        nlohmann::json cases=nlohmann::json::array();
        {
            const auto scene=BuildScene(wallDepth,false);
            Contracts(fixture,scene);
            Run(fixture,scene,false,cases,"designed-no-occlusion");
            Run(fixture,scene,true,cases,"designed-wall-half");
            Run(fixture,scene,true,cases,"designed-wall-half-repeat");
        }
        {
            const auto scene=BuildScene(wallDepth,true);
            Run(fixture,scene,true,cases,"designed-hole");
        }
        {
            const auto scene=BuildScene(1.0f,false);
            Run(fixture,scene,true,cases,"designed-background-only");
        }
        {
            auto scene=BuildScene(wallDepth,false);
            std::mt19937 random(0x6031u);std::uniform_real_distribution<float> half(0.005f,0.4f);
            for(auto& item:scene.items){const float h=half(random);item.minimum=item.minimum- Vec3{h,h,0};item.maximum=item.maximum+Vec3{h,h,0};}
            Run(fixture,scene,true,cases,"random-wide-conservative");
        }
        const auto stats=fixture.context.states->Stats();
        fixture.Shutdown();
        nlohmann::json report={{"success",true},{"cases",cases},{"uav_barriers",stats.uavBarriers},{"transitions",stats.transitions},
            {"resources_after_shutdown",fixture.context.states->Stats().resources},
            {"memory_used_after_shutdown",fixture.context.memory->Stats().used}};
        std::ofstream file(output/"verification.json",std::ios::binary);file<<report.dump(2)<<'\n';file.close();Check(bool(file),"write occlusion verification");
        std::cout<<"GPU occlusion: "<<cases.size()<<" cases, CPU reference match/conservative, capacity and view contracts passed\n";return 0;
    }
    catch(const std::exception& exception){std::cerr<<exception.what()<<'\n';return 1;}
}
