#include "PlatformLayer/Graphics/D3D12/D3D12Diagnostics.h"
#include "PlatformLayer/Graphics/D3D12/D3D12Memory.h"
#include "PlatformLayer/Graphics/D3D12/D3D12Presentation.h"
#include "PlatformLayer/Windows/Window.h"
#include "PlatformLayer/Windows/ExecutablePath.h"
#include "ResourceLayer/Assets/Primitives.h"
#include "CoreLayer/Localization.h"
#include <algorithm>
#include <atomic>
#include <chrono>
#include <fstream>
#include <iostream>
#include <set>
#include <stdexcept>
#include <thread>

namespace rhi=se::rhi;
namespace fs=std::filesystem;
using Microsoft::WRL::ComPtr;
namespace
{
void Check(bool value,const char* message){if(!value)throw std::runtime_error(message);}
void Hr(HRESULT value,const char* message){Check(SUCCEEDED(value),message);}
void Ok(rhi::Result value,const char* message){if(!value)throw std::runtime_error(std::string(message)+": "+value.Key());}
struct Fixture
{
    WindowState window;std::shared_ptr<rhi::Device> device;
    Fixture(const Localization& text,rhi::MemorySettings settings={})
    {
        Check(CreateEngineWindow(window,false),"memory hidden window");RECT rect{0,0,320,240};
        AdjustWindowRectEx(&rect,static_cast<DWORD>(GetWindowLongPtrW(window.hwnd,GWL_STYLE)),FALSE,0);
        Check(SetWindowPos(window.hwnd,nullptr,0,0,rect.right-rect.left,rect.bottom-rect.top,SWP_NOMOVE|SWP_NOZORDER|SWP_NOACTIVATE)!=FALSE,"memory dimensions");
        int code=0;Check(PumpWindowMessages(code),"memory window messages");rhi::Result result;
        device=rhi::Device::CreateD3D12({window.hwnd,320,240},text,result,settings);Ok(result,"memory native device");
    }
    ~Fixture(){if(device)device->Shutdown();window.postQuitOnDestroy=false;DestroyEngineWindow(window);}
};
void NoProblems(rhi::Device& device)
{const auto stats=device.Stats();Check(stats.debugWarnings==0&&stats.debugErrors==0,"memory GPU warning/error/corruption zero");}
void Consistent(const D3D12MemoryAllocator& memory)
{
    auto ranges=memory.Allocations();const auto stats=memory.Stats();UINT64 used=0;
    for(const auto& range:ranges){used+=range.bytes;Check(range.bytes&&range.alignment&&range.offset%range.alignment==0,"API placement alignment");}
    Check(used==stats.used&&ranges.size()==stats.resources&&stats.used+stats.free==stats.reserved&&stats.reserved<=stats.budget,"owned heap accounting self-consistent");
    std::sort(ranges.begin(),ranges.end(),[](const auto& a,const auto& b){return a.heap==b.heap?a.offset<b.offset:a.heap<b.heap;});
    for(size_t index=1;index<ranges.size();++index)
        if(ranges[index].placed&&ranges[index].heap==ranges[index-1].heap)
            Check(ranges[index-1].offset+ranges[index-1].bytes<=ranges[index].offset&&ranges[index-1].category==ranges[index].category,"live placed allocations disjoint and compatible");
}
D3D12_RESOURCE_DESC Buffer(UINT64 bytes)
{
    D3D12_RESOURCE_DESC desc{};desc.Dimension=D3D12_RESOURCE_DIMENSION_BUFFER;desc.Width=bytes;
    desc.Height=desc.DepthOrArraySize=desc.MipLevels=1;desc.SampleDesc={1,0};desc.Layout=D3D12_TEXTURE_LAYOUT_ROW_MAJOR;return desc;
}
D3D12_RESOURCE_DESC Texture(UINT width,UINT height,UINT16 array=1,UINT16 mips=1)
{
    D3D12_RESOURCE_DESC desc{};desc.Dimension=D3D12_RESOURCE_DIMENSION_TEXTURE2D;desc.Width=width;desc.Height=height;
    desc.DepthOrArraySize=array;desc.MipLevels=mips;desc.Format=DXGI_FORMAT_R8G8B8A8_UNORM;desc.SampleDesc={1,0};return desc;
}
void AllocationAndCancellation(const Localization& text)
{
    Fixture f(text,{rhi::MemoryMode::Placed,256*1024,16*1024*1024});auto& device=*f.device;
    auto memory=rhi::D3D12DiagnosticsAccess::Memory(device);auto& p=*rhi::D3D12DiagnosticsAccess::Presentation(device);
    auto* native=rhi::D3D12DiagnosticsAccess::NativeDevice(device);const auto base=memory->Stats().used;
    std::vector<ComPtr<ID3D12Resource>> resources;
    for(const auto desc:{Buffer(13),Buffer(64000),Buffer(65537),Texture(3,2),Texture(257,3),Texture(32,32,2,3)})
    {
        ComPtr<ID3D12Resource> resource;Hr(memory->Create(desc,D3D12_RESOURCE_STATE_COMMON,nullptr,resource,"AllocationTest"),"allocate varied resource");
        const auto info=memory->Allocation(*resource.Get());const auto expected=native->GetResourceAllocationInfo(0,1,&desc);
        Check(info&&info->bytes==expected.SizeInBytes&&info->alignment==expected.Alignment&&info->placed,"use adapter allocation size, not pixel byte count");
        resources.push_back(std::move(resource));Consistent(*memory);
    }
    std::set<UINT64> bufferHeaps,textureHeaps,rtHeaps;
    for(const auto& info:memory->Allocations())switch(info.category)
    {case D3D12MemoryCategory::Buffer:bufferHeaps.insert(info.heap);break;case D3D12MemoryCategory::Texture:textureHeaps.insert(info.heap);break;default:rtHeaps.insert(info.heap);}
    Check(!bufferHeaps.empty()&&!textureHeaps.empty()&&!rtHeaps.empty(),"all three heap compatibility categories present");
    for(const auto heap:bufferHeaps)Check(!textureHeaps.contains(heap)&&!rtHeaps.contains(heap),"buffer heap not shared with incompatible texture");
    auto record=p.states->Find(*resources.back().Get());Check(record&&record->states.size()==6&&record->activationPending,"array/mip states await placed activation");
    Check(BeginD3D12FrameCommands(p,text),"cancel activation record begin");
    {
        D3D12StateRecording cancelled(*p.states);Hr(cancelled.Use(*p.commandList.Get(),*resources.back().Get(),D3D12_RESOURCE_STATE_COPY_DEST,"CancelledActivation"),"candidate alias activation");
        Check(cancelled.PendingActivations()==1&&record->activationPending,"recording did not publish activation");Hr(p.commandList->Close(),"close cancelled list");
    }
    Check(record->activationPending,"cancelled activation rolled back");
    Check(BeginD3D12FrameCommands(p,text),"discard cancelled list references");Hr(p.commandList->Close(),"close empty replacement list");
    resources.clear();p.states->Collect();Check(memory->Stats().used==base,"native resource destruction returns allocation bytes");
    memory->Trim();Consistent(*memory);Check(memory->Stats().placedHeaps==1,"only live depth heap remains after trim");NoProblems(device);
}
struct Geometry{rhi::Buffer vertices,indices;};
Geometry CreateGeometry(rhi::Device& device)
{
    Geometry result;const auto cube=se::MakeUnitCube();std::vector<rhi::Vertex> vertices;
    for(const auto& vertex:cube.vertices)vertices.push_back({vertex.position,vertex.normal,vertex.uv,vertex.tangent});
    Ok(device.CreateBuffer({rhi::BufferUsage::Vertex,sizeof(rhi::Vertex),"Memory.vertices"},std::as_bytes(std::span(vertices)),result.vertices),"memory vertices");
    Ok(device.CreateBuffer({rhi::BufferUsage::Index,4,"Memory.indices"},std::as_bytes(std::span(cube.indices)),result.indices),"memory indices");return result;
}
rhi::Pipeline Pipeline(rhi::Device& device)
{rhi::Pipeline result;Ok(device.CreatePipeline({rhi::PipelineKind::TexturedMesh,GetExecutableDirectory()/"Resources/Shaders/TexturedMesh.hlsl",1},result),"memory pipeline");return result;}
rhi::Texture Color(rhi::Device& device,std::uint8_t value)
{const std::array<std::uint8_t,4> pixels{value,value,value,255};rhi::Texture result;Ok(device.CreateTexture({1,1},pixels,result),"memory texture");return result;}
std::unique_ptr<rhi::CommandContext> Record(rhi::Device& device,const Geometry& geometry,rhi::TextureHandle texture,rhi::PipelineHandle pipeline)
{
    auto commands=device.BeginCommands();Check(bool(commands),"memory commands");Ok(commands->BeginPass({rhi::PassKind::Clear}),"memory clear");Ok(commands->EndPass(),"memory clear end");
    Ok(commands->BeginPass({rhi::PassKind::Opaque}),"memory opaque");
    const auto camera=*se::PerspectiveRH(se::Pi/3,4.0f/3,0.1f,100)*se::Translation({0,0,-3});
    Ok(commands->Draw({*geometry.vertices,*geometry.indices,texture,pipeline,{},camera,{1,1,1,1},false,true}),"memory draw");
    Ok(commands->EndPass(),"memory end");Ok(commands->Close(),"memory close");return commands;
}
rhi::Image Capture(rhi::Device& device,const Geometry& geometry,rhi::TextureHandle texture,rhi::PipelineHandle pipeline)
{
    auto commands=Record(device,geometry,texture,pipeline);rhi::Image image;rhi::CompletionToken token;Ok(device.Submit(*commands,token,&image),"memory capture");
    const auto offset=(size_t(120)*320+160)*4;Check(image.width==320&&image.height==240&&std::abs(int(image.rgba8[offset])-128)<=1,"memory storage preserved texture data");return image;
}
bool Overlap(const D3D12AllocationInfo& a,const D3D12AllocationInfo& b)
{return a.heap==b.heap&&a.offset<b.offset+b.bytes&&b.offset<a.offset+a.bytes;}
void FenceReuse(const Localization& text)
{
    Fixture f(text,{rhi::MemoryMode::Placed,256*1024,16*1024*1024});auto& device=*f.device;
    auto memory=rhi::D3D12DiagnosticsAccess::Memory(device);auto pipeline=Pipeline(device);
    Ok(device.BeginUploadBatch(),"fence setup batch");auto old=CreateGeometry(device);auto texture=Color(device,128);
    rhi::CompletionToken token;Ok(device.EndUploadBatch(token),"fence setup submit");Ok(device.WaitIdle(),"fence setup ready");
    Capture(device,old,*texture,*pipeline);const auto oldV=*memory->Allocation(*rhi::D3D12DiagnosticsAccess::Resource(device,*old.vertices));
    const auto oldI=*memory->Allocation(*rhi::D3D12DiagnosticsAccess::Resource(device,*old.indices));
    auto commands=Record(device,old,*texture,*pipeline);auto& p=*rhi::D3D12DiagnosticsAccess::Presentation(device);
    ComPtr<ID3D12Fence> gate;Hr(rhi::D3D12DiagnosticsAccess::NativeDevice(device)->CreateFence(0,D3D12_FENCE_FLAG_NONE,IID_PPV_ARGS(gate.GetAddressOf())),"memory fence gate");
    struct Gate
    {
        ComPtr<ID3D12Fence> fence;HANDLE event;std::atomic<bool> timedOut{false};std::thread worker;
        explicit Gate(ComPtr<ID3D12Fence> value):fence(std::move(value)),event(CreateEventW(nullptr,TRUE,FALSE,nullptr))
        {Check(event!=nullptr,"memory gate event");worker=std::thread([this]{if(WaitForSingleObject(event,3000)==WAIT_TIMEOUT){timedOut=true;fence->Signal(1);}});}
        void Release(){Hr(fence->Signal(1),"release memory gate");SetEvent(event);}
        ~Gate(){fence->Signal(1);SetEvent(event);if(worker.joinable())worker.join();CloseHandle(event);}
    } release{gate};
    Hr(p.queue->Wait(gate.Get(),1),"hold resource allocation in flight");Ok(device.Submit(*commands,token),"submit gated memory users");
    Check(!device.IsComplete(token)&&!release.timedOut,"memory GPU work not completed");old={};device.Collect();
    auto ranges=memory->Allocations();Check(std::any_of(ranges.begin(),ranges.end(),[&](const auto& range){return range.serial==oldV.serial;}),"destroyed logical buffer retains allocation lease");
    Ok(device.BeginUploadBatch(),"prepare replacement while old heap ranges in flight");auto next=CreateGeometry(device);rhi::CompletionToken upload;
    Ok(device.EndUploadBatch(upload),"queue replacement copies without wait");
    const auto nextV=*memory->Allocation(*rhi::D3D12DiagnosticsAccess::Resource(device,*next.vertices));
    const auto nextI=*memory->Allocation(*rhi::D3D12DiagnosticsAccess::Resource(device,*next.indices));
    Check(!Overlap(oldV,nextV)&&!Overlap(oldV,nextI)&&!Overlap(oldI,nextV)&&!Overlap(oldI,nextI),"new placed resources cannot overlap in-flight allocations");
    Consistent(*memory);Check(!release.timedOut,"replacement preparation remained asynchronous");release.Release();Ok(device.WaitIdle(),"complete gated heap users");device.Collect();
    ranges=memory->Allocations();Check(std::none_of(ranges.begin(),ranges.end(),[&](const auto& range){return range.serial==oldV.serial||range.serial==oldI.serial;}),"completed native resources release heap intervals");
    Ok(device.BeginUploadBatch(),"reuse completed heap intervals");auto recycled=CreateGeometry(device);Ok(device.EndUploadBatch(upload),"reused buffer upload");
    const auto reused=*memory->Allocation(*rhi::D3D12DiagnosticsAccess::Resource(device,*recycled.vertices));
    Check(reused.heap==oldV.heap&&reused.offset==oldV.offset,"completed physical interval actually reused");
    Capture(device,recycled,*texture,*pipeline);next={};recycled={};texture.reset();pipeline.reset();Ok(device.TrimMemory(),"trim released heaps");
    Check(memory->Stats().resources==1&&memory->Stats().placedHeaps==1,"only active depth remains after native retirement");Consistent(*memory);
    Check(device.Stats().stateActivations>0,"placed resources activated through unique state tracker");NoProblems(device);
}
void Budget(const Localization& text)
{
    Fixture f(text,{rhi::MemoryMode::Placed,256*1024,1024*1024});auto& device=*f.device;auto memory=rhi::D3D12DiagnosticsAccess::Memory(device);
    auto pipeline=Pipeline(device);auto geometry=CreateGeometry(device);auto texture=Color(device,128);
    const auto before=Capture(device,geometry,*texture,*pipeline);
    std::vector<std::uint8_t> large(1024*1024*4,255);rhi::Texture failed;
    Check(device.CreateTexture({1024,1024},large,failed).error==rhi::Error::CapacityExceeded&&!failed,"oversized DEFAULT request rejected by engine budget");
    std::vector<rhi::Texture> pressure;bool exhausted=false;
    for(UINT index=0;index<32;++index)
    {
        rhi::Texture item;const std::array<std::uint8_t,4> pixel{255,0,255,255};const auto result=device.CreateTexture({1,1},pixel,item);
        if(!result){Check(result.error==rhi::Error::CapacityExceeded&&!item,"small heap budget exhaustion is explicit");exhausted=true;break;}
        pressure.push_back(std::move(item));Consistent(*memory);
    }
    Check(exhausted&&memory->Stats().budgetFailures>=2,"bounded pressure reaches budget without overflow");
    Check(Capture(device,geometry,*texture,*pipeline).rgba8==before.rgba8,"budget failure preserves previous successful resources");
    pressure.clear();geometry={};texture.reset();pipeline.reset();Ok(device.TrimMemory(),"budget cleanup and trim");
    Check(memory->Stats().resources==1&&memory->Stats().placedHeaps==1,"budget allocator recovers to active depth baseline");Consistent(*memory);NoProblems(device);
}
void Compare(const Localization& text,const fs::path& output)
{
    std::ofstream csv(output/"memory-comparison.csv");csv<<"mode,resources,reserved_bytes,used_bytes,free_bytes,explicit_heap_creations,committed_allocation_creations,resource_creations,upload_submissions,create_upload_wall_ms\n";
    rhi::Image reference;
    for(const auto mode:{rhi::MemoryMode::CommittedBaseline,rhi::MemoryMode::Placed})
    {
        Fixture f(text,{mode,1024*1024,64*1024*1024});auto& device=*f.device;auto memory=rhi::D3D12DiagnosticsAccess::Memory(device);auto pipeline=Pipeline(device);
        const auto start=std::chrono::steady_clock::now();Ok(device.BeginUploadBatch(),"memory comparison batch");auto geometry=CreateGeometry(device);
        std::vector<rhi::Buffer> buffers(64);const std::array<std::uint32_t,3> indices{0,0,0};
        for(auto& buffer:buffers)Ok(device.CreateBuffer({rhi::BufferUsage::Index,4,"Memory.Pressure"},std::as_bytes(std::span(indices)),buffer),"memory pressure buffer");
        std::vector<rhi::Texture> textures;for(UINT index=0;index<24;++index)textures.push_back(Color(device,static_cast<std::uint8_t>(128+index)));
        rhi::CompletionToken token;Ok(device.EndUploadBatch(token),"memory comparison one submit");Ok(device.WaitIdle(),"memory comparison upload ready");
        const auto elapsed=std::chrono::duration<double,std::milli>(std::chrono::steady_clock::now()-start).count();
        auto image=Capture(device,geometry,*textures[0],*pipeline);if(reference.rgba8.empty())reference=image;else Check(image.rgba8==reference.rgba8,"committed and placed complete image identical");
        Consistent(*memory);const auto stats=memory->Stats();
        Check(stats.resources==91&&stats.resourceCreations==91,"same DEFAULT resource set in both modes");
        if(mode==rhi::MemoryMode::Placed)Check(stats.placedHeaps<stats.resources&&stats.committedResources==0,"placed resources share actual heaps");
        else Check(stats.committedResources==91&&stats.free==0,"committed baseline uses one backing allocation per resource");
        const auto name=mode==rhi::MemoryMode::Placed?"placed":"committed";
        csv<<name<<','<<stats.resources<<','<<stats.reserved<<','<<stats.used<<','<<stats.free<<','<<stats.heapCreations<<','<<stats.committedCreations<<','<<stats.resourceCreations<<','<<device.Stats().uploadSubmissions<<','<<elapsed<<'\n';
        std::ofstream picture(output/(std::string(name)+".ppm"),std::ios::binary);picture<<"P6\n"<<image.width<<' '<<image.height<<"\n255\n";
        for(size_t offset=0;offset<image.rgba8.size();offset+=4)picture.write(reinterpret_cast<const char*>(image.rgba8.data()+offset),3);
        Check(bool(picture),"memory comparison screenshot");
        geometry={};buffers.clear();textures.clear();pipeline.reset();Ok(device.TrimMemory(),"comparison trim");Check(memory->Stats().resources==1,"comparison resources retire");NoProblems(device);
    }
    Check(bool(csv),"memory comparison CSV");
}
}
int wmain(int argc,wchar_t** argv)
{
    try
    {
        Check(argc==2,"supply memory output directory");const auto output=fs::absolute(argv[1]);fs::create_directories(output);
        Localization text;std::string error;Check(text.Load(GetExecutableDirectory()/"Resources/Localization/en-US.txt",error),"memory strings");
        AllocationAndCancellation(text);FenceReuse(text);Budget(text);Compare(text,output);
        std::cout<<"MemoryTests passed (API footprints, compatibility, activation rollback, Fence reuse, budget, trim and committed comparison); GPU warnings/errors=0.\n";return 0;
    }
    catch(const std::exception& error){std::cerr<<"MemoryTests: "<<error.what()<<'\n';return 1;}
}
