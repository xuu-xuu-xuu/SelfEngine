#include "PlatformLayer/Graphics/D3D12/D3D12Diagnostics.h"
#include "PlatformLayer/Graphics/D3D12/D3D12Presentation.h"
#include "PlatformLayer/Windows/Window.h"
#include "PlatformLayer/Windows/ExecutablePath.h"
#include "ResourceLayer/Assets/Primitives.h"
#include "CoreLayer/Localization.h"
#include <atomic>
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
    void Size(UINT width,UINT height)
    {
        RECT rect{0,0,static_cast<LONG>(width),static_cast<LONG>(height)};
        AdjustWindowRectEx(&rect,static_cast<DWORD>(GetWindowLongPtrW(window.hwnd,GWL_STYLE)),FALSE,0);
        Check(SetWindowPos(window.hwnd,nullptr,0,0,rect.right-rect.left,rect.bottom-rect.top,SWP_NOMOVE|SWP_NOZORDER|SWP_NOACTIVATE)!=FALSE,"descriptor HWND size");
        int code=0;Check(PumpWindowMessages(code),"descriptor messages");
    }
    explicit Fixture(const Localization& text)
    {
        Check(CreateEngineWindow(window,false),"descriptor hidden window");Size(320,240);rhi::Result result;
        device=rhi::Device::CreateD3D12({window.hwnd,320,240},text,result);Ok(result,"descriptor hardware device");
    }
    ~Fixture(){if(device)device->Shutdown();window.postQuitOnDestroy=false;DestroyEngineWindow(window);}
};
void NoProblems(rhi::Device& device)
{const auto stats=device.Stats();Check(stats.debugWarnings==0&&stats.debugErrors==0,"descriptor GPU warning/error/corruption zero");}
void Pools(rhi::Device& device)
{
    for(const auto type:{D3D12_DESCRIPTOR_HEAP_TYPE_RTV,D3D12_DESCRIPTOR_HEAP_TYPE_DSV,D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV})
    {
        auto pool=std::make_shared<D3D12DescriptorPool>(*rhi::D3D12DiagnosticsAccess::NativeDevice(device),type,2,6);
        std::shared_ptr<D3D12DescriptorRange> a,b,c,d,e,bad;
        Hr(pool->Allocate(1,a),"small pool first");Hr(pool->Allocate(1,b),"small pool second");Hr(pool->Allocate(2,c),"grow another CPU page");
        const auto original=a->Cpu().ptr;auto retained=a;a.reset();Check(pool->Stats().used==4,"shared range owns slots until last owner");
        Check(!pool->Configure(1,6),"configure cannot invalidate live range");b.reset();retained.reset();Check(pool->Stats().used==2,"adjacent ranges freed");
        Hr(pool->Allocate(2,d),"coalesced free range");Check(d->Cpu().ptr==original,"free-list actually reuses contiguous slots");
        Hr(pool->Allocate(2,e),"fill expanded pool");Check(pool->Stats().capacity==6&&pool->Stats().used==6,"pool capacity/used self-consistent");
        Check(FAILED(pool->Allocate(1,bad))&&!bad&&pool->Stats().failures==1,"bounded pool fails without overrun");
        std::set<SIZE_T> handles;for(const auto& range:{c,d,e})
        {Check(range->Type()==type&&!range->Cpu(range->Count()).ptr,"typed range bounds");for(UINT index=0;index<range->Count();++index)Check(handles.insert(range->Cpu(index).ptr).second,"live ranges never overlap");}
        c.reset();d.reset();e.reset();Check(pool->Stats().used==0&&pool->Configure(1,2),"all ranges reclaimed and pool reconfigurable");
    }
    NoProblems(device);
}
struct Geometry{rhi::Buffer vertices,indices;rhi::Pipeline pipeline;};
Geometry Create(rhi::Device& device)
{
    Geometry result;const auto cube=se::MakeUnitCube();std::vector<rhi::Vertex> vertices;
    for(const auto& vertex:cube.vertices)vertices.push_back({vertex.position,vertex.normal,vertex.uv,vertex.tangent});
    Ok(device.CreatePipeline({rhi::PipelineKind::TexturedMesh,GetExecutableDirectory()/"Resources/Shaders/TexturedMesh.hlsl",1},result.pipeline),"descriptor pipeline");
    Ok(device.BeginUploadBatch(),"descriptor geometry batch");
    Ok(device.CreateBuffer({rhi::BufferUsage::Vertex,sizeof(rhi::Vertex),"Materials.vertices"},std::as_bytes(std::span(vertices)),result.vertices),"material geometry");
    Ok(device.CreateBuffer({rhi::BufferUsage::Index,4,"Materials.indices"},std::as_bytes(std::span(cube.indices)),result.indices),"material indices");
    rhi::CompletionToken token;Ok(device.EndUploadBatch(token),"descriptor geometry submit");Ok(device.WaitIdle(),"geometry complete");return result;
}
std::array<std::uint8_t,4> Color(UINT index,UINT seed)
{return {static_cast<std::uint8_t>(index*17+seed*13),static_cast<std::uint8_t>(index*41+seed*7+22),static_cast<std::uint8_t>(index*67+seed*19+80),255};}
std::vector<rhi::Texture> Palette(rhi::Device& device,UINT count,UINT seed)
{
    Ok(device.BeginUploadBatch(),"palette upload begin");std::vector<rhi::Texture> palette(count);
    for(UINT index=0;index<count;++index){const auto color=Color(index,seed);Ok(device.CreateTexture({1,1},color,palette[index]),"palette CPU SRV range and upload");}
    rhi::CompletionToken token;Ok(device.EndUploadBatch(token),"palette submit");return palette;
}
rhi::DrawIndexed Draw(const Geometry& geometry,rhi::TextureHandle texture,UINT cell,bool data=false)
{
    const auto camera=*se::OrthographicRH(8,6,0.1f,100)*se::Translation({0,0,-3});
    const auto world=se::Translation({float(cell%8)-3.5f,2.5f-float(cell/8),0})*se::Scaling({0.75f,0.75f,0.75f});
    return {*geometry.vertices,*geometry.indices,texture,*geometry.pipeline,world,camera,{1,1,1,1},data,true};
}
std::unique_ptr<rhi::CommandContext> Record(rhi::Device& device,const Geometry& geometry,std::span<const rhi::Texture> palette)
{
    auto commands=device.BeginCommands();Check(bool(commands),"material command packet");
    Ok(commands->BeginPass({rhi::PassKind::Clear}),"material clear");Ok(commands->EndPass(),"material clear end");Ok(commands->BeginPass({rhi::PassKind::Opaque}),"material opaque");
    for(UINT index=0;index<palette.size();++index)Ok(commands->Draw(Draw(geometry,*palette[index],index)),"material draw");
    Ok(commands->EndPass(),"material end");Ok(commands->Close(),"material close");return commands;
}
rhi::Image Submit(rhi::Device& device,rhi::CommandContext& commands,bool capture)
{rhi::CompletionToken token;rhi::Image image;Ok(device.Submit(commands,token,capture?&image:nullptr),"material submit");return image;}
void Pixels(const rhi::Image& image,UINT count,UINT seed)
{
    Check(image.width==320&&image.height==240,"material image dimensions");
    for(UINT index=0;index<count;++index)
    {
        const auto offset=(size_t(index/8*40+20)*image.width+index%8*40+20)*4;const auto expected=Color(index,seed);
        for(UINT channel=0;channel<4;++channel)Check(std::abs(int(image.rgba8[offset+channel])-int(expected[channel]))<=1,"material slot has expected texture, not stale/recycled texture");
    }
}
void Save(const rhi::Image& image,const fs::path& path)
{
    std::ofstream stream(path,std::ios::binary);stream<<"P6\n"<<image.width<<' '<<image.height<<"\n255\n";
    for(size_t index=0;index<image.rgba8.size();index+=4)stream.write(reinterpret_cast<const char*>(image.rgba8.data()+index),3);
    Check(bool(stream),"save material image");
}
void Limits(const Localization& text)
{
    Fixture f(text);auto& device=*f.device;
    Check(device.ConfigureDescriptors({0,8,1,2}).error==rhi::Error::InvalidArgument,"invalid descriptor limits rejected");
    Ok(device.ConfigureDescriptors({2,10,1,2}),"small persistent and visible limits");auto geometry=Create(device);auto palette=Palette(device,5,0);
    rhi::Texture overflow;const auto color=Color(5,0);
    Check(device.CreateTexture({1,1},color,overflow).error==rhi::Error::CapacityExceeded&&!overflow,"persistent descriptor exhaustion is explicit");
    Check(device.ConfigureDescriptors({}).error==rhi::Error::InvalidState,"live texture prevents reconfigure");
    auto commands=device.BeginCommands();Ok(commands->BeginPass({rhi::PassKind::Clear}),"limited clear");Ok(commands->EndPass(),"limited clear end");Ok(commands->BeginPass({rhi::PassKind::Opaque}),"limited opaque");
    Ok(commands->Draw(Draw(geometry,*palette[0],0)),"visible first");Ok(commands->Draw(Draw(geometry,*palette[1],1)),"visible second");
    Check(commands->Draw(Draw(geometry,*palette[2],2)).error==rhi::Error::CapacityExceeded,"third unique binding rejected before GPU recording");
    Ok(commands->Draw(Draw(geometry,*palette[0],0)),"duplicate binding does not consume extra slot");
    Ok(commands->EndPass(),"limited pass end");Ok(commands->Close(),"limited packet close");palette.clear();
    Check(device.ConfigureDescriptors({}).error==rhi::Error::InvalidState,"recorded references keep CPU ranges valid after logical destroy");
    const auto frame=rhi::D3D12DiagnosticsAccess::Presentation(device)->frameCursor;const auto image=Submit(device,*commands,true);Pixels(image,2,0);
    const auto stats=device.Stats();Check(rhi::D3D12DiagnosticsAccess::Presentation(device)->frames[frame].descriptors.Stats().used==2
        &&stats.descriptorFailures>=2&&stats.persistentDescriptors==0,"legal prefix submits and CPU descriptors retire");
    Ok(device.ConfigureDescriptors({}),"reconfigure after records and Fence retire");NoProblems(device);
}
void LifetimeAndChanges(const Localization& text,const fs::path& output)
{
    Fixture f(text);auto& device=*f.device;Ok(device.ConfigureDescriptors({2,192,2,64}),"small pages with bounded growth");
    auto geometry=Create(device);auto old=Palette(device,24,0);Ok(device.WaitIdle(),"first palette complete");
    auto reference=Record(device,geometry,old);Pixels(Submit(device,*reference,true),24,0);
    auto commands=Record(device,geometry,old);std::set<SIZE_T> oldSlots;for(const auto& texture:old)oldSlots.insert(rhi::D3D12DiagnosticsAccess::SrvCpu(device,*texture).ptr);
    auto oldLife=rhi::D3D12DiagnosticsAccess::Lifetime(device,*old[0]);auto& p=*rhi::D3D12DiagnosticsAccess::Presentation(device);
    ComPtr<ID3D12Fence> gate;Hr(rhi::D3D12DiagnosticsAccess::NativeDevice(device)->CreateFence(0,D3D12_FENCE_FLAG_NONE,IID_PPV_ARGS(gate.GetAddressOf())),"descriptor gate");
    struct Gate
    {
        ComPtr<ID3D12Fence> fence;HANDLE event;std::atomic<bool> timedOut{false};std::thread worker;
        explicit Gate(ComPtr<ID3D12Fence> value):fence(std::move(value)),event(CreateEventW(nullptr,TRUE,FALSE,nullptr))
        {Check(event!=nullptr,"descriptor gate event");worker=std::thread([this]{if(WaitForSingleObject(event,3000)==WAIT_TIMEOUT){timedOut=true;fence->Signal(1);}});}
        void Release(){Hr(fence->Signal(1),"release descriptor gate");SetEvent(event);}
        ~Gate(){fence->Signal(1);SetEvent(event);if(worker.joinable())worker.join();CloseHandle(event);}
    } release{gate};
    Hr(p.queue->Wait(gate.Get(),1),"hold texture table in flight");const auto frame=p.frameCursor;rhi::CompletionToken token;
    Ok(device.Submit(*commands,token),"submit palette behind gate");Check(!device.IsComplete(token)&&!release.timedOut,"palette demonstrably in flight");
    old.clear();device.Collect();Check(!oldLife.expired()&&device.Stats().retiringDescriptors>=48,"in-flight descriptor ranges retained after logical texture destruction");
    const auto table=p.frames[frame].descriptors.Stats();Check(table.used==24&&table.capacity>=24,"visible table grows from two slots");
    auto next=Palette(device,24,1);for(const auto& texture:next)Check(!oldSlots.contains(rhi::D3D12DiagnosticsAccess::SrvCpu(device,*texture).ptr),"in-flight persistent CPU slot not reused");
    Check(!release.timedOut,"new resource preparation did not require gated frame reuse");release.Release();Ok(device.WaitIdle(),"finish gated materials");device.Collect();
    Check(oldLife.expired()&&device.Stats().retiringDescriptors==0&&device.Stats().persistentDescriptors==48,"completed resources and CPU slots really retire");
    commands=Record(device,geometry,next);Pixels(Submit(device,*commands,true),24,1);
    auto recycled=Palette(device,24,2);bool reused=false;for(const auto& texture:recycled)reused|=oldSlots.contains(rhi::D3D12DiagnosticsAccess::SrvCpu(device,*texture).ptr);
    Check(reused,"completed CPU descriptor slots actually reused");next.clear();commands=Record(device,geometry,recycled);Pixels(Submit(device,*commands,true),24,2);
    recycled.clear();Ok(device.WaitIdle(),"palette cleanup");device.Collect();
    std::ofstream csv(output/"descriptor-cycles.csv");csv<<"cycle,logical_textures,persistent_used,persistent_capacity,retiring_slots,frame_capacity,heap_creations\n";
    for(UINT cycle=0;cycle<16;++cycle)
    {
        auto palette=Palette(device,24,cycle+3);commands=Record(device,geometry,palette);palette.clear();const auto waits=device.Stats().globalWaits;
        const bool capture=cycle%4==3;auto image=Submit(device,*commands,capture);if(capture){Pixels(image,24,cycle+3);if(cycle==15)Save(image,output/"materials.ppm");}
        else Check(device.Stats().globalWaits==waits,"ordinary material replacement adds no global flush");
        const auto stats=device.Stats();Check(stats.persistentDescriptors<=192&&stats.descriptorCapacity<=192,"material churn stays within configured CPU descriptor bound");
        csv<<cycle<<','<<stats.textures<<','<<stats.persistentDescriptors<<','<<stats.descriptorCapacity<<','<<stats.retiringDescriptors<<','<<stats.frameDescriptorCapacity<<','<<stats.descriptorHeapsCreated<<'\n';
    }
    Check(bool(csv),"descriptor cycle statistics");Ok(device.WaitIdle(),"final descriptor retirement");device.Collect();
    Check(device.Stats().persistentDescriptors==0&&device.Stats().retiringDescriptors==0,"all unused material descriptor ranges reclaimed");
    f.Size(512,256);Ok(device.Resize(512,256),"pooled RTV/DSV resize");f.Size(320,240);Ok(device.Resize(320,240),"pooled RTV/DSV restore");
    Check(device.Stats().rtvDescriptors==2&&device.Stats().dsvDescriptors==1,"RTV/DSV ranges reused across resize");NoProblems(device);
}
}
int wmain(int argc,wchar_t** argv)
{
    try
    {
        Check(argc==2,"supply descriptor output directory");const auto output=fs::absolute(argv[1]);fs::create_directories(output);
        Localization text;std::string error;Check(text.Load(GetExecutableDirectory()/"Resources/Localization/en-US.txt",error),"descriptor strings");
        {Fixture f(text);Pools(*f.device);}Limits(text);LifetimeAndChanges(text,output);
        std::cout<<"DescriptorTests passed (typed pools, capacity, dedup, controlled Fence, material churn and RTV/DSV resize); GPU warnings/errors=0.\n";return 0;
    }
    catch(const std::exception& error){std::cerr<<"DescriptorTests: "<<error.what()<<'\n';return 1;}
}
