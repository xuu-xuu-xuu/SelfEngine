#include "FunctionLayer/Rendering/EnvironmentRender.h"
#include "FunctionLayer/Rendering/ForwardRenderer.h"
#include "FunctionLayer/Rendering/ShadowSystem.h"
#include "FunctionLayer/Rendering/StaticModelRender.h"
#include "PlatformLayer/Graphics/D3D12/D3D12Diagnostics.h"
#include "PlatformLayer/Graphics/D3D12/D3D12Presentation.h"
#include "PlatformLayer/Windows/Window.h"
#include "PlatformLayer/Windows/ExecutablePath.h"
#include "ResourceLayer/Assets/AssetIO.h"
#include "ResourceLayer/Assets/Primitives.h"
#include "Materials/MaterialsDemo.h"
#include "PlatformLayer/InputState.h"
#include "CoreLayer/Localization.h"
#include "CoreLayer/Math/Half.h"
#include <d3d12sdklayers.h>
#include <algorithm>
#include <atomic>
#include <cstring>
#include <fstream>
#include <iostream>
#include <thread>
#include <nlohmann/json.hpp>

using namespace se;
using Microsoft::WRL::ComPtr;
namespace
{
constexpr UINT width=257,height=193;
void Check(bool value,const char* message){if(!value)throw std::runtime_error(message);}
void Hr(HRESULT value,const char* message){Check(SUCCEEDED(value),message);}
void Ok(rhi::Result value,const char* message){if(!value)throw std::runtime_error(std::string(message)+": "+value.Key());}
struct Fixture
{
    WindowState window;std::shared_ptr<rhi::Device> device;
    explicit Fixture(const Localization& text)
    {
        Check(CreateEngineWindow(window,false),"environment hidden window");RECT size{0,0,width,height};
        AdjustWindowRectEx(&size,static_cast<DWORD>(GetWindowLongPtrW(window.hwnd,GWL_STYLE)),FALSE,0);
        Check(SetWindowPos(window.hwnd,nullptr,0,0,size.right-size.left,size.bottom-size.top,SWP_NOMOVE|SWP_NOZORDER|SWP_NOACTIVATE)!=FALSE,"environment odd surface");
        int code=0;Check(PumpWindowMessages(code),"environment window messages");rhi::Result result;
        device=rhi::Device::CreateD3D12({window.hwnd,width,height},text,result);Ok(result,"environment device");
    }
    ~Fixture(){if(device)device->Shutdown();window.postQuitOnDestroy=false;DestroyEngineWindow(window);}
};
void Save(const rhi::Image& image,const std::filesystem::path& path)
{
    std::ofstream stream(path,std::ios::binary);stream<<"P6\n"<<image.width<<' '<<image.height<<"\n255\n";
    for(size_t i=0;i<image.rgba8.size();i+=4)stream.write(reinterpret_cast<const char*>(image.rgba8.data()+i),3);Check(bool(stream),"save actual IBL image");
}
std::vector<std::vector<std::uint8_t>> ReadAll(rhi::Device& device,rhi::TextureHandle handle,const Localization& text)
{
    auto& p=*rhi::D3D12DiagnosticsAccess::Presentation(device);auto* native=rhi::D3D12DiagnosticsAccess::NativeDevice(device);
    auto* texture=rhi::D3D12DiagnosticsAccess::Resource(device,handle);const auto desc=texture->GetDesc();const UINT count=UINT(desc.MipLevels)*desc.DepthOrArraySize;
    std::vector<D3D12_PLACED_SUBRESOURCE_FOOTPRINT> layouts(count);std::vector<UINT> rows(count);std::vector<UINT64> rowBytes(count);UINT64 bytes=0;
    native->GetCopyableFootprints(&desc,0,count,0,layouts.data(),rows.data(),rowBytes.data(),&bytes);
    D3D12_RESOURCE_DESC buffer{};buffer.Dimension=D3D12_RESOURCE_DIMENSION_BUFFER;buffer.Width=bytes;
    buffer.Height=buffer.DepthOrArraySize=buffer.MipLevels=1;buffer.SampleDesc={1,0};buffer.Layout=D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
    D3D12_HEAP_PROPERTIES heap{};heap.Type=D3D12_HEAP_TYPE_READBACK;heap.CreationNodeMask=heap.VisibleNodeMask=1;ComPtr<ID3D12Resource> readback;
    Hr(native->CreateCommittedResource(&heap,D3D12_HEAP_FLAG_NONE,&buffer,D3D12_RESOURCE_STATE_COPY_DEST,nullptr,IID_PPV_ARGS(readback.GetAddressOf())),"all-mip readback");
    Hr(p.states->Register(*readback.Get(),D3D12_RESOURCE_STATE_COPY_DEST,D3D12TrackedHeap::Readback,"EnvironmentTest.Readback"),"readback registry");
    Check(BeginD3D12FrameCommands(p,text),"all-mip commands");D3D12StateRecording states(*p.states);
    Hr(states.Use(*p.commandList.Get(),*texture,D3D12_RESOURCE_STATE_COPY_SOURCE,"EnvironmentTest"),"all-mip copy state");
    for(UINT index=0;index<count;++index)
    {
        D3D12_TEXTURE_COPY_LOCATION source{};source.pResource=texture;source.Type=D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;source.SubresourceIndex=index;
        D3D12_TEXTURE_COPY_LOCATION target{};target.pResource=readback.Get();target.Type=D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;target.PlacedFootprint=layouts[index];
        p.commandList->CopyTextureRegion(&target,0,0,0,&source,nullptr);
    }
    Hr(states.Use(*p.commandList.Get(),*texture,D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE,"EnvironmentTest.Restore"),"all-mip restore");
    Hr(p.commandList->Close(),"all-mip close");ID3D12CommandList* lists[]{p.commandList.Get()};p.gpuWorkPending=p.untrackedSubmission=true;p.queue->ExecuteCommandLists(1,lists);states.Commit();
    Check(WaitForD3D12Gpu(p,text),"all-mip completion");void* mapped=nullptr;const D3D12_RANGE range{0,static_cast<SIZE_T>(bytes)};Hr(readback->Map(0,&range,&mapped),"all-mip map");
    std::vector<std::vector<std::uint8_t>> output(count);
    for(UINT index=0;index<count;++index)
    {auto& data=output[index];data.resize(static_cast<size_t>(rowBytes[index])*rows[index]);for(UINT row=0;row<rows[index];++row)
        std::memcpy(data.data()+size_t(row)*rowBytes[index],static_cast<const std::byte*>(mapped)+layouts[index].Offset+size_t(row)*layouts[index].Footprint.RowPitch,static_cast<size_t>(rowBytes[index]));}
    const D3D12_RANGE noWrites{0,0};readback->Unmap(0,&noWrites);return output;
}
void Uploads(rhi::Device& device,const Localization& text)
{
    for(const bool cube:{false,true})
    {
        const rhi::TextureDescription description{cube?8u:7u,cube?8u:3u,cube?4u:3u,1,cube?rhi::TextureFormat::Rgba16Float:rhi::TextureFormat::Rgba8,"AllSubresources",cube?6u:2u,cube};
        std::vector<std::vector<std::uint8_t>> storage,expected;
        std::vector<rhi::TextureSubresource> inputs;const UINT bpp=cube?8:4;
        for(UINT layer=0;layer<description.arrayLayers;++layer)for(UINT mip=0;mip<description.mipLevels;++mip)
        {
            const auto w=std::max(1u,description.width>>mip),h=std::max(1u,description.height>>mip),pitch=w*bpp+13;
            storage.emplace_back(size_t(pitch)*h,0xcd);expected.emplace_back(size_t(w)*h*bpp);
            auto& padded=storage.back();auto& tight=expected.back();
            for(UINT y=0;y<h;++y)for(UINT x=0;x<w;++x)for(UINT channel=0;channel<4;++channel)
            {
                if(cube){const auto half=FloatToHalf(float(layer*16+mip*4+channel)+float(x)*0.125f+float(y)*0.03125f);
                    std::memcpy(padded.data()+size_t(y)*pitch+x*bpp+channel*2,&half,2);std::memcpy(tight.data()+(size_t(y)*w+x)*bpp+channel*2,&half,2);}
                else {const auto value=static_cast<std::uint8_t>(layer*53+mip*17+x*3+y*7+channel);padded[size_t(y)*pitch+x*bpp+channel]=value;tight[(size_t(y)*w+x)*bpp+channel]=value;}
            }
        }
        for(size_t i=0;i<storage.size();++i)inputs.push_back({storage[i],std::max(1u,description.width>>unsigned(i%description.mipLevels))*bpp+13});
        rhi::Texture texture;Ok(device.CreateTextureSubresources(description,inputs,texture),"padded array/cube upload");
        Check(ReadAll(device,*texture,text)==expected,"every GPU face, mip and odd padded row matches original bytes exactly");
        rhi::Texture rejected;auto invalid=description;invalid.mipLevels=32;
        Check(device.CreateTextureSubresources(invalid,inputs,rejected).error==rhi::Error::InvalidArgument&&!rejected,"excessive mip count rejected atomically");
        invalid=description;invalid.cube=true;invalid.arrayLayers=2;Check(!device.CreateTextureSubresources(invalid,inputs,rejected),"malformed cube shape rejected");
        auto shortRows=inputs;shortRows[0].rowPitch=1;Check(!device.CreateTextureSubresources(description,shortRows,rejected),"short source row pitch rejected");
    }
}
CameraSnapshot Camera(Vec3 direction={0,0,-1},Vec3 position={0,0,3})
{
    const auto z=-Normalize(direction);const auto x=Normalize(Cross(std::abs(z.y)>0.99f?Vec3{0,0,1}:Vec3{0,1,0},z)),y=Cross(z,x);Mat4 world;
    const float axes[4][3]{{x.x,x.y,x.z},{y.x,y.y,y.z},{z.x,z.y,z.z},{position.x,position.y,position.z}};
    for(size_t column=0;column<4;++column)for(size_t row=0;row<3;++row)world(row,column)=axes[column][row];
    CameraSnapshot output;CameraError error;Check(BuildCameraSnapshot({1,world,{Pi/3,0.1f,100,true},true},{0,0,width,height},1,output,error),"environment camera");return output;
}
float Lut(const EnvironmentTexture& texture,float u,float v,size_t channel)
{
    const double x=std::clamp(double(u)*texture.size-0.5,0.0,double(texture.size-1)),y=std::clamp(double(v)*texture.size-0.5,0.0,double(texture.size-1));
    const auto x0=size_t(x),y0=size_t(y),x1=std::min(x0+1,size_t(texture.size-1)),y1=std::min(y0+1,size_t(texture.size-1));
    const auto fetch=[&](size_t a,size_t b){return double(HalfToFloat(texture.subresources[0][(b*texture.size+a)*4+channel]));};
    const auto top=fetch(x0,y0)*(1-(x-x0))+fetch(x1,y0)*(x-x0),bottom=fetch(x0,y1)*(1-(x-x0))+fetch(x1,y1)*(x-x0);return float(top*(1-(y-y0))+bottom*(y-y0));
}
struct Captured {rhi::Image image;rhi::HdrImage hdr;};
void Gpu(Fixture& fixture,const Localization& text,const std::filesystem::path& output)
{
    auto& device=*fixture.device;Uploads(device,text);rhi::Pipeline mesh,depth,final,sky;
    rhi::PipelineDescription description{rhi::PipelineKind::EnvironmentMesh,GetExecutableDirectory()/"Resources/Shaders/EnvironmentForward.hlsl",5};description.colorFormat=rhi::ColorFormat::Rgba16Float;
    Ok(device.CreatePipeline(description,mesh),"IBL mesh PSO");
    description={rhi::PipelineKind::EnvironmentSky,GetExecutableDirectory()/"Resources/Shaders/EnvironmentSky.hlsl",5};description.colorFormat=rhi::ColorFormat::Rgba16Float;Ok(device.CreatePipeline(description,sky),"cube sky PSO");
    Ok(device.CreatePipeline({rhi::PipelineKind::ShadowDepth,GetExecutableDirectory()/"Resources/Shaders/ShadowDepth.hlsl",4},depth),"IBL depth PSO");
    Ok(device.CreatePipeline({rhi::PipelineKind::ShadowOutput,GetExecutableDirectory()/"Resources/Shaders/ShadowOutput.hlsl",4},final),"IBL final PSO");
    Check(device.Stats().shaderArtifactLoads==4&&device.Stats().shaderProgramCompilations==0,"all environment programs load cooked artifacts");
    std::vector<AssetDiagnostic> diagnostics;auto uniform=std::make_shared<EnvironmentAsset>();FloatImage image{16,8,std::vector<Vec3>(128,{1,0.5f,0.25f})};
    Check(BakeEnvironment(image,asset_io::Hash({reinterpret_cast<const std::uint8_t*>("UniformGpu"),10}),{8,4,8,128},*uniform,diagnostics),"uniform offline fixture");
    EnvironmentRenderCache environmentCache;std::shared_ptr<const RenderEnvironment> environment;
    Check(environmentCache.Upload(uniform,device,environment,diagnostics),"uniform environment upload");
    StaticModelRenderCache models{true};auto source=std::make_shared<StaticModel>();source->meshes.push_back(MakeUnitCube());source->meshes[0].material=0;source->materials.emplace_back();ModelNode node;node.name="Plane";node.meshes={0};source->nodes.push_back(node);
    std::shared_ptr<const StaticRenderModel> cube;Check(models.Upload(source,{},std::filesystem::current_path(),device,cube,diagnostics),"IBL primitive");
    auto camera=Camera();std::vector<rhi::DrawIndexed> draws;AppendPbrModelDraws(*cube,Translation({0,0,-0.005f})*Scaling({4,3,0.01f}),{},draws,*mesh,*OrthographicRH(4,3,0.1f,100)*camera.view,camera.position,{0,0,1},{});
    rhi::ForwardLighting lighting;DirectionalShadowView shadowView;shadowView.enabled=false;shadowView.resolution=256;rhi::ShadowSettings shadows;
    Check(PrepareDirectionalShadow(shadowView,lighting,shadows,diagnostics),"IBL shadow prefix");float intensity=1,yaw=0;std::uint64_t frame=0;
    Ok(device.EnableGpuTiming(true),"environment timestamps");
    const auto record=[&]{auto commands=device.BeginCommands(++frame);const auto bindings=PrepareEnvironmentBindings(*environment,camera,intensity,yaw,diagnostics);
        Ok(RecordShadowedFrame(*commands,lighting,shadows,*depth,*final,*sky,draws,draws,&bindings),"record explicit IBL frame");return commands;};
    const auto capture=[&]{auto commands=record();Captured result;rhi::CompletionToken token;Ok(device.Submit(*commands,token,&result.image,&result.hdr),"actual IBL HDR capture");
        for(float value:result.hdr.rgba32)Check(std::isfinite(value)&&value>=0&&value<=65504,"actual IBL HDR remains finite");return result;};
    const size_t center=(size_t(height/2)*width+width/2)*4;
    for(float metal:{0.0f,0.5f,1.0f})for(float rough:{0.045f,0.25f,0.5f,0.75f,1.0f})
    {
        draws[0].tint={0.8f,0.3f,0.1f,1};draws[0].pbr.metallic=metal;draws[0].pbr.roughness=rough;intensity=metal==0?0.5f:metal==1?2.0f:1.0f;
        const auto actual=capture();const float colors[]{0.8f,0.3f,0.1f},radiance[]{1,0.5f,0.25f};
        for(size_t channel=0;channel<3;++channel)
        {
            const double f0=0.04*(1-metal)+colors[channel]*metal;
            const double irradiance=HalfToFloat(uniform->textures[0].subresources[0][channel]);
            const double expected=(irradiance*colors[channel]*(1-metal)*(1-f0)/double(Pi)+radiance[channel]*(f0*Lut(uniform->textures[2],1,rough,0)+Lut(uniform->textures[2],1,rough,1)))*intensity;
            Check(std::abs(actual.hdr.rgba32[center+channel]-expected)<0.004+expected*0.003,"split-sum HDR matches independent scalar diffuse/F0/LUT oracle");
        }
    }
    intensity=0;const auto black=capture();Check(black.hdr.rgba32[center]==0&&black.hdr.rgba32[0]==0,"zero intensity removes both sky and IBL");
    auto axes=std::make_shared<EnvironmentAsset>(*uniform);
    for(auto& sub:axes->textures[0].subresources)for(size_t i=0;i<sub.size();i+=4){sub[i]=sub[i+1]=sub[i+2]=0;}
    for(UINT face=0;face<6;++face)for(UINT mip=0;mip<4;++mip)
    {auto& sub=axes->textures[1].subresources[size_t(face)*4+mip];for(size_t i=0;i<sub.size();i+=4){sub[i]=FloatToHalf(1+float(face)*0.25f);sub[i+1]=FloatToHalf(float(mip)+1);sub[i+2]=FloatToHalf(0.5f);}}
    for(size_t i=0;i<axes->textures[2].subresources[0].size();i+=4){axes->textures[2].subresources[0][i]=0x3c00;axes->textures[2].subresources[0][i+1]=0;}
    Check(environmentCache.Upload(axes,device,environment,diagnostics),"axis and mip oracle upload");intensity=1;draws.clear();
    const Vec3 directions[]{{1,0,0},{-1,0,0},{0,1,0},{0,-1,0},{0,0,1},{0,0,-1}};
    for(size_t face=0;face<6;++face)
    {camera=Camera(directions[face]);const auto actual=capture();const double linear=1+double(face)*0.25;
        Check(actual.hdr.rgba32[center]==linear&&actual.hdr.rgba32[center+1]==1,"sky cardinal axes select exact cube face and mip zero");
        const double mapped=linear/(1+linear),srgb=mapped<=0.0031308?12.92*mapped:1.055*std::pow(mapped,1/2.4)-0.055;
        Check(std::abs(int(actual.image.rgba8[center])-int(std::lround(srgb*255)))<=1,"HDR environment reaches existing single tone map and sRGB output");}
    camera=Camera({1,0,0});yaw=Pi/2;const auto rotatedSky=capture();Check(rotatedSky.hdr.rgba32[center]==2,"positive world yaw uses inverse lookup in sky");
    camera=Camera({1,0,0},{100,50,-30});Check(capture().hdr.rgba32==rotatedSky.hdr.rgba32,"sky is invariant under camera translation");
    camera=Camera();yaw=0;AppendPbrModelDraws(*cube,Translation({0,0,-0.005f})*Scaling({4,3,0.01f}),{},draws,*mesh,*OrthographicRH(4,3,0.1f,100)*camera.view,camera.position,{0,0,1},{});
    draws[0].tint={1,1,1,1};draws[0].pbr.metallic=1;
    for(UINT mip=1;mip<4;++mip){draws[0].pbr.roughness=float(mip)/3;const auto actual=capture();Check(std::abs(actual.hdr.rgba32[center+1]-float(mip+1))<0.004f,"roughness explicitly samples every prefiltered GPU mip");}
    yaw=Pi/2;Check(capture().hdr.rgba32[center]==1.25f,"IBL uses same inverse world yaw as sky");yaw=0;
    auto seam=std::make_shared<EnvironmentAsset>(*axes);
    for(UINT face=0;face<6;++face)for(UINT mip=0;mip<4;++mip)
    {auto& texture=seam->textures[1];const UINT size=std::max(1u,texture.size>>mip);auto& sub=texture.subresources[size_t(face)*4+mip];for(UINT y=0;y<size;++y)for(UINT x=0;x<size;++x)
        {const auto d=EnvironmentCubeDirection(face,(float(x)+0.5f)/size,(float(y)+0.5f)/size);const auto i=(size_t(y)*size+x)*4;sub[i]=FloatToHalf(0.5f+d.x*0.5f);sub[i+1]=FloatToHalf(0.5f+d.y*0.5f);sub[i+2]=FloatToHalf(0.5f+d.z*0.5f);}}
    Check(environmentCache.Upload(seam,device,environment,diagnostics),"continuous direction cube");draws.clear();camera=Camera(Normalize(Vec3{1.00001f,0,1}));const auto edgeA=capture();camera=Camera(Normalize(Vec3{1,0,1.00001f}));const auto edgeB=capture();
    for(size_t channel=0;channel<3;++channel)Check(std::abs(edgeA.hdr.rgba32[center+channel]-edgeB.hdr.rgba32[center+channel])<0.003f,"seamless cube sampling agrees across adjacent faces");
    const auto before=device.Stats();for(unsigned i=0;i<32;++i){auto commands=record();rhi::CompletionToken token;Ok(device.Submit(*commands,token),"ordinary IBL frame");}
    const auto after=device.Stats();Check(after.globalWaits==before.globalWaits&&after.uploadedBytes==before.uploadedBytes&&after.pipelineCreations==before.pipelineCreations&&after.shaderProgramCompilations==before.shaderProgramCompilations&&after.memoryResourceCreations==before.memoryResourceCreations,"ordinary IBL frames do not bake, upload, compile, create resources or flush");
    Ok(device.ExportGpuPassTimingsCsv(output/"passes.csv"),"actual IBL pass timings");
    // 在真实队列上扣住两帧，避免靠 GPU 已经跑完的偶然时序证明寿命。
    Ok(device.WaitIdle(),"before controlled environment switch");std::shared_ptr<const RenderEnvironment> nextEnvironment;
    Check(environmentCache.Upload(axes,device,nextEnvironment,diagnostics),"next environment ready before gate");Ok(device.WaitIdle(),"complete fixture uploads before gate");
    auto* presentation=rhi::D3D12DiagnosticsAccess::Presentation(device);ComPtr<ID3D12Fence> gate;
    Hr(rhi::D3D12DiagnosticsAccess::NativeDevice(device)->CreateFence(0,D3D12_FENCE_FLAG_NONE,IID_PPV_ARGS(gate.GetAddressOf())),"environment queue gate");
    struct Gate
    {
        ComPtr<ID3D12Fence> fence;HANDLE event;std::thread worker;std::atomic<bool> timedOut{false};
        explicit Gate(ComPtr<ID3D12Fence> value):fence(std::move(value)),event(CreateEventW(nullptr,TRUE,FALSE,nullptr))
        {Check(event!=nullptr,"environment gate event");worker=std::thread([this]{if(WaitForSingleObject(event,3000)==WAIT_TIMEOUT){timedOut=true;fence->Signal(1);}});}
        void Release(){fence->Signal(1);SetEvent(event);}
        ~Gate(){Release();if(worker.joinable())worker.join();CloseHandle(event);}
    } release{gate};
    const auto oldHandle=*environment->textures[1];auto oldState=presentation->states->Find(*rhi::D3D12DiagnosticsAccess::Resource(device,oldHandle));
    const auto oldSlot=rhi::D3D12DiagnosticsAccess::SrvCpu(device,oldHandle).ptr;const auto oldLifetime=rhi::D3D12DiagnosticsAccess::Lifetime(device,oldHandle);
    Hr(presentation->queue->Wait(gate.Get(),1),"hold two environment versions on GPU");const auto waits=device.Stats().globalWaits;
    auto packet=record();rhi::CompletionToken firstToken,secondToken;Ok(device.Submit(*packet,firstToken),"submit retained old cube");packet.reset();environment=std::move(nextEnvironment);
    packet=record();Ok(device.Submit(*packet,secondToken),"submit replacement cube without drain");packet.reset();
    Check(!release.timedOut&&!device.IsComplete(firstToken)&&!device.IsComplete(secondToken)&&oldState->attached&&!oldLifetime.expired()
        &&rhi::D3D12DiagnosticsAccess::SrvCpu(device,*environment->textures[1]).ptr!=oldSlot&&device.Stats().globalWaits==waits,"old cube and descriptors survive both unfinished frames without reuse or flush");
    release.Release();Ok(device.WaitIdle(),"complete environment switch");device.Collect();Check(!oldState->attached&&oldLifetime.expired(),"old environment is actually destroyed only after its Fence");
    auto fallback=environmentCache.Fallback(device,diagnostics);Check(bool(fallback),"explicit initialized missing-environment fallback");environment=fallback;camera=Camera();
    const auto missing=capture();Check(std::all_of(missing.hdr.rgba32.begin(),missing.hdr.rgba32.end(),[i=size_t{0}](float value)mutable{return (++i%4)==0?value==1:value==0;}),"first missing environment renders finite black through explicit GPU fallback");fallback.reset();
    // 脱离 HDR 和旁路 manifest 的容器仍通过 AssetStore 与相同 GPU 路径装载。
    const auto standalone=output/"standalone";std::filesystem::create_directories(standalone);const auto container=standalone/"soft.seenv";
    std::filesystem::copy_file(GetExecutableDirectory()/"Resources/Environments/soft.seenv",container,std::filesystem::copy_options::overwrite_existing);
    AssetStore assets(standalone,output/"derived");AssetId id;Check(assets.RegisterSource("soft.seenv",{},id,diagnostics),"standalone source identity");
    environment=environmentCache.Load(assets,id,device,diagnostics);Check(environment&&environment->source,"standalone baked environment upload");
    const auto cached=environment;const auto uploads=device.Stats().uploadSubmissions;Check(environmentCache.Load(assets,id,device,diagnostics)==cached&&device.Stats().uploadSubmissions==uploads,"unchanged environment reuses active GPU version");
    std::vector<std::uint8_t> original;Check(asset_io::Read(container,original),"read published container");auto corrupt=original;corrupt.back()^=1;Check(asset_io::WriteAtomic(container,corrupt),"corrupt revision fixture");
    Check(environmentCache.Load(assets,id,device,diagnostics)==cached,"invalid container keeps last valid GPU environment");Check(asset_io::WriteAtomic(container,original),"restore published container");
    Check(environmentCache.Load(assets,id,device,diagnostics)==cached,"recovered identical source reuses GPU version");
    // 实际 World 的 25 球和 3 法线方块，共享材料与离线环境。
    MaterialsDemo demo("en-US",false,"soft");auto services=demo.Services();Check(services.windowOpened(fixture.window.hwnd)&&services.update(0),"environment World sample");
    const auto scene=[&]{FrameData data;services.extract(data);Check(data.meshes.size()==28&&data.assets&&!data.environment.asset.empty(),"environment snapshot contract");
        auto selected=SelectCamera(data.cameras,{0,0,960,720},frame+1);Check(selected.snapshot.has_value(),"sample camera");camera=*selected.snapshot;
        environment=environmentCache.Load(*data.assets,data.environment.asset,device,diagnostics);Check(bool(environment),"sample environment load");intensity=data.environment.intensity;yaw=data.environment.yaw;
        lighting=PrepareForwardLighting(data.lights,data.exposure,data.skyColor,diagnostics);Check(PrepareDirectionalShadow(data.shadow,lighting,shadows,diagnostics),"sample shadow settings");draws.clear();
        auto sphereSource=std::make_shared<StaticModel>(*source);sphereSource->meshes[0]=MakeUnitSphere();sphereSource->meshes[0].material=0;std::shared_ptr<const StaticRenderModel> sphere;
        Check(models.Upload(sphereSource,{},std::filesystem::current_path(),device,sphere,diagnostics),"sample sphere");
        for(const auto& instance:data.meshes){std::vector<std::shared_ptr<const PbrRenderMaterial>> overrides;for(const auto& material:instance.mesh.materials)overrides.push_back(models.LoadMaterial(*data.assets,material,device,diagnostics));
            AppendPbrModelDraws(instance.mesh.meshAsset==BuiltinSphereAssetId?*sphere:*cube,instance.world,overrides,draws,*mesh,camera.viewProjection,camera.position,{0,0,1},{});}
        return capture();};
    Ok(device.Resize(960,720),"sample image size");const auto soft=scene();Save(soft.image,output/"soft.ppm");
    InputState input;input.hasFocus=true;input.keyEvents={{KeyEventType::Pressed,'M'}};services.input(input,false,false);Check(services.update(0),"sample environment switch");
    const auto contrast=scene();Save(contrast.image,output/"contrast.ppm");Check(contrast.hdr.rgba32!=soft.hdr.rgba32,"two offline environments affect actual World image");
    const auto& prefiltered=environment->source->textures[1];float sharpPeak=0,roughPeak=0;
    for(UINT face=0;face<6;++face)for(UINT mip=0;mip<prefiltered.mipLevels;++mip)
    {const auto& sub=prefiltered.subresources[size_t(face)*prefiltered.mipLevels+mip];for(size_t i=0;i<sub.size();i+=4)
        {const float value=HalfToFloat(sub[i]);if(!mip)sharpPeak=std::max(sharpPeak,value);if(mip+1==prefiltered.mipLevels)roughPeak=std::max(roughPeak,value);}}
    Check(sharpPeak>8&&roughPeak<sharpPeak*0.25f,"offline GGX filtering strongly broadens high contrast features as roughness increases");
    input.keyEvents={{KeyEventType::Pressed,VK_RIGHT}};services.input(input,false,false);Check(services.update(0),"sample yaw control");const auto rotated=scene();Check(rotated.hdr.rgba32!=contrast.hdr.rgba32,"sample yaw changes sky and material response");
    input.keyEvents={{KeyEventType::Pressed,VK_OEM_PLUS}};services.input(input,false,false);Check(services.update(0),"sample exposure control");const auto exposed=scene();Check(exposed.hdr.rgba32==rotated.hdr.rgba32&&exposed.image.rgba8!=rotated.image.rgba8,"exposure changes output without changing environment HDR");services.windowClosing();
    Ok(device.ExportGpuPassTimingsCsv(output/"sample-passes.csv"),"environment sample timings");
    const auto stats=device.Stats();if(stats.debugWarnings||stats.debugErrors)
    {ComPtr<ID3D12InfoQueue> queue;rhi::D3D12DiagnosticsAccess::NativeDevice(device)->QueryInterface(IID_PPV_ARGS(queue.GetAddressOf()));for(UINT64 i=0;queue&&i<queue->GetNumStoredMessages();++i)
        {SIZE_T bytes=0;queue->GetMessage(i,nullptr,&bytes);std::vector<std::byte> storage(bytes);auto* message=reinterpret_cast<D3D12_MESSAGE*>(storage.data());if(SUCCEEDED(queue->GetMessage(i,message,&bytes))&&message->Severity<=D3D12_MESSAGE_SEVERITY_WARNING)std::cerr<<"D3D12 "<<message->ID<<": "<<message->pDescription<<'\n';}}
    Check(stats.debugWarnings==0&&stats.debugErrors==0,"IBL GPU warning/error/corruption zero");
    const nlohmann::json evidence{{"frames",frame},{"ordinaryFrames",32},{"halfTextureAllSubresourcesExact",true},{"splitSumScalarCases",15},{"cardinalSkyFaces",6},
        {"contrastSharpPeak",sharpPeak},{"contrastRoughPeak",roughPeak},{"cookerVersion",EnvironmentCookerVersion},{"debugWarnings",stats.debugWarnings},{"debugErrors",stats.debugErrors}};
    std::ofstream report(output/"validation.json");report<<evidence.dump(2)<<'\n';Check(bool(report),"environment validation evidence");
}
}
int wmain(int argc,wchar_t** argv)
{
    try{Check(argc==2,"supply environment GPU output directory");const auto output=std::filesystem::absolute(argv[1]);std::filesystem::create_directories(output);Localization text;std::string error;
        Check(text.Load(GetExecutableDirectory()/"Resources/Localization/en-US.txt",error),"environment strings");Fixture fixture(text);Gpu(fixture,text,output);std::cout<<"EnvironmentGpuTests passed (actual array/cube/mips, split-sum HDR, sky axes/yaw/seams, standalone recovery, ordinary frames, World sample); GPU warnings/errors=0.\n";return 0;}
    catch(const std::exception& error){std::cerr<<"EnvironmentGpuTests: "<<error.what()<<'\n';return 1;}
}
