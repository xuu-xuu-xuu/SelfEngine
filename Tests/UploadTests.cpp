#include "PlatformLayer/Graphics/D3D12/D3D12Diagnostics.h"
#include "PlatformLayer/Graphics/D3D12/D3D12Presentation.h"
#include "PlatformLayer/Windows/Window.h"
#include "PlatformLayer/Windows/ExecutablePath.h"
#include "ResourceLayer/Assets/Primitives.h"
#include "CoreLayer/Localization.h"
#include "CoreLayer/Profiler.h"
#include <atomic>
#include <chrono>
#include <cstring>
#include <fstream>
#include <iostream>
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
    explicit Fixture(const Localization& text)
    {
        Check(CreateEngineWindow(window,false),"upload hidden window");RECT rect{0,0,320,240};
        AdjustWindowRectEx(&rect,static_cast<DWORD>(GetWindowLongPtrW(window.hwnd,GWL_STYLE)),FALSE,0);
        Check(SetWindowPos(window.hwnd,nullptr,0,0,rect.right-rect.left,rect.bottom-rect.top,SWP_NOMOVE|SWP_NOZORDER|SWP_NOACTIVATE)!=FALSE,"upload surface size");
        int code=0;Check(PumpWindowMessages(code),"upload window messages");rhi::Result result;
        device=rhi::Device::CreateD3D12({window.hwnd,320,240},text,result);Ok(result,"upload native device");
    }
    ~Fixture(){if(device)device->Shutdown();window.postQuitOnDestroy=false;DestroyEngineWindow(window);}
};
void NoProblems(rhi::Device& device)
{const auto stats=device.Stats();Check(stats.debugWarnings==0&&stats.debugErrors==0,"upload GPU warning/error/corruption zero");}
std::vector<std::uint8_t> Pattern(UINT width,UINT height,UINT seed)
{
    std::vector<std::uint8_t> pixels(size_t(width)*height*4);
    for(UINT y=0;y<height;++y)for(UINT x=0;x<width;++x)
    {const auto i=(size_t(y)*width+x)*4;pixels[i]=static_cast<std::uint8_t>(x*19+y*31+seed);pixels[i+1]=static_cast<std::uint8_t>(x*7+seed);pixels[i+2]=static_cast<std::uint8_t>(y*41+seed);pixels[i+3]=255;}
    return pixels;
}
std::vector<std::uint8_t> ReadTexture(rhi::Device& device,rhi::TextureHandle handle,const Localization& text)
{
    auto& p=*rhi::D3D12DiagnosticsAccess::Presentation(device);auto* native=rhi::D3D12DiagnosticsAccess::NativeDevice(device);
    auto* texture=rhi::D3D12DiagnosticsAccess::Resource(device,handle);const auto desc=texture->GetDesc();
    D3D12_PLACED_SUBRESOURCE_FOOTPRINT layout{};UINT64 bytes=0;native->GetCopyableFootprints(&desc,0,1,0,&layout,nullptr,nullptr,&bytes);
    D3D12_RESOURCE_DESC buffer{};buffer.Dimension=D3D12_RESOURCE_DIMENSION_BUFFER;buffer.Width=bytes;
    buffer.Height=buffer.DepthOrArraySize=buffer.MipLevels=1;buffer.SampleDesc={1,0};buffer.Layout=D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
    D3D12_HEAP_PROPERTIES heap{};heap.Type=D3D12_HEAP_TYPE_READBACK;heap.CreationNodeMask=heap.VisibleNodeMask=1;
    ComPtr<ID3D12Resource> readback;Hr(native->CreateCommittedResource(&heap,D3D12_HEAP_FLAG_NONE,&buffer,D3D12_RESOURCE_STATE_COPY_DEST,nullptr,IID_PPV_ARGS(readback.GetAddressOf())),"upload texture readback");
    Hr(p.states->Register(*readback.Get(),D3D12_RESOURCE_STATE_COPY_DEST,D3D12TrackedHeap::Readback,"UploadTest.Readback"),"readback state");
    Check(BeginD3D12FrameCommands(p,text),"readback commands");D3D12StateRecording states(*p.states);
    Hr(states.Use(*p.commandList.Get(),*texture,D3D12_RESOURCE_STATE_COPY_SOURCE,"UploadTest"),"read texture transition");
    Hr(states.Use(*p.commandList.Get(),*readback.Get(),D3D12_RESOURCE_STATE_COPY_DEST,"UploadTest"),"readback transition");
    D3D12_TEXTURE_COPY_LOCATION source{};source.pResource=texture;source.Type=D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
    D3D12_TEXTURE_COPY_LOCATION target{};target.pResource=readback.Get();target.Type=D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;target.PlacedFootprint=layout;
    p.commandList->CopyTextureRegion(&target,0,0,0,&source,nullptr);
    Hr(states.Use(*p.commandList.Get(),*texture,D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE,"UploadTest.Restore"),"restore texture state");
    Hr(p.commandList->Close(),"close texture readback");ID3D12CommandList* lists[]{p.commandList.Get()};
    p.gpuWorkPending=p.untrackedSubmission=true;p.queue->ExecuteCommandLists(1,lists);states.Commit();
    struct Guard{D3D12Presentation& p;bool pending=true;~Guard(){if(pending)ReleaseD3D12Presentation(p);}} guard{p};
    Check(WaitForD3D12Gpu(p,text),"wait texture readback");guard.pending=false;
    void* mapped=nullptr;const D3D12_RANGE range{0,static_cast<SIZE_T>(bytes)};Hr(readback->Map(0,&range,&mapped),"map texture readback");
    std::vector<std::uint8_t> pixels(size_t(desc.Width)*desc.Height*4);
    for(UINT row=0;row<desc.Height;++row)std::memcpy(pixels.data()+size_t(row)*desc.Width*4,
        static_cast<const std::byte*>(mapped)+layout.Offset+size_t(row)*layout.Footprint.RowPitch,size_t(desc.Width)*4);
    const D3D12_RANGE noWrite{0,0};readback->Unmap(0,&noWrite);return pixels;
}
void Arena(rhi::Device& device)
{
    auto* native=rhi::D3D12DiagnosticsAccess::NativeDevice(device);D3D12UploadArena arena;
    arena.Initialize(*native,rhi::D3D12DiagnosticsAccess::States(device));Check(arena.SetBudget(64*1024),"small arena budget");
    D3D12UploadSlice first,second,bad;Hr(arena.Allocate(13,16,first),"unaligned size");Hr(arena.Allocate(257,512,second),"texture placement alignment");
    Check(first.resource==second.resource&&second.offset==512&&second.Gpu()%512==0,"shared page, disjoint aligned ranges");
    std::memset(first.cpu,7,13);std::memset(second.cpu,9,257);Check(first.cpu[12]==std::byte{7}&&second.cpu[256]==std::byte{9},"ranges preserve independent bytes");
    Check(FAILED(arena.Allocate(64*1024,256,bad))&&!bad.resource&&arena.Stats().failures==1,"capacity fails without overwriting");
    Check(FAILED(arena.Allocate(1,3,bad)),"non-power-of-two alignment rejected");
    const auto created=arena.Stats().pagesCreated;arena.Reset();Hr(arena.Allocate(64*1024,256,bad),"reset recovers capacity");
    Check(arena.Stats().pagesCreated==created&&arena.Stats().reservedBytes==64*1024,"completed page reused");
    arena.SetDedicated(true);Hr(arena.Allocate(256,256,first),"baseline first");Hr(arena.Allocate(256,256,second),"baseline second");
    Check(first.resource!=second.resource,"baseline keeps dedicated storage");arena.Reset();Check(arena.Stats().reservedBytes==0,"baseline releases on completed reset");
}
void GatedUploads(rhi::Device& device,const Localization& text)
{
    auto& p=*rhi::D3D12DiagnosticsAccess::Presentation(device);rhi::CompletionToken unused;
    Check(device.EndUploadBatch(unused).error==rhi::Error::InvalidState,"end without batch rejected");
    Ok(device.BeginUploadBatch(),"first upload batch");
    Check(device.BeginUploadBatch().error==rhi::Error::InvalidState&&!device.BeginCommands()
        &&device.Resize(320,240).error==rhi::Error::InvalidState&&device.WaitIdle().error==rhi::Error::InvalidState
        &&device.SetUploadMode(rhi::UploadMode::Pooled).error==rhi::Error::InvalidState,"active batch phase restrictions");
    const std::array<UINT,3> widths{3,7,257},heights{2,5,3};std::array<rhi::Texture,3> textures;
    std::array<std::vector<std::uint8_t>,3> expected;
    for(size_t i=0;i<3;++i)
    {expected[i]=Pattern(widths[i],heights[i],static_cast<UINT>(i*37));auto input=expected[i];Ok(device.CreateTexture({widths[i],heights[i]},input,textures[i]),"batched odd-width texture");std::fill(input.begin(),input.end(),0);}
    rhi::Texture invalid;const auto white=Pattern(1,1,0);
    Check(!device.CreateTexture({0,1},white,invalid),"bad creation preserves earlier batch records");
    const std::array<std::uint32_t,3> indices{0,1,2};rhi::Buffer transient;
    Ok(device.CreateBuffer({rhi::BufferUsage::Index,4,"Gated.Upload"},std::as_bytes(std::span(indices)),transient),"transient upload destination");
    std::weak_ptr<const D3D12StateRecord> life=p.states->Find(*rhi::D3D12DiagnosticsAccess::Resource(device,*transient));
    ComPtr<ID3D12Fence> gate;Hr(rhi::D3D12DiagnosticsAccess::NativeDevice(device)->CreateFence(0,D3D12_FENCE_FLAG_NONE,IID_PPV_ARGS(gate.GetAddressOf())),"upload gate");
    struct Gate
    {
        ComPtr<ID3D12Fence> fence;HANDLE event;std::thread worker;
        explicit Gate(ComPtr<ID3D12Fence> value):fence(std::move(value)),event(CreateEventW(nullptr,TRUE,FALSE,nullptr))
        {Check(event!=nullptr,"upload gate event");worker=std::thread([this]{if(WaitForSingleObject(event,400)==WAIT_TIMEOUT)fence->Signal(1);});}
        ~Gate(){fence->Signal(1);SetEvent(event);if(worker.joinable())worker.join();CloseHandle(event);}
    } release{gate};
    Hr(p.queue->Wait(gate.Get(),1),"block direct queue before upload submission");
    rhi::CompletionToken first,second,third;Ok(device.EndUploadBatch(first),"first async upload submission");
    Check(!device.IsComplete(first),"End batch did not synchronously drain");transient.reset();device.Collect();
    Check(!life.expired(),"pending copy retains native destination after logical destroy");
    Ok(device.BeginUploadBatch(),"second frame upload batch");rhi::Texture other;
    const auto otherBytes=Pattern(7,5,213);Ok(device.CreateTexture({7,5},otherBytes,other),"other in-flight frame page");Ok(device.EndUploadBatch(second),"second async batch");
    const auto waits=p.waitCounts[static_cast<size_t>(D3D12WaitReason::FrameReuse)];
    Ok(device.BeginUploadBatch(),"third batch waits before reusing first page");
    Check(p.waitCounts[static_cast<size_t>(D3D12WaitReason::FrameReuse)]>waits,"page reuse waited on owning frame Fence");
    rhi::Texture last;Ok(device.CreateTexture({7,5},otherBytes,last),"write reused first page after completion");Ok(device.EndUploadBatch(third),"third batch submit");
    Ok(device.WaitIdle(),"complete all upload copies");device.Collect();Check(life.expired(),"completed copy releases native destination");
    for(size_t i=0;i<3;++i)Check(ReadTexture(device,*textures[i],text)==expected[i],"row pitch and in-flight bytes exactly preserved");
    Check(ReadTexture(device,*other,text)==otherBytes&&ReadTexture(device,*last,text)==otherBytes,"second and reused upload pages correct");
    for(auto& frame:p.frames)Check(frame.uploads.SetBudget(64*1024),"force small native frame upload budget");
    Ok(device.BeginUploadBatch(),"capacity recovery batch");rhi::Texture beforeFailure,tooLarge,afterFailure;
    Ok(device.CreateTexture({7,5},otherBytes,beforeFailure),"valid record before exhausted request");
    const auto large=Pattern(129,129,0);const auto failures=device.Stats().uploadFailures;
    Check(!device.CreateTexture({129,129},large,tooLarge)&&!tooLarge&&device.Stats().uploadFailures==failures+1,"native upload budget rejects oversized footprint");
    Ok(device.CreateTexture({7,5},otherBytes,afterFailure),"remaining page capacity survives failed request");
    Ok(device.EndUploadBatch(unused),"successful copies survive failed independent request");Ok(device.WaitIdle(),"capacity batch complete");
    Check(ReadTexture(device,*beforeFailure,text)==otherBytes&&ReadTexture(device,*afterFailure,text)==otherBytes,"capacity failure did not cancel successful resources");NoProblems(device);
}
struct Geometry{rhi::Buffer vertices,indices;rhi::Texture white;rhi::Pipeline pipeline;};
Geometry Create(rhi::Device& device,bool batch)
{
    Geometry result;if(batch)Ok(device.BeginUploadBatch(),"benchmark batch begin");const auto cube=se::MakeUnitCube();std::vector<rhi::Vertex> vertices;
    for(const auto& vertex:cube.vertices)vertices.push_back({vertex.position,vertex.normal,vertex.uv,vertex.tangent});
    Ok(device.CreateBuffer({rhi::BufferUsage::Vertex,sizeof(rhi::Vertex),"Grid.vertices"},std::as_bytes(std::span(vertices)),result.vertices),"grid vertices");
    Ok(device.CreateBuffer({rhi::BufferUsage::Index,4,"Grid.indices"},std::as_bytes(std::span(cube.indices)),result.indices),"grid indices");
    const std::array<std::uint8_t,4> white{255,255,255,255};Ok(device.CreateTexture({1,1},white,result.white),"grid white texture");
    if(batch){rhi::CompletionToken token;Ok(device.EndUploadBatch(token),"benchmark batch end");}
    return result;
}
rhi::Image Grid(rhi::Device& device,const Geometry& geometry,UINT count,UINT phase,bool capture)
{
    auto commands=device.BeginCommands();Check(bool(commands),"grid command packet");
    Ok(commands->BeginPass({rhi::PassKind::Clear}),"grid clear");Ok(commands->EndPass(),"grid clear end");Ok(commands->BeginPass({rhi::PassKind::Opaque}),"grid opaque");
    const auto camera=*se::OrthographicRH(17,16,0.1f,100)*se::Translation({0,0,-3});
    for(UINT index=0;index<count;++index)
    {
        se::Vec4 tint{0,0,0,1};switch((index+phase)%3){case 0:tint.x=1;break;case 1:tint.y=1;break;default:tint.z=1;}
        const auto world=se::Translation({float(index%17)-8,7.5f-float(index/17),0})*se::Scaling({0.7f,0.7f,0.7f});
        Ok(commands->Draw({*geometry.vertices,*geometry.indices,*geometry.white,*geometry.pipeline,world,camera,tint,false,true}),"grid independent object constant");
    }
    Ok(commands->EndPass(),"grid end");Ok(commands->Close(),"grid close");rhi::CompletionToken token;rhi::Image image;
    Ok(device.Submit(*commands,token,capture?&image:nullptr),"grid submit");return image;
}
void GridPixels(const rhi::Image& image,UINT count,UINT phase)
{
    for(UINT index=0;index<count;++index)
    {
        const auto x=static_cast<UINT>((float(index%17)+0.5f)*320/17),y=static_cast<UINT>((float(index/17)+0.5f)*240/16);
        const auto offset=(size_t(y)*image.width+x)*4;
        for(UINT channel=0;channel<3;++channel)Check(image.rgba8[offset+channel]==(channel==(index+phase)%3?255:0),"each object, including page boundary, has its own tint");
    }
}
void Save(const rhi::Image& image,const fs::path& path)
{
    std::ofstream stream(path,std::ios::binary);stream<<"P6\n"<<image.width<<' '<<image.height<<"\n255\n";
    for(size_t offset=0;offset<image.rgba8.size();offset+=4)stream.write(reinterpret_cast<const char*>(image.rgba8.data()+offset),3);
    Check(bool(stream),"save grid image");
}
void Benchmark(const Localization& text,const fs::path& output)
{
    std::ofstream csv(output/"upload-comparison.csv");csv<<"mode,batched,draws,profiled_frames,additional_capture_frames,allocation_requests,pages_created,upload_submissions,upload_wall_ms,command_record_wall_ms\n";
    rhi::Image reference;
    for(UINT variant=0;variant<3;++variant)
    {
        Fixture f(text);auto& device=*f.device;const bool pooled=variant!=0,batched=variant==2;
        Ok(device.SetUploadMode(pooled?rhi::UploadMode::Pooled:rhi::UploadMode::DedicatedBaseline),"benchmark strategy");const auto before=device.Stats();
        const auto start=std::chrono::steady_clock::now();auto geometry=Create(device,batched);Ok(device.WaitIdle(),"benchmark upload complete");
        const auto uploadMs=std::chrono::duration<double,std::milli>(std::chrono::steady_clock::now()-start).count();
        Ok(device.CreatePipeline({rhi::PipelineKind::TexturedMesh,GetExecutableDirectory()/"Resources/Shaders/TexturedMesh.hlsl",1},geometry.pipeline),"grid shader");
        se::Profiler profiler;profiler.SetEnabled(true);device.SetCpuProfiler(&profiler);
        for(UINT frame=0;frame<12;++frame){Check(profiler.BeginFrame(frame+1),"benchmark profile begin");Grid(device,geometry,16,frame%3,false);Check(profiler.EndFrame(),"benchmark profile end");}
        auto image=Grid(device,geometry,16,2,true);device.SetCpuProfiler(nullptr);GridPixels(image,16,2);
        if(variant==0)reference=image;else Check(reference.rgba8==image.rgba8,"dedicated, pooled and batched output identical");
        double recordMs=0;for(const auto& frame:profiler.Frames()){const auto found=frame.scopes.find("CPU.CommandRecord");if(found!=frame.scopes.end())recordMs+=found->second.milliseconds;}
        const auto after=device.Stats();Check(after.uploadSubmissions-before.uploadSubmissions==(batched?1:3),"actual upload submissions match selected batching");
        if(pooled)Check(after.uploadPagesCreated-before.uploadPagesCreated<=2,"fixed small workload reuses pages across frames");
        csv<<(pooled?"pooled":"dedicated")<<','<<batched<<",16,12,1,"<<after.uploadAllocations-before.uploadAllocations<<','<<after.uploadPagesCreated-before.uploadPagesCreated<<','<<after.uploadSubmissions-before.uploadSubmissions<<','<<uploadMs<<','<<recordMs<<'\n';
        if(variant==2)
        {
            const auto flushes=device.Stats().globalWaits;
            for(UINT frame=0;frame<12;++frame)Grid(device,geometry,257,frame%3,false);
            Check(device.Stats().globalWaits==flushes,"257-draw ordinary frames have no global wait");
            image=Grid(device,geometry,257,2,true);GridPixels(image,257,2);Save(image,output/"257-objects.ppm");
            Check(device.Stats().uploadReservedBytes>=4*D3D12UploadArena::pageBytes,"both frame contexts expanded beyond one page");
        }
        NoProblems(device);
    }
    Check(bool(csv),"write upload comparison");
}
}
int wmain(int argc,wchar_t** argv)
{
    try
    {
        Check(argc==2,"supply upload test output directory");const auto output=fs::absolute(argv[1]);fs::create_directories(output);
        Localization text;std::string error;Check(text.Load(GetExecutableDirectory()/"Resources/Localization/en-US.txt",error),"upload strings");
        {Fixture f(text);Arena(*f.device);GatedUploads(*f.device,text);}
        Benchmark(text,output);std::cout<<"UploadTests passed (alignment, budget, two queued frames, row bytes, 257 constants, baseline comparison); GPU warnings/errors=0.\n";return 0;
    }
    catch(const std::exception& error){std::cerr<<"UploadTests: "<<error.what()<<'\n';return 1;}
}
