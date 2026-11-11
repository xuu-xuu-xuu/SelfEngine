#include "PlatformLayer/Graphics/D3D12/D3D12Diagnostics.h"
#include "PlatformLayer/Graphics/D3D12/D3D12Context.h"
#include "PlatformLayer/Graphics/D3D12/D3D12Presentation.h"
#include "PlatformLayer/Windows/Window.h"
#include "PlatformLayer/Windows/ExecutablePath.h"
#include "FunctionLayer/Rendering/StaticModelRender.h"
#include "ResourceLayer/Assets/Primitives.h"
#include "ResourceLayer/Assets/AssetIO.h"
#include "CoreLayer/Localization.h"
#include <d3d12sdklayers.h>
#include <atomic>
#include <fstream>
#include <iostream>
#include <stdexcept>
#include <thread>

namespace fs=std::filesystem;
namespace rhi=se::rhi;
using Microsoft::WRL::ComPtr;
namespace
{
void Check(bool value,const char* message){if(!value)throw std::runtime_error(message);}
void Hr(HRESULT result,const char* message){Check(SUCCEEDED(result),message);}
void Ok(rhi::Result result,const char* message){if(!result)throw std::runtime_error(std::string(message)+": "+result.Key());}
struct WindowFixture
{
    WindowState window;
    WindowFixture(){Check(CreateEngineWindow(window,false),"state test hidden window");}
    ~WindowFixture(){window.postQuitOnDestroy=false;DestroyEngineWindow(window);}
    void Size(UINT width,UINT height)
    {
        RECT rect{0,0,static_cast<LONG>(width),static_cast<LONG>(height)};
        AdjustWindowRectEx(&rect,static_cast<DWORD>(GetWindowLongPtrW(window.hwnd,GWL_STYLE)),FALSE,0);
        Check(SetWindowPos(window.hwnd,nullptr,0,0,rect.right-rect.left,rect.bottom-rect.top,SWP_NOMOVE|SWP_NOZORDER|SWP_NOACTIVATE)!=FALSE,"state test resize");
        int code=0;Check(PumpWindowMessages(code)&&window.width==width&&window.height==height,"state actual HWND dimensions");
    }
};
ComPtr<ID3D12Resource> Make(ID3D12Device& device,D3D12_RESOURCE_DESC description,D3D12_HEAP_TYPE heapType,D3D12_RESOURCE_STATES initial)
{
    D3D12_HEAP_PROPERTIES heap{};heap.Type=heapType;heap.CreationNodeMask=heap.VisibleNodeMask=1;
    ComPtr<ID3D12Resource> resource;Hr(device.CreateCommittedResource(&heap,D3D12_HEAP_FLAG_NONE,&description,initial,nullptr,
        IID_PPV_ARGS(resource.GetAddressOf())),"create state test resource");return resource;
}
void NativeStates(const Localization& text)
{
    WindowFixture window;D3D12Context context;D3D12Presentation p;
    struct Guard{D3D12Presentation& p;~Guard(){ReleaseD3D12Presentation(p);}} guard{p};
    Check(InitializeD3D12Context(context,text)&&InitializeD3D12Presentation(p,context,window.window.hwnd,text),"state native setup");
    Check(D3D12StateRegistry::ForDevice(*context.device.Get())==context.states&&p.states==context.states,"one registry per native device");
    const auto base=context.states->Stats().resources;
    D3D12_RESOURCE_DESC description{};description.Dimension=D3D12_RESOURCE_DIMENSION_TEXTURE2D;description.Width=4;description.Height=4;
    description.DepthOrArraySize=2;description.MipLevels=2;description.Format=DXGI_FORMAT_R8G8B8A8_UNORM;description.SampleDesc={1,0};
    auto array=Make(*context.device.Get(),description,D3D12_HEAP_TYPE_DEFAULT,D3D12_RESOURCE_STATE_COMMON);
    Hr(context.states->Register(*array.Get(),D3D12_RESOURCE_STATE_COMMON,D3D12TrackedHeap::Default,"MipArray"),"register subresources");
    Check(context.states->Find(*array.Get())->states.size()==4,"mip and array subresource count");
    ComPtr<ID3D12CommandAllocator> allocator;ComPtr<ID3D12GraphicsCommandList> commands;
    Hr(context.device->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT,IID_PPV_ARGS(allocator.GetAddressOf())),"state allocator");
    Hr(context.device->CreateCommandList(0,D3D12_COMMAND_LIST_TYPE_DIRECT,allocator.Get(),nullptr,IID_PPV_ARGS(commands.GetAddressOf())),"state commands");
    const auto execute=[&](D3D12StateRecording& states){
        Hr(commands->Close(),"close state commands");ID3D12CommandList* lists[]{commands.Get()};p.gpuWorkPending=p.untrackedSubmission=true;
        p.queue->ExecuteCommandLists(1,lists);states.Commit();Check(WaitForD3D12Gpu(p,text),"state submission completed");
    };
    const auto reset=[&]{Hr(allocator->Reset(),"reset state allocator");Hr(commands->Reset(allocator.Get(),nullptr),"reset state list");};
    {
        D3D12StateRecording states(*context.states);
        Hr(states.Use(*commands.Get(),*array.Get(),D3D12_RESOURCE_STATE_COPY_DEST,"ArrayCopy",3,1),"last subresource transition");
        Hr(states.Use(*commands.Get(),*array.Get(),D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE,"ArrayRead",0,1),"first subresource transition");
        Check(context.states->Find(*array.Get())->states[3]==D3D12_RESOURCE_STATE_COMMON,"recording does not publish state");
        Check(states.PendingTransitions()==2,"two exact subresource barriers");execute(states);
    }
    const auto partial=context.states->Find(*array.Get());
    Check(partial->states[0]==D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE&&partial->states[1]==D3D12_RESOURCE_STATE_COMMON
        &&partial->states[2]==D3D12_RESOURCE_STATE_COMMON&&partial->states[3]==D3D12_RESOURCE_STATE_COPY_DEST,"only declared subresources changed");
    reset();const auto submissions=context.states->Stats().submissions;
    {
        D3D12StateRecording cancelled(*context.states);
        Hr(cancelled.Use(*commands.Get(),*array.Get(),D3D12_RESOURCE_STATE_COPY_DEST,"Cancel",2,1),"candidate cancellation");
        Check(FAILED(cancelled.Use(*commands.Get(),*array.Get(),D3D12_RESOURCE_STATE_COPY_DEST,"BadRange",4,1)),"range rejected");
        Check(FAILED(cancelled.Use(*commands.Get(),*array.Get(),D3D12_RESOURCE_STATE_INDEX_BUFFER,"BadUsage",0,1)),"texture cannot become index buffer");
        Hr(commands->Close(),"discard unsubmitted list");
    }
    Check(context.states->Stats().submissions==submissions&&partial->states[2]==D3D12_RESOURCE_STATE_COMMON,"cancelled state rolled back");
    reset();
    {
        D3D12StateRecording states(*context.states);
        Hr(states.Use(*commands.Get(),*array.Get(),D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE,"AllRead"),"mixed to uniform all-subresource use");
        Check(states.PendingTransitions()==3,"already-readable subresource has no extra barrier");
        Hr(states.Use(*commands.Get(),*array.Get(),D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE,"RepeatRead"),"duplicate read");
        Check(states.PendingTransitions()==3,"duplicate read emits zero barrier");execute(states);
    }
    D3D12_RESOURCE_DESC buffer{};buffer.Dimension=D3D12_RESOURCE_DIMENSION_BUFFER;buffer.Width=64;
    buffer.Height=buffer.DepthOrArraySize=buffer.MipLevels=1;buffer.SampleDesc={1,0};buffer.Layout=D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
    auto gpu=Make(*context.device.Get(),buffer,D3D12_HEAP_TYPE_DEFAULT,D3D12_RESOURCE_STATE_COMMON);
    auto upload=Make(*context.device.Get(),buffer,D3D12_HEAP_TYPE_UPLOAD,D3D12_RESOURCE_STATE_GENERIC_READ);
    Hr(context.states->Register(*gpu.Get(),D3D12_RESOURCE_STATE_COMMON,D3D12TrackedHeap::Default,"DecayBuffer"),"default buffer registration");
    Hr(context.states->Register(*upload.Get(),D3D12_RESOURCE_STATE_GENERIC_READ,D3D12TrackedHeap::Upload,"FixedUpload"),"upload registration");
    reset();
    {
        D3D12StateRecording states(*context.states);
        Hr(states.Use(*commands.Get(),*gpu.Get(),D3D12_RESOURCE_STATE_VERTEX_AND_CONSTANT_BUFFER,"VertexRead"),"default read transition");
        Hr(states.Use(*commands.Get(),*upload.Get(),D3D12_RESOURCE_STATE_COPY_SOURCE,"CopySource"),"fixed upload compatible read");
        Check(states.PendingTransitions()==1,"GENERIC_READ accepts copy-source without transition");
        Check(FAILED(states.Use(*commands.Get(),*upload.Get(),D3D12_RESOURCE_STATE_COPY_DEST,"BadUploadWrite")),"UPLOAD write state rejected");execute(states);
    }
    Check(context.states->Find(*gpu.Get())->states[0]==D3D12_RESOURCE_STATE_COMMON
        &&context.states->Find(*upload.Get())->states[0]==D3D12_RESOURCE_STATE_GENERIC_READ,"default buffer decay and fixed UPLOAD state");
    Check(context.states->Stats().bufferDecays>0&&!context.states->Events().empty(),"state audit and decay statistics");
    {
        buffer.Flags=D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS;
        auto uav=Make(*context.device.Get(),buffer,D3D12_HEAP_TYPE_DEFAULT,D3D12_RESOURCE_STATE_COMMON);
        Hr(context.states->Register(*uav.Get(),D3D12_RESOURCE_STATE_COMMON,D3D12TrackedHeap::Default,"UavDependency"),"UAV registration");
        const auto before=context.states->Stats().uavBarriers;reset();
        {
            D3D12StateRecording states(*context.states);
            Check(FAILED(states.UavBarrier(*commands.Get(),*uav.Get(),"UndeclaredUav")),"UAV dependency requires local declaration");
            Check(FAILED(states.Use(*commands.Get(),*gpu.Get(),D3D12_RESOURCE_STATE_UNORDERED_ACCESS,"MissingUavFlag")),"UAV flag required");
            Check(FAILED(states.Use(*commands.Get(),*upload.Get(),D3D12_RESOURCE_STATE_UNORDERED_ACCESS,"UploadUav")),"UPLOAD cannot be UAV");
            Check(FAILED(states.Use(*commands.Get(),*uav.Get(),static_cast<D3D12_RESOURCE_STATES>(D3D12_RESOURCE_STATE_UNORDERED_ACCESS|D3D12_RESOURCE_STATE_COPY_SOURCE),"MixedUav")),"UAV write state is exclusive");
            Hr(states.Use(*commands.Get(),*uav.Get(),D3D12_RESOURCE_STATE_UNORDERED_ACCESS,"UavFirst"),"UAV transition");
            Hr(states.Use(*commands.Get(),*uav.Get(),D3D12_RESOURCE_STATE_UNORDERED_ACCESS,"UavSecond"),"UAV same state");
            Hr(states.UavBarrier(*commands.Get(),*uav.Get(),"UavDependency"),"explicit same-state dependency");
            Check(states.PendingTransitions()==1&&states.PendingUavBarriers()==1&&states.PendingActivations()==0,"barrier kinds counted independently");
            execute(states);Check(FAILED(states.UavBarrier(*commands.Get(),*uav.Get(),"Committed")),"committed UAV recording rejected");
        }
        Check(context.states->Stats().uavBarriers==before+1&&context.states->Find(*uav.Get())->states[0]==D3D12_RESOURCE_STATE_COMMON,"UAV event committed and buffer decayed");
        reset();
        {
            D3D12StateRecording cancelled(*context.states);
            Hr(cancelled.Use(*commands.Get(),*uav.Get(),D3D12_RESOURCE_STATE_UNORDERED_ACCESS,"CancelledUav"),"candidate UAV state");
            Hr(cancelled.UavBarrier(*commands.Get(),*uav.Get(),"CancelledDependency"),"candidate UAV dependency");
            Hr(commands->Close(),"discard UAV recording");
        }
        Check(context.states->Stats().uavBarriers==before+1&&context.states->Find(*uav.Get())->states[0]==D3D12_RESOURCE_STATE_COMMON,"cancelled UAV has no published state or event");
    }
    commands.Reset();allocator.Reset();gpu.Reset();upload.Reset();array.Reset();context.states->Collect();
    Check(!partial->attached.load()&&context.states->Stats().resources==base,"metadata snapshot does not keep native resource alive");
    ComPtr<ID3D12InfoQueue> info;context.device.As(&info);
    if(info)
    {
        for(UINT64 index=0;index<info->GetNumStoredMessages();++index)
        {
            SIZE_T bytes=0;info->GetMessage(index,nullptr,&bytes);std::vector<std::byte> storage(bytes);
            auto* message=reinterpret_cast<D3D12_MESSAGE*>(storage.data());Hr(info->GetMessage(index,message,&bytes),"native state diagnostic");
            Check(message->Severity>D3D12_MESSAGE_SEVERITY_WARNING,"subresource/decay GPU warning/error/corruption zero");
        }
    }
}
struct Resources {rhi::Buffer vertices,indices;rhi::Texture texture;rhi::Pipeline pipeline;};
Resources Create(rhi::Device& device)
{
    const auto cube=se::MakeUnitCube();std::vector<rhi::Vertex> vertices;
    for(const auto& vertex:cube.vertices)vertices.push_back({vertex.position,vertex.normal,vertex.uv,vertex.tangent});
    Resources resources;Ok(device.CreateBuffer({rhi::BufferUsage::Vertex,sizeof(rhi::Vertex),"Gate.vertices"},std::as_bytes(std::span(vertices)),resources.vertices),"gate vertex");
    Ok(device.CreateBuffer({rhi::BufferUsage::Index,sizeof(std::uint32_t),"Gate.indices"},std::as_bytes(std::span(cube.indices)),resources.indices),"gate index");
    const std::array<std::uint8_t,4> white{255,255,255,255};Ok(device.CreateTexture({1,1},white,resources.texture),"gate texture");
    Ok(device.CreatePipeline({rhi::PipelineKind::TexturedMesh,GetExecutableDirectory()/"Resources/Shaders/TexturedMesh.hlsl",1},resources.pipeline),"gate pipeline");return resources;
}
void Record(rhi::CommandContext& commands,std::span<const rhi::DrawIndexed> draws)
{
    Ok(commands.BeginPass({rhi::PassKind::Clear}),"lifetime clear");Ok(commands.EndPass(),"lifetime clear end");
    Ok(commands.BeginPass({rhi::PassKind::Opaque}),"lifetime opaque");for(const auto& draw:draws)Ok(commands.Draw(draw),"lifetime draw");
    Ok(commands.EndPass(),"lifetime opaque end");Ok(commands.Close(),"lifetime close");
}
void LifetimeAndScenes(const fs::path& samples,const fs::path& output,const Localization& text)
{
    WindowFixture window;window.Size(320,240);rhi::Result result;
    auto device=rhi::Device::CreateD3D12({window.window.hwnd,320,240},text,result);Ok(result,"lifetime device");
    struct DeviceGuard{std::shared_ptr<rhi::Device> device;~DeviceGuard(){device->Shutdown();}} deviceGuard{device};
    const auto base=device->Stats().trackedResources;auto resources=Create(*device);
    const auto vbLife=rhi::D3D12DiagnosticsAccess::Lifetime(*device,*resources.vertices);
    const auto texLife=rhi::D3D12DiagnosticsAccess::Lifetime(*device,*resources.texture);
    const auto psoLife=rhi::D3D12DiagnosticsAccess::Lifetime(*device,*resources.pipeline);
    const auto registry=rhi::D3D12DiagnosticsAccess::States(*device);
    std::weak_ptr<const D3D12StateRecord> nativeLife=registry->Find(*rhi::D3D12DiagnosticsAccess::Resource(*device,*resources.vertices));
    const auto oldPipeline=*resources.pipeline;
    const auto projection=*se::PerspectiveRH(se::Pi/3,4.0f/3,0.1f,100)*se::Translation({0,0,-3});
    const rhi::DrawIndexed draw{*resources.vertices,*resources.indices,*resources.texture,*resources.pipeline,{},projection,{1,1,1,1},false,true};
    auto commands=device->BeginCommands();Record(*commands,std::span(&draw,1));
    ComPtr<ID3D12Fence> gate;Hr(rhi::D3D12DiagnosticsAccess::NativeDevice(*device)->CreateFence(0,D3D12_FENCE_FLAG_NONE,IID_PPV_ARGS(gate.GetAddressOf())),"gate fence");
    struct GateGuard
    {
        ComPtr<ID3D12Fence> fence;HANDLE event;std::atomic<bool> timedOut{false};std::thread watchdog;
        explicit GateGuard(ComPtr<ID3D12Fence> value):fence(std::move(value)),event(CreateEventW(nullptr,TRUE,FALSE,nullptr))
        {
            Check(event!=nullptr,"gate cancellation event");watchdog=std::thread([this]{if(WaitForSingleObject(event,3000)==WAIT_TIMEOUT){timedOut=true;fence->Signal(1);}});
        }
        void Release(){Hr(fence->Signal(1),"release controlled GPU gate");SetEvent(event);}
        ~GateGuard(){fence->Signal(1);SetEvent(event);if(watchdog.joinable())watchdog.join();CloseHandle(event);}
    } gateGuard{gate};
    Hr(rhi::D3D12DiagnosticsAccess::Queue(*device)->Wait(gate.Get(),1),"queue waits on unsignaled gate");
    rhi::CompletionToken token;Ok(device->Submit(*commands,token),"submit behind gate");
    Check(!gateGuard.timedOut&&!device->IsComplete(token),"GPU submission demonstrably incomplete");
    resources={};device->Collect();
    Check(!vbLife.expired()&&!texLife.expired()&&!psoLife.expired()&&!nativeLife.expired()
        &&device->Stats().retiring>=4,"queued GPU and descriptor/pipeline storage survives logical release");
    rhi::Pipeline replacement;Ok(device->CreatePipeline({rhi::PipelineKind::TexturedMesh,GetExecutableDirectory()/"Resources/Shaders/TexturedMesh.hlsl",1},replacement),"reuse pipeline slot during GPU wait");
    Check(replacement->index==oldPipeline.index&&replacement->generation!=oldPipeline.generation&&!psoLife.expired(),"new logical slot does not reclaim old in-flight pipeline");
    gateGuard.Release();Ok(device->WaitIdle(),"gate completion drain");device->Collect();
    Check(vbLife.expired()&&texLife.expired()&&psoLife.expired()&&nativeLife.expired(),"native resource and entry retire only after completion");
    Check(device->Stats().trackedResources==base&&device->Stats().retiring==0,"resource counts return to baseline");
    const auto source=output/"assets"/se::asset_io::NewId();fs::create_directories(source);
    for(const auto& entry:fs::directory_iterator(samples))if(entry.is_regular_file())fs::copy_file(entry.path(),source/entry.path().filename());
    se::AssetStore assets(source,output/"cache");std::vector<se::AssetDiagnostic> diagnostics;se::AssetId glbId,fbxId;
    Check(assets.RegisterSource("Hierarchy.glb",{},glbId,diagnostics)&&assets.RegisterSource("Hierarchy.fbx",{},fbxId,diagnostics),"scene resource registration");
    se::StaticModelRenderCache cache;
    for(int scene=0;scene<12;++scene)
    {
        {
            auto model=cache.Load(assets,scene%2?fbxId:glbId,*device,diagnostics);Check(bool(model),"scene resource upload");
            std::vector<rhi::DrawIndexed> draws;se::AppendStaticModelDraws(*model,{},draws,*replacement,projection,true);
            commands=device->BeginCommands();Record(*commands,draws);Ok(device->Submit(*commands,token),"scene frame submit");
            cache.Prune(nullptr,{});
        }
        const UINT width=scene%2?320:511,height=scene%2?240:257;window.Size(width,height);Ok(device->Resize(width,height),"scene switch resize");
        device->Collect();
        Check(device->Stats().buffers==0&&device->Stats().textures==0&&device->Stats().trackedResources==base,"scene exit and resize release all unreferenced resources");
    }
    auto first=cache.Load(assets,glbId,*device,diagnostics);Check(bool(first),"model before source move");
    fs::create_directories(source/"Moved");fs::rename(source/"Hierarchy.glb",source/"Moved/Hierarchy.glb");
    fs::rename(source/"Hierarchy.glb.asset.json",source/"Moved/Hierarchy.glb.asset.json");fs::copy_file(source/"checker.png",source/"Moved/checker.png");
    Check(assets.Scan(diagnostics),"moved source metadata scan");auto moved=cache.Load(assets,glbId,*device,diagnostics);
    Check(moved&&cache.ModelCount()==1,"stable GPU cache identity survives source path change");
    std::ofstream(source/"Moved/Hierarchy.glb",std::ios::binary)<<"broken";
    Check(cache.Load(assets,glbId,*device,diagnostics)==moved,"moved source reload failure preserves old GPU revision");
    first.reset();moved.reset();cache.Prune(nullptr,{});Ok(device->WaitIdle(),"final scene cleanup");device->Collect();
    Check(device->Stats().trackedResources==base&&device->Stats().buffers==0&&device->Stats().textures==0,"cache owns no permanent unused texture");
    const auto stats=device->Stats();Check(stats.stateTransitions>0&&stats.bufferDecays>0&&stats.debugWarnings==0&&stats.debugErrors==0,"state/lifetime GPU warning/error/corruption zero");
    std::cout<<"Controlled unsignaled Fence preserved resources/pipeline slot; 12 scene/resize cycles returned tracked resources to "<<base<<"; InfoQueue warning/error/corruption=0.\n";
}
}
int wmain(int argc,wchar_t** argv)
{
    try
    {
        Check(argc==3,"supply state test sample/output paths");const auto output=fs::absolute(argv[2]);fs::create_directories(output);
        Localization text;std::string error;Check(text.Load(GetExecutableDirectory()/"Resources/Localization/en-US.txt",error),"state test strings");
        NativeStates(text);LifetimeAndScenes(fs::absolute(argv[1]),output,text);
        std::cout<<"Subresources, cancellation, decay, lifetime gates and scene cache pruning passed.\n";return 0;
    }
    catch(const std::exception& error){std::cerr<<"ResourceStateTests: "<<error.what()<<'\n';return 1;}
}
