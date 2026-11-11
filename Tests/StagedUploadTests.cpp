#include "PlatformLayer/Graphics/D3D12/D3D12Diagnostics.h"
#include "PlatformLayer/Graphics/D3D12/D3D12Presentation.h"
#include "PlatformLayer/Windows/Window.h"
#include "PlatformLayer/Windows/ExecutablePath.h"
#include "ResourceLayer/Assets/Primitives.h"
#include "ResourceLayer/Assets/ImageWriter.h"
#include "CoreLayer/Localization.h"
#include <cstring>
#include <iostream>
#include <limits>
#include <stdexcept>

using namespace se;
namespace fs=std::filesystem;
using Microsoft::WRL::ComPtr;
namespace
{
void Check(bool value,const char* message){if(!value)throw std::runtime_error(message);}
void Ok(rhi::Result result,const char* message){if(!result)throw std::runtime_error(std::string(message)+": "+result.Key());}
void Hr(HRESULT result,const char* message){Check(SUCCEEDED(result),message);}
struct Fixture
{
    WindowState window;std::shared_ptr<rhi::Device> device;
    explicit Fixture(const Localization& text)
    {
        Check(CreateEngineWindow(window,false)&&SetEngineWindowClientSize(window,320,240),"staged hidden surface");int code=0;Check(PumpWindowMessages(code),"surface messages");rhi::Result result;
        device=rhi::Device::CreateD3D12({window.hwnd,320,240},text,result);Ok(result,"staged device");
    }
    ~Fixture(){if(device)device->Shutdown();window.postQuitOnDestroy=false;DestroyEngineWindow(window);}
};
struct Gate
{
    ComPtr<ID3D12Fence> fence;
    explicit Gate(rhi::Device& device)
    {Hr(rhi::D3D12DiagnosticsAccess::NativeDevice(device)->CreateFence(0,D3D12_FENCE_FLAG_NONE,IID_PPV_ARGS(fence.GetAddressOf())),"create controlled gate");Hr(rhi::D3D12DiagnosticsAccess::Queue(device)->Wait(fence.Get(),1),"queue gate");}
    void Open(){Hr(fence->Signal(1),"release controlled gate");}
    ~Gate(){if(fence)fence->Signal(1);}
};
std::vector<std::uint8_t> Pattern(UINT width,UINT height)
{
    std::vector<std::uint8_t> pixels(size_t(width)*height*4);
    for(UINT y=0;y<height;++y)for(UINT x=0;x<width;++x){const auto at=(size_t(y)*width+x)*4;pixels[at]=std::uint8_t(x*19+y*7);pixels[at+1]=std::uint8_t(x*3+y*41);pixels[at+2]=std::uint8_t(x*11+y*13);pixels[at+3]=255;}return pixels;
}
rhi::Image Draw(rhi::Device& device,rhi::BufferHandle vertices,rhi::BufferHandle indices,rhi::TextureHandle texture,rhi::PipelineHandle pipeline)
{
    auto commands=device.BeginCommands();Check(bool(commands),"staged draw commands");Ok(commands->BeginPass({rhi::PassKind::Clear}),"clear");Ok(commands->EndPass(),"clear end");Ok(commands->BeginPass({rhi::PassKind::Opaque}),"opaque");
    rhi::DrawIndexed draw{vertices,indices,texture,pipeline,Rotation(AxisAngle({0,1,0},0.5f)),*OrthographicRH(2.5f,1.875f,.1f,100)*Translation({0,0,-4}),{1,1,1,1},false,true};
    Ok(commands->Draw(draw),"complete staged draw");Ok(commands->EndPass(),"opaque end");Ok(commands->Close(),"close");rhi::Image image;rhi::CompletionToken token;Ok(device.Submit(*commands,token,&image),"capture actual GPU");return image;
}
void RejectDraw(rhi::Device& device,rhi::BufferHandle vertices,rhi::BufferHandle indices,rhi::TextureHandle texture,rhi::PipelineHandle pipeline)
{
    auto commands=device.BeginCommands();Check(bool(commands),"incomplete validation commands");Ok(commands->BeginPass({rhi::PassKind::Clear}),"validation clear");Ok(commands->EndPass(),"validation clear end");Ok(commands->BeginPass({rhi::PassKind::Opaque}),"validation opaque");
    rhi::DrawIndexed draw{vertices,indices,texture,pipeline};Check(commands->Draw(draw).error==rhi::Error::InvalidState,"incomplete coverage/Fence rejects Draw");
}
std::vector<std::uint8_t> ReadTexture(rhi::Device& device,rhi::TextureHandle texture,const Localization& text)
{
    auto& p=*rhi::D3D12DiagnosticsAccess::Presentation(device);auto* native=rhi::D3D12DiagnosticsAccess::NativeDevice(device);auto* resource=rhi::D3D12DiagnosticsAccess::Resource(device,texture);const auto desc=resource->GetDesc();
    D3D12_PLACED_SUBRESOURCE_FOOTPRINT layout{};UINT64 bytes=0;native->GetCopyableFootprints(&desc,0,1,0,&layout,nullptr,nullptr,&bytes);
    D3D12_RESOURCE_DESC buffer{};buffer.Dimension=D3D12_RESOURCE_DIMENSION_BUFFER;buffer.Width=bytes;buffer.Height=buffer.DepthOrArraySize=buffer.MipLevels=1;buffer.SampleDesc={1,0};buffer.Layout=D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
    D3D12_HEAP_PROPERTIES heap{};heap.Type=D3D12_HEAP_TYPE_READBACK;heap.CreationNodeMask=heap.VisibleNodeMask=1;ComPtr<ID3D12Resource> readback;
    Hr(native->CreateCommittedResource(&heap,D3D12_HEAP_FLAG_NONE,&buffer,D3D12_RESOURCE_STATE_COPY_DEST,nullptr,IID_PPV_ARGS(readback.GetAddressOf())),"readback buffer");
    Hr(p.states->Register(*readback.Get(),D3D12_RESOURCE_STATE_COPY_DEST,D3D12TrackedHeap::Readback,"Staged.TestReadback"),"readback state");Check(BeginD3D12FrameCommands(p,text),"readback commands");D3D12StateRecording states(*p.states);
    Hr(states.Use(*p.commandList.Get(),*resource,D3D12_RESOURCE_STATE_COPY_SOURCE,"Staged.Readback"),"readback source");D3D12_TEXTURE_COPY_LOCATION source{},target{};source.pResource=resource;source.Type=D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;target.pResource=readback.Get();target.Type=D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;target.PlacedFootprint=layout;
    p.commandList->CopyTextureRegion(&target,0,0,0,&source,nullptr);Hr(states.Use(*p.commandList.Get(),*resource,D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE,"Staged.Restore"),"restore sampled texture");Hr(p.commandList->Close(),"close readback");ID3D12CommandList* lists[]{p.commandList.Get()};p.gpuWorkPending=p.untrackedSubmission=true;p.queue->ExecuteCommandLists(1,lists);states.Commit();Check(WaitForD3D12Gpu(p,text),"readback wait");
    void* mapped=nullptr;const D3D12_RANGE range{0,static_cast<SIZE_T>(bytes)};Hr(readback->Map(0,&range,&mapped),"map readback");std::vector<std::uint8_t> result(size_t(desc.Width)*desc.Height*4);
    for(UINT row=0;row<desc.Height;++row)std::memcpy(result.data()+size_t(row)*desc.Width*4,static_cast<const std::byte*>(mapped)+layout.Offset+size_t(row)*layout.Footprint.RowPitch,size_t(desc.Width)*4);
    const D3D12_RANGE noWrite{0,0};readback->Unmap(0,&noWrite);return result;
}
void Case(rhi::Device& device,const Localization& text,UINT width,UINT height,const fs::path& root)
{
    const auto cube=MakeUnitCube();std::vector<rhi::Vertex> vertices;for(const auto& v:cube.vertices)vertices.push_back({v.position,v.normal,v.uv,v.tangent});
    const auto vertexBytes=std::as_bytes(std::span(vertices)),indexBytes=std::as_bytes(std::span(cube.indices));auto pixels=Pattern(width,height);const auto expected=pixels;
    rhi::Buffer referenceV,referenceI,stagedV,stagedI;rhi::Texture referenceT,stagedT;rhi::Pipeline pipeline;
    Ok(device.CreatePipeline({rhi::PipelineKind::TexturedMesh,GetExecutableDirectory()/"Resources/Shaders/TexturedMesh.hlsl",1},pipeline),"staged pipeline");
    Ok(device.CreateBuffer({},vertexBytes,referenceV),"reference vertex");Ok(device.CreateBuffer({rhi::BufferUsage::Index,4},indexBytes,referenceI),"reference index");Ok(device.CreateTexture({width,height},pixels,referenceT),"reference whole texture");
    auto reference=Draw(device,*referenceV,*referenceI,*referenceT,*pipeline);const auto before=device.Stats();
    Ok(device.CreateStagedBuffer({},vertexBytes.size(),stagedV),"empty vertex");Ok(device.CreateStagedBuffer({rhi::BufferUsage::Index,4},indexBytes.size(),stagedI),"empty index");Ok(device.CreateStagedTexture({width,height},stagedT),"empty texture");
    RejectDraw(device,*stagedV,*referenceI,*referenceT,*pipeline);RejectDraw(device,*referenceV,*stagedI,*referenceT,*pipeline);RejectDraw(device,*referenceV,*referenceI,*stagedT,*pipeline);
    Check(device.UploadBufferRange(*stagedV,0,vertexBytes).error==rhi::Error::InvalidState,"copy requires explicit batch");
    UINT64 footprintTotal=0,copies=0;const auto chargeBuffer=[&](UINT64 bytes){rhi::UploadFootprint f;Ok(device.QueryBufferUploadFootprint(bytes,f),"buffer footprint");Check(f.stagingBytes==((bytes+511)/512)*512&&f.payloadBytes==bytes&&f.alignment==512,"buffer padded allocation oracle");footprintTotal+=f.stagingBytes;++copies;};
    const auto chargeTexture=[&](UINT rows){rhi::UploadFootprint f;Ok(device.QueryTextureUploadFootprint({width,height},rows,f),"texture footprint");const UINT64 pitch=(UINT64(width)*4+255)/256*256;Check(f.rowPitch==pitch&&f.payloadBytes==UINT64(width)*rows*4&&f.stagingBytes==((pitch*rows+511)/512)*512,"texture footprint oracle");footprintTotal+=f.stagingBytes;++copies;};
    Ok(device.BeginUploadBatch(),"first partial batch");auto invalid=vertices;invalid[0].normal={0,0,0};Check(!device.UploadBufferRange(*stagedV,0,std::as_bytes(std::span(invalid)).first(8*sizeof(rhi::Vertex))),"bad vertex leaves next offset intact");
    Check(!device.UploadBufferRange(*stagedV,sizeof(rhi::Vertex),vertexBytes.first(sizeof(rhi::Vertex)))&&!device.UploadTextureRows(*stagedT,1,1,std::span(pixels).first(size_t(width)*4)),"gaps rejected without committing progress");
    Ok(device.UploadBufferRange(*stagedV,0,vertexBytes.first(8*sizeof(rhi::Vertex))),"first vertex range");chargeBuffer(8*sizeof(rhi::Vertex));
    Ok(device.UploadBufferRange(*stagedI,0,indexBytes.first(12*sizeof(std::uint32_t))),"first index range");chargeBuffer(12*sizeof(std::uint32_t));
    Ok(device.UploadTextureRows(*stagedT,0,1,std::span(pixels).first(size_t(width)*4)),"first texture row");chargeTexture(1);
    rhi::CompletionToken token;Ok(device.EndUploadBatch(token),"first batch end");Ok(device.WaitIdle(),"test drain partial phase");RejectDraw(device,*stagedV,*referenceI,*referenceT,*pipeline);
    Ok(device.BeginUploadBatch(),"remaining geometry batch");const auto vo=8*sizeof(rhi::Vertex),io=12*sizeof(std::uint32_t);
    Check(!device.UploadBufferRange(*stagedV,0,vertexBytes.first(vo)),"overlap rejected");Ok(device.UploadBufferRange(*stagedV,vo,vertexBytes.subspan(vo)),"remaining vertices");chargeBuffer(vertexBytes.size()-vo);
    Ok(device.UploadBufferRange(*stagedI,io,indexBytes.subspan(io)),"remaining indices");chargeBuffer(indexBytes.size()-io);Ok(device.EndUploadBatch(token),"geometry batch end");Ok(device.WaitIdle(),"test geometry drain");
    UINT row=1;while(row<height)
    {
        const UINT rows=std::min(64u,height-row);const bool last=row+rows==height;std::unique_ptr<Gate> gate;if(last)gate=std::make_unique<Gate>(device);
        Ok(device.BeginUploadBatch(),"row batch");auto input=std::vector<std::uint8_t>(pixels.begin()+size_t(row)*width*4,pixels.begin()+size_t(row+rows)*width*4);
        Ok(device.UploadTextureRows(*stagedT,row,rows,input),"next texture rows");chargeTexture(rows);std::fill(input.begin(),input.end(),0);Ok(device.EndUploadBatch(token),"row batch end");
        if(gate){Check(!device.IsComplete(token),"actual copy Fence remains behind queue gate");RejectDraw(device,*stagedV,*stagedI,*stagedT,*pipeline);gate->Open();}
        Ok(device.WaitIdle(),"test row drain");row+=rows;
    }
    Check(ReadTexture(device,*stagedT,text)==expected,"all GPU texture texels match independent CPU source");
    auto staged=Draw(device,*stagedV,*stagedI,*stagedT,*pipeline);Check(staged.width==reference.width&&staged.height==reference.height&&staged.rgba8==reference.rgba8,"whole image equals reference uploads");
    Ok(device.BeginUploadBatch(),"completed immutability validation");Check(!device.UploadBufferRange(*stagedI,0,indexBytes)&&!device.UploadTextureRows(*stagedT,0,1,std::span(pixels).first(size_t(width)*4)),"completed objects are immutable");Ok(device.EndUploadBatch(token),"validation end");Ok(device.WaitIdle(),"validation drain");
    const auto after=device.Stats();Check(after.stagedCopies-before.stagedCopies==copies&&after.stagedPayloadBytes-before.stagedPayloadBytes==vertexBytes.size()+indexBytes.size()+pixels.size()&&after.stagedFootprintBytes-before.stagedFootprintBytes==footprintTotal,"actual staged accounting");
    std::vector<AssetDiagnostic> diagnostics;Check(WritePpmImage(root/(std::to_string(width)+"-reference.ppm"),reference.width,reference.height,reference.rgba8,diagnostics)&&WritePpmImage(root/(std::to_string(width)+"-staged.ppm"),staged.width,staged.height,staged.rgba8,diagnostics),"save image evidence");
}
void Cancelled(rhi::Device& device)
{
    device.Collect();
    const auto base=device.Stats();rhi::Texture texture;Ok(device.CreateStagedTexture({7,5},texture),"cancelled candidate");auto weak=rhi::D3D12DiagnosticsAccess::Lifetime(device,*texture);auto state=rhi::D3D12DiagnosticsAccess::States(device)->Find(*rhi::D3D12DiagnosticsAccess::Resource(device,*texture));std::weak_ptr<const D3D12StateRecord> physical=state;state.reset();
    Gate gate(device);Ok(device.BeginUploadBatch(),"cancelled copy begin");const auto pixels=Pattern(7,2);Ok(device.UploadTextureRows(*texture,0,2,pixels),"cancelled partial rows");texture.reset();rhi::CompletionToken token;Ok(device.EndUploadBatch(token),"cancelled copy submit");device.Collect();
    std::cout<<"cancel gate: complete="<<device.IsComplete(token)<<", entryExpired="<<weak.expired()<<", physicalExpired="<<physical.expired()<<", used="<<device.Stats().memoryUsedBytes<<", baseline="<<base.memoryUsedBytes<<'\n';
    Check(!device.IsComplete(token)&&!weak.expired()&&!physical.expired()&&device.Stats().memoryUsedBytes>base.memoryUsedBytes,"logical cancellation retains actual in-flight backing");gate.Open();Ok(device.WaitIdle(),"cancelled copy complete");device.Collect();
    Check(weak.expired()&&physical.expired()&&device.Stats().memoryUsedBytes==base.memoryUsedBytes,"candidate retires after real Fence");
}
void BufferFence(rhi::Device& device)
{
    const auto cube=MakeUnitCube();std::vector<rhi::Vertex> vertices;for(const auto& v:cube.vertices)vertices.push_back({v.position,v.normal,v.uv,v.tangent});
    const auto vb=std::as_bytes(std::span(vertices)),ib=std::as_bytes(std::span(cube.indices));rhi::Buffer ordinaryV,ordinaryI,stagedV,stagedI;rhi::Texture texture;rhi::Pipeline pipeline;
    Ok(device.CreateBuffer({},vb,ordinaryV),"buffer-fence reference vertex");Ok(device.CreateBuffer({rhi::BufferUsage::Index,4},ib,ordinaryI),"buffer-fence reference index");
    const auto pixels=Pattern(3,2);Ok(device.CreateTexture({3,2},pixels,texture),"buffer-fence reference texture");Ok(device.CreatePipeline({rhi::PipelineKind::TexturedMesh,GetExecutableDirectory()/"Resources/Shaders/TexturedMesh.hlsl",1},pipeline),"buffer-fence pipeline");
    const auto reference=Draw(device,*ordinaryV,*ordinaryI,*texture,*pipeline);Ok(device.CreateStagedBuffer({},vb.size(),stagedV),"gated vertex");Ok(device.CreateStagedBuffer({rhi::BufferUsage::Index,4},ib.size(),stagedI),"gated index");
    Gate gate(device);Ok(device.BeginUploadBatch(),"gated complete buffer copies");Ok(device.UploadBufferRange(*stagedV,0,vb),"gated whole vertex");Ok(device.UploadBufferRange(*stagedI,0,ib),"gated whole index");rhi::CompletionToken token;Ok(device.EndUploadBatch(token),"gated buffer end");
    Check(!device.IsComplete(token),"complete buffer coverage still waits for actual Fence");RejectDraw(device,*stagedV,*ordinaryI,*texture,*pipeline);RejectDraw(device,*ordinaryV,*stagedI,*texture,*pipeline);
    gate.Open();Ok(device.WaitIdle(),"buffer-fence test drain");Check(Draw(device,*stagedV,*stagedI,*texture,*pipeline).rgba8==reference.rgba8,"gated buffers draw correctly after Fence");
}
}
int wmain(int argc,wchar_t** argv)
{
    try
    {
        Localization text;std::string error;Check(text.Load(GetExecutableDirectory()/"Resources/Localization/en-US.txt",error),"staged localization");const fs::path root=argc>1?argv[1]:L"out/validation/staged";fs::create_directories(root);Fixture fixture(text);
        for(const auto size:{std::pair{1u,1u},std::pair{3u,2u},std::pair{7u,5u},std::pair{257u,193u},std::pair{1025u,5u}})Case(*fixture.device,text,size.first,size.second,root);
        BufferFence(*fixture.device);Cancelled(*fixture.device);const auto stats=fixture.device->Stats();Check(!stats.debugWarnings&&!stats.debugErrors,"staged GPU warning/error zero");
        std::cout<<"Staged GPU buffer/row copies, aligned footprints, complete texel/image reference, queue-gated readiness and retirement passed\n";return 0;
    }
    catch(const std::exception& error){std::cerr<<error.what()<<'\n';return 1;}
}
