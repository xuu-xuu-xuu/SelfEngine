#include "FunctionLayer/Rendering/AssetUploadScheduler.h"
#include "PlatformLayer/Graphics/D3D12/D3D12Diagnostics.h"
#include "PlatformLayer/Graphics/D3D12/D3D12Presentation.h"
#include "PlatformLayer/Windows/Window.h"
#include "PlatformLayer/Windows/ExecutablePath.h"
#include "ResourceLayer/Assets/AssetIO.h"
#include "ResourceLayer/Assets/ImageWriter.h"
#include "ResourceLayer/Assets/Primitives.h"
#include "CoreLayer/Localization.h"
#include <nlohmann/json.hpp>
#include <algorithm>
#include <iostream>
#include <stdexcept>

using namespace se;
using namespace std::chrono_literals;
namespace fs=std::filesystem;
using Microsoft::WRL::ComPtr;
namespace
{
void Check(bool value,const char* text){if(!value)throw std::runtime_error(text);}
void Ok(rhi::Result value,const char* text){if(!value)throw std::runtime_error(std::string(text)+": "+value.Key());}
struct Fixture
{
    WindowState window;std::shared_ptr<rhi::Device> device;rhi::Buffer vertices,indices;rhi::Texture fallback;rhi::Pipeline pipeline;
    explicit Fixture(const Localization& text)
    {
        Check(CreateEngineWindow(window,false)&&SetEngineWindowClientSize(window,320,240),"asset upload surface");int exit=0;Check(PumpWindowMessages(exit),"window messages");rhi::Result result;device=rhi::Device::CreateD3D12({window.hwnd,320,240},text,result);Ok(result,"device");
        const auto cube=MakeUnitCube();std::vector<rhi::Vertex> v;for(const auto& value:cube.vertices)v.push_back({value.position,value.normal,value.uv,value.tangent});
        Ok(device->CreateBuffer({},std::as_bytes(std::span(v)),vertices),"warm scene vertex");Ok(device->CreateBuffer({rhi::BufferUsage::Index,4},std::as_bytes(std::span(cube.indices)),indices),"warm scene index");
        const auto& image=DefaultTextureImage(DefaultTextureKind::Fallback);Ok(device->CreateTexture({image.width,image.height},image.rgba8,fallback),"placeholder");Ok(device->CreatePipeline({rhi::PipelineKind::TexturedMesh,GetExecutableDirectory()/"Resources/Shaders/TexturedMesh.hlsl",1},pipeline),"scene pipeline");
    }
    ~Fixture(){if(device)device->Shutdown();window.postQuitOnDestroy=false;DestroyEngineWindow(window);}
    rhi::Image Draw(rhi::TextureHandle texture,bool capture)
    {
        auto commands=device->BeginCommands();Check(bool(commands),"running scene commands");Ok(commands->BeginPass({rhi::PassKind::Clear}),"clear");Ok(commands->EndPass(),"clear end");Ok(commands->BeginPass({rhi::PassKind::Opaque}),"opaque");
        rhi::DrawIndexed draw{*vertices,*indices,texture,*pipeline,Rotation(AxisAngle({0,1,0},.4f)),*OrthographicRH(2.5f,1.875f,.1f,100)*Translation({0,0,-4}),{1,1,1,1},false,true};
        Ok(commands->Draw(draw),"draw active scene");Ok(commands->EndPass(),"opaque end");Ok(commands->Close(),"close scene");rhi::CompletionToken token;rhi::Image image;Ok(device->Submit(*commands,token,capture?&image:nullptr),"present active scene");return image;
    }
};
void WriteBmp(const fs::path& path,UINT width,UINT height)
{
    const size_t pitch=(size_t(width)*3+3)/4*4;std::vector<std::uint8_t> bytes(54+pitch*height);
    const auto u16=[&](size_t at,UINT value){bytes[at]=std::uint8_t(value);bytes[at+1]=std::uint8_t(value>>8);};const auto u32=[&](size_t at,UINT value){for(UINT i=0;i<4;++i)bytes[at+i]=std::uint8_t(value>>(i*8));};
    bytes[0]='B';bytes[1]='M';u32(2,static_cast<UINT>(bytes.size()));u32(10,54);u32(14,40);u32(18,width);u32(22,height);u16(26,1);u16(28,24);
    for(UINT y=0;y<height;++y)for(UINT x=0;x<width;++x){const auto at=54+size_t(y)*pitch+x*3;bytes[at]=std::uint8_t(x*7+y*11);bytes[at+1]=std::uint8_t(y*19+x);bytes[at+2]=std::uint8_t(x*31+y*3);}
    Check(asset_io::WriteAtomic(path,bytes),"write large real BMP");
}
void CpuQueued(AsyncAssetLoader& loader,TaskQueue& tasks,const AssetTicket& ticket)
{
    for(unsigned i=0;i<32;++i){loader.Pump();loader.Pump();if(loader.Query(ticket).state==AssetLoadState::UploadQueued)return;Check(loader.Query(ticket).state!=AssetLoadState::Failed,"CPU candidate failed");tasks.WaitForReady(2s);}throw std::runtime_error("CPU candidate timeout");
}
void ImageBudget(Fixture& fixture,AssetStore& assets,const AssetId& id,AssetUploadBudget budget,const fs::path& root,bool fits)
{
    auto& device=*fixture.device;TaskQueue tasks(2,16);AsyncAssetLoader loader(assets,tasks);AssetUploadScheduler uploads(device,loader,budget);auto ticket=loader.Request(id,AsyncAssetKind::Image,AssetTarget::Renderable);
    std::vector<AssetDiagnostic> diagnostics;auto cpu=assets.LoadTextureImage(id,diagnostics);Check(bool(cpu),"synchronous reference decode");rhi::Texture reference;Ok(device.CreateTexture({cpu->width,cpu->height},cpu->rgba8,reference),"reference whole image");const auto image=fixture.Draw(*reference,true);
    nlohmann::json rows=nlohmann::json::array();bool done=false;std::uint64_t payload=0;const auto framesBefore=device.Stats().submittedFrames;
    for(unsigned frame=0;frame<2048;++frame)
    {
        loader.Pump();const auto before=device.Stats();AssetUploadStep step;Ok(uploads.Tick(step),"budgeted tick");const auto after=device.Stats();
        Check(step.footprintBytes<=budget.bytes&&step.tasks<=budget.tasks&&after.stagedFootprintBytes-before.stagedFootprintBytes==step.footprintBytes&&after.stagedCopies-before.stagedCopies==step.tasks,"actual RHI copy/footprint hard limits");
        if(step.submitted){const auto& presentation=*rhi::D3D12DiagnosticsAccess::Presentation(device);const auto index=(presentation.frameCursor+presentation.frameCount-1)%presentation.frameCount;Check(presentation.frames[index].uploads.Stats().usedBytes==step.footprintBytes,"actual frame arena usage agrees with charged footprint");}
        Check(after.globalWaits==before.globalWaits,"upload scheduler does not globally drain");payload+=step.payloadBytes;
        auto view=loader.Query(ticket);auto gpu=std::dynamic_pointer_cast<const RenderImageAsset>(view.gpu);fixture.Draw(gpu?*gpu->texture:*fixture.fallback,false);
        rows.push_back({{"frame",frame},{"state",AssetLoadStateKey(view.state)},{"payload",step.payloadBytes},{"footprint",step.footprintBytes},{"tasks",step.tasks},{"completed",step.completed}});
        if(view.state==AssetLoadState::Ready)
        {
            Check(fits&&gpu&&payload==cpu->rgba8.size(),"only fully uploaded image reaches Ready");const auto actual=fixture.Draw(*gpu->texture,true);Check(actual.rgba8==image.rgba8,"budgeted final image matches whole upload");
            Check(WritePpmImage(root/(std::to_string(budget.bytes)+"-async.ppm"),actual.width,actual.height,actual.rgba8,diagnostics),"save asynchronous image");done=true;break;
        }
        if(view.state==AssetLoadState::Failed)
        {
            Check(!fits&&payload==0&&std::any_of(view.diagnostics.begin(),view.diagnostics.end(),[](const auto& d){return d.key=="Assets.UploadBudgetTooSmall";}),"too-small minimum slice fails explicitly");done=true;break;
        }
        if(tasks.Pending())tasks.WaitForReady(1ms);
    }
    Check(done&&device.Stats().submittedFrames>framesBefore,"scene continues to submit during loading");
    const auto json=rows.dump(2);Check(asset_io::WriteAtomic(root/(std::to_string(budget.bytes)+"-frames.json"),{reinterpret_cast<const std::uint8_t*>(json.data()),json.size()}),"budget frame evidence");
    Check(!uploads.SetBudget({0,1})&&!uploads.SetBudget({512,0}),"invalid budget preserves configuration");
}
struct Gate
{
    ComPtr<ID3D12Fence> fence;
    explicit Gate(rhi::Device& device){Check(SUCCEEDED(rhi::D3D12DiagnosticsAccess::NativeDevice(device)->CreateFence(0,D3D12_FENCE_FLAG_NONE,IID_PPV_ARGS(fence.GetAddressOf()))),"loading gate");Check(SUCCEEDED(rhi::D3D12DiagnosticsAccess::Queue(device)->Wait(fence.Get(),1)),"loading queue gate");}
    void Open(){Check(SUCCEEDED(fence->Signal(1)),"open loading gate");}
    ~Gate(){if(fence)fence->Signal(1);}
};
void CancelAndReplace(Fixture& fixture,AssetStore& assets,const AssetId& id)
{
    auto& device=*fixture.device;device.Collect();TaskQueue tasks(2,16);AsyncAssetLoader loader(assets,tasks);AssetUploadScheduler uploads(device,loader,{1536,1});
    auto old=loader.Request(id,AsyncAssetKind::Image,AssetTarget::Renderable);CpuQueued(loader,tasks,old);const auto oldToken=loader.QueuedUploads()[0].token;const auto base=device.Stats();
    Gate gate(device);AssetUploadStep first;Ok(uploads.Tick(first),"first scene copy");Check(first.tasks==1&&loader.Query(old).state==AssetLoadState::UploadingGPU,"old scene is truly in-flight");
    old.Cancel();loader.Pump();AssetUploadStep cancelled;Ok(uploads.Tick(cancelled),"cancel active scene");Check(cancelled.cancelled==1&&loader.Query(old).state==AssetLoadState::Cancelled&&device.Stats().memoryUsedBytes>base.memoryUsedBytes,"cancel does not free queued physical destination");
    auto replacement=loader.Request(id,AsyncAssetKind::Image,AssetTarget::Renderable);CpuQueued(loader,tasks,replacement);const auto token=loader.QueuedUploads()[0].token;Check(token.operation!=oldToken.operation,"new scene request has new identity");
    AssetUploadStep second;Ok(uploads.Tick(second),"replacement first copy on other frame context");Check(second.tasks==1&&device.Stats().memoryResources>=base.memoryResources+2,"old/new physical resources coexist behind Fence");
    Check(!loader.CompleteUpload({oldToken,false,{},{}})&&loader.Query(replacement).state==AssetLoadState::UploadingGPU,"late old result cannot publish replacement");
    gate.Open();Ok(device.WaitIdle(),"controlled test drain");bool ready=false;
    for(unsigned frame=0;frame<512;++frame){loader.Pump();AssetUploadStep step;Ok(uploads.Tick(step),"replacement progress");Check(step.footprintBytes<=1536&&step.tasks<=1,"replacement budget");auto view=loader.Query(replacement);fixture.Draw(*fixture.fallback,false);if(view.state==AssetLoadState::Ready){ready=true;break;}}
    Check(ready,"replacement reaches Ready after gate release");
}
rhi::Image DrawModel(rhi::Device& device,const StaticRenderModel& model,rhi::PipelineHandle pipeline)
{
    std::vector<rhi::DrawIndexed> draws;const auto clip=*OrthographicRH(5.333333f,4,.1f,100)*Translation({-.5f,-.5f,-4});AppendPbrModelDraws(model,Mat4{}, {},draws,pipeline,clip,{.5f,.5f,4},{.3f,.8f,.5f},{4,4,4});Check(!draws.empty(),"prepared model has parts");
    auto commands=device.BeginCommands();Check(bool(commands),"model commands");Ok(commands->BeginPass({rhi::PassKind::Clear}),"model clear");Ok(commands->EndPass(),"model clear end");Ok(commands->BeginPass({rhi::PassKind::Opaque}),"model opaque");for(const auto& draw:draws)Ok(commands->Draw(draw),"draw budgeted model");Ok(commands->EndPass(),"model opaque end");Ok(commands->Close(),"model close");rhi::Image image;rhi::CompletionToken token;Ok(device.Submit(*commands,token,&image),"model capture");return image;
}
void Model(Fixture& fixture,AssetStore& assets,const AssetId& id,const fs::path& root)
{
    auto& device=*fixture.device;TaskQueue tasks(2,16);AsyncAssetLoader loader(assets,tasks);AssetUploadScheduler uploads(device,loader,{1024,2});auto ticket=loader.Request(id,AsyncAssetKind::Model,AssetTarget::Renderable);bool ready=false;
    for(unsigned frame=0;frame<256;++frame){loader.Pump();AssetUploadStep step;Ok(uploads.Tick(step),"model scheduler");Check(step.footprintBytes<=1024&&step.tasks<=2,"model per-frame limits");fixture.Draw(*fixture.fallback,false);if(loader.Query(ticket).state==AssetLoadState::Ready){ready=true;break;}if(tasks.Pending())tasks.WaitForReady(1ms);}
    auto view=loader.Query(ticket);auto gpu=std::dynamic_pointer_cast<const RenderModelAsset>(view.gpu);Check(ready&&gpu&&gpu->model->source==view.cpu->model,"model lease publishes complete canonical revision");
    std::vector<AssetDiagnostic> diagnostics;auto cpu=assets.LoadModel(id,diagnostics);StaticModelRenderCache reference(true);std::shared_ptr<const StaticRenderModel> ordinary;Check(reference.Upload(cpu,assets.Resolve(id),assets.SourceRoot(),device,ordinary,diagnostics),"whole reference model upload");
    rhi::Pipeline pipeline;Ok(device.CreatePipeline({rhi::PipelineKind::PbrMesh,GetExecutableDirectory()/"Resources/Shaders/Pbr.hlsl",2},pipeline),"model PBR pipeline");auto expected=DrawModel(device,*ordinary,*pipeline),actual=DrawModel(device,*gpu->model,*pipeline);Check(actual.rgba8==expected.rgba8,"PBR model image matches canonical whole upload");
    Check(WritePpmImage(root/"model-async.ppm",actual.width,actual.height,actual.rgba8,diagnostics)&&WritePpmImage(root/"model-reference.ppm",expected.width,expected.height,expected.rgba8,diagnostics),"save model evidence");
    const auto bytes=loader.Stats().cpuBytes;ticket.Cancel();loader.CollectUnused();view={};Check(loader.Stats().cpuBytes==bytes,"GPU model source alias retains CPU revision budget");gpu.reset();device.Collect();Check(loader.Stats().cpuBytes==0,"last model reader releases CPU revision");
}
}
int wmain(int argc,wchar_t** argv)
{
    try
    {
        Check(argc==3,"usage: AssetUploadTests samples output");const auto root=fs::absolute(argv[2])/asset_io::NewId();const auto source=root/"source";fs::create_directories(source);for(const auto& entry:fs::directory_iterator(argv[1]))if(entry.is_regular_file())fs::copy_file(entry.path(),source/entry.path().filename());WriteBmp(source/"large.bmp",257,193);
        AssetStore assets(source,root/"cache");std::vector<AssetDiagnostic> diagnostics;AssetId image,model;Check(assets.RegisterSource("large.bmp",{},image,diagnostics)&&assets.RegisterSource("Hierarchy.glb",{},model,diagnostics),"loading test catalog");
        Localization text;std::string error;Check(text.Load(GetExecutableDirectory()/"Resources/Localization/en-US.txt",error),"loading localization");Fixture fixture(text);
        ImageBudget(fixture,assets,image,{512,1},root,false);ImageBudget(fixture,assets,image,{4096,1},root,true);ImageBudget(fixture,assets,image,{8192,3},root,true);CancelAndReplace(fixture,assets,image);Model(fixture,assets,model,root);
        const auto stats=fixture.device->Stats();Check(!stats.debugErrors&&!stats.debugWarnings,"budgeted uploads GPU warnings/errors zero");std::cout<<"Budgeted real GPU image/model uploads, running-frame hard limits, source leases, cancellation/new scene and true Fence publication passed\n";return 0;
    }
    catch(const std::exception& error){std::cerr<<error.what()<<'\n';return 1;}
}
