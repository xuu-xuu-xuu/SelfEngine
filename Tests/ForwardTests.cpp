#include "FunctionLayer/Rendering/ForwardRenderer.h"
#include "FunctionLayer/Rendering/StaticModelRender.h"
#include "FunctionLayer/Rendering/SandboxRenderer.h"
#include "FunctionLayer/Rendering/ColorGrading.h"
#include "FunctionLayer/Runtime/ComponentMetadata.h"
#include "ResourceLayer/Scenes/SceneSerializer.h"
#include "ResourceLayer/Assets/Primitives.h"
#include "PlatformLayer/Graphics/D3D12/D3D12Diagnostics.h"
#include "PlatformLayer/Graphics/D3D12/D3D12Presentation.h"
#include "PlatformLayer/Windows/ExecutablePath.h"
#include "PlatformLayer/Windows/Window.h"
#include "CoreLayer/Localization.h"
#include "Materials/MaterialsDemo.h"
#include <nlohmann/json.hpp>
#include <algorithm>
#include <atomic>
#include <cstdlib>
#include <fstream>
#include <iostream>
#include <stdexcept>
#include <thread>
#include <d3d12sdklayers.h>

namespace fs=std::filesystem;
namespace rhi=se::rhi;
using namespace se;
namespace
{
void Check(bool value,const char* message){if(!value)throw std::runtime_error(message);}
void Ok(rhi::Result result,const char* message){if(!result)throw std::runtime_error(std::string(message)+": "+result.Key());}
bool Has(const std::vector<AssetDiagnostic>& diagnostics,const char* key){return std::any_of(diagnostics.begin(),diagnostics.end(),[&](const auto& d){return d.key==key;});}
void NoProblems(rhi::Device& device)
{
    const auto stats=device.Stats();if(stats.debugWarnings||stats.debugErrors)
    {
        Microsoft::WRL::ComPtr<ID3D12InfoQueue> queue;Check(SUCCEEDED(rhi::D3D12DiagnosticsAccess::NativeDevice(device)->QueryInterface(IID_PPV_ARGS(queue.GetAddressOf()))),"debug queue diagnostics");
        for(UINT64 index=0;index<queue->GetNumStoredMessages();++index)
        {
            SIZE_T count=0;queue->GetMessage(index,nullptr,&count);std::vector<std::byte> storage(count);auto* message=reinterpret_cast<D3D12_MESSAGE*>(storage.data());
            if(SUCCEEDED(queue->GetMessage(index,message,&count))&&message->Severity<=D3D12_MESSAGE_SEVERITY_WARNING)std::cerr<<"D3D12 "<<message->ID<<": "<<message->pDescription<<'\n';
        }
    }
    Check(stats.debugWarnings==0&&stats.debugErrors==0,"Forward GPU warning/error/corruption zero");
}
void Save(const rhi::Image& image,const fs::path& path)
{
    std::ofstream stream(path,std::ios::binary);stream<<"P6\n"<<image.width<<' '<<image.height<<"\n255\n";
    for(size_t index=0;index<image.rgba8.size();index+=4)stream.write(reinterpret_cast<const char*>(image.rgba8.data()+index),3);Check(bool(stream),"save Forward image");
}
void Cpu()
{
    World world;std::string error;PropertyValue value;const auto light=world.Create("Point",42);
    Check(world.SetLight(light,{{1,0.5f,0.2f},4,LightType::Point,7}),"new point component");
    Check(ReadProperty(world,light,"light.point",value,error)&&std::get<bool>(value),"light type in metadata");
    Check(WriteProperty(world,light,"light.range",8.0f,error)&&!WriteProperty(world,light,"light.range",-1.0f,error),"point range edit validation");
    Check(!world.SetLight(light,{{1,1,1},1,static_cast<LightType>(99),10}),"unknown light type rejected");
    std::string bytes;Check(SerializeScene(world,bytes,error),"serialize new light fields");World restored;
    Check(DeserializeScene(bytes,restored,error)&&restored.GetLight(restored.FindByStableId(42))->type==LightType::Point&&restored.GetLight(restored.FindByStableId(42))->range==8,"new light roundtrip");
    auto json=nlohmann::json::parse(bytes);auto& legacyEntity=json["entities"][0];
    const auto lightProperties=legacyEntity["components"][1]["properties"];legacyEntity["transform"]=legacyEntity["components"][0]["properties"];
    legacyEntity["components"]={{"light",lightProperties}};json["version"]=1;
    json["entities"][0]["components"]["light"].erase("type");json["entities"][0]["components"]["light"].erase("range");
    Check(DeserializeScene(json.dump(),restored,error)&&restored.GetLight(restored.FindByStableId(42))->type==LightType::Directional,"old light schema defaults to directional");
    json["entities"][0]["components"]["light"]["type"]="invalid";Check(!DeserializeScene(json.dump(),restored,error),"unknown serialized type rejected");
    std::vector<LightInstance> lights;
    for(std::uint64_t index=0;index<10;++index)lights.push_back({100-index,Translation({float(index),1,2}),{{1,1,1},1,LightType::Point,10}});
    lights.push_back({3,{},{{1,0,0},3}});lights.push_back({2,{},{{0,1,0},2}});
    std::vector<AssetDiagnostic> diagnostics;auto settings=PrepareForwardLighting(lights,1,{0.1f,0.2f,0.3f},diagnostics);
    Check(settings.pointCount==8&&settings.points[0].position.x==9&&settings.points[7].position.x==2&&settings.directionalRadiance.y==2&&Has(diagnostics,"Rendering.LightLimit"),"stable bounded lights selected from unsorted snapshot");
    lights={{1,{},{{1e30f,1e30f,1e30f},1e30f,LightType::Point,1e30f}}};diagnostics.clear();
    settings=PrepareForwardLighting(lights,1e20f,{1e20f,0,0},diagnostics);
    Check(settings.points[0].radiance.x==10000&&settings.points[0].range==10000&&settings.exposure==64&&Has(diagnostics,"Rendering.LightClamped"),"finite source overflow guarded before float multiply");
    Check(lights[0].light.intensity==1e30f,"render adapter does not mutate World snapshot");
}
struct Fixture
{
    WindowState window;std::shared_ptr<rhi::Device> device;StaticModelRenderCache cache{true};rhi::Pipeline mesh,output,sky;
    std::uint64_t nextSource=1;
    explicit Fixture(const Localization& text)
    {
        Check(CreateEngineWindow(window,false),"Forward hidden window");Size(320,240);rhi::Result result;
        device=rhi::Device::CreateD3D12({window.hwnd,320,240},text,result);Ok(result,"Forward device");
        rhi::PipelineDescription description{rhi::PipelineKind::ForwardMesh,GetExecutableDirectory()/"Resources/Shaders/Forward.hlsl",3};description.colorFormat=rhi::ColorFormat::Rgba16Float;
        Ok(device->CreatePipeline(description,mesh),"Forward reflected HDR pipeline");Ok(device->CreatePipeline({rhi::PipelineKind::FinalOutput,GetExecutableDirectory()/"Resources/Shaders/FinalOutput.hlsl",3},output),"full-screen output pipeline");
        description.kind=rhi::PipelineKind::Sky;description.shader=GetExecutableDirectory()/"Resources/Shaders/Sky.hlsl";
        description.bindingLayoutVersion=rhi::BindingLayout(description.kind);
        Ok(device->CreatePipeline(description,sky),"HDR sky pipeline");
        Check(device->Stats().shaderArtifactLoads==3&&device->Stats().shaderProgramCompilations==0,"all Forward artifacts used");
    }
    void Size(UINT width,UINT height)
    {
        RECT rect{0,0,LONG(width),LONG(height)};AdjustWindowRectEx(&rect,static_cast<DWORD>(GetWindowLongPtrW(window.hwnd,GWL_STYLE)),FALSE,0);
        Check(SetWindowPos(window.hwnd,nullptr,0,0,rect.right-rect.left,rect.bottom-rect.top,SWP_NOMOVE|SWP_NOZORDER|SWP_NOACTIVATE)!=FALSE,"Forward window size");int code=0;Check(PumpWindowMessages(code),"Forward window messages");
    }
    ~Fixture(){if(device)device->Shutdown();cache.Clear();mesh.reset();output.reset();sky.reset();window.postQuitOnDestroy=false;DestroyEngineWindow(window);}
};
std::shared_ptr<const StaticRenderModel> Upload(Fixture& f,bool quad,bool cube=false)
{
    auto source=std::make_shared<StaticModel>();auto mesh=quad?MeshData{}:cube?MakeUnitCube():MakeUnitSphere();mesh.name="Reference";mesh.material=0;
    if(quad)
    {
        mesh.vertices={{{-1,-1,0},{0,0,1},{0,1},{1,0,0,1}},{{1,-1,0},{0,0,1},{1,1},{1,0,0,1}},{{1,1,0},{0,0,1},{1,0},{1,0,0,1}},{{-1,1,0},{0,0,1},{0,0},{1,0,0,1}}};mesh.indices={0,1,2,0,2,3};
    }
    source->meshes.push_back(mesh);source->materials.emplace_back();ModelNode node;node.name="Reference";node.meshes={0};source->nodes.push_back(node);
    std::vector<AssetDiagnostic> diagnostics;std::shared_ptr<const StaticRenderModel> model;
    Check(f.cache.Upload(source,{},fs::current_path(),*f.device,model,diagnostics),"Forward geometry/material upload");return model;
}
struct Captured{rhi::Image output;rhi::HdrImage hdr;};
Captured Capture(Fixture& f,const rhi::ForwardLighting& lighting,std::span<const rhi::DrawIndexed> draws,std::uint64_t source=0)
{
    if(!source)source=f.nextSource++;
    auto commands=f.device->BeginCommands(source);Ok(RecordForwardFrame(*commands,lighting,*f.output,*f.sky,draws),"explicit Forward passes");Captured image;rhi::CompletionToken token;
    Ok(f.device->Submit(*commands,token,&image.output,&image.hdr),"HDR and final capture");return image;
}
int Encode(double linear,double exposure)
{
    const double radiance=std::max(linear,0.0)*exposure,mapped=radiance/(1+radiance);
    return static_cast<int>(std::lround(255*(mapped<=0.0031308?mapped*12.92:1.055*std::pow(mapped,1.0/2.4)-0.055)));
}
std::array<double,3> Reference(const rhi::DrawIndexed& draw,const rhi::ForwardLighting& lighting)
{
    const Vec3 point{(160.5f/320*4)-2,1.5f-(120.5f/240*3),0},v=Normalize(draw.pbr.cameraPosition-point);const double nv=v.z;
    const double rough=std::max(double(draw.pbr.roughness),0.045),a2=std::pow(rough,4);std::array<double,3> total{};
    const double color[]{draw.tint.x,draw.tint.y,draw.tint.z};
    const auto accumulate=[&](Vec3 direction,Vec3 radiance,double attenuation){
        const auto l=Normalize(direction),h=Normalize(v+l);const double nl=std::clamp(double(l.z),0.0,1.0),nh=std::clamp(double(h.z),0.0,1.0),vh=std::clamp(double(Dot(v,h)),0.0,1.0);
        const double divisor=nh*nh*(a2-1)+1,d=a2/(3.141592653589793*divisor*divisor),visibility=0.5/std::max(nl*std::sqrt(nv*nv*(1-a2)+a2)+nv*std::sqrt(nl*nl*(1-a2)+a2),1e-8);
        const double light[]{radiance.x,radiance.y,radiance.z};
        for(size_t channel=0;channel<3;++channel)
        {const double f0=0.04*(1-draw.pbr.metallic)+color[channel]*draw.pbr.metallic,f=f0+(1-f0)*std::pow(1-vh,5);
            total[channel]+=((1-f)*(1-draw.pbr.metallic)*color[channel]/3.141592653589793+d*visibility*f)*nl*light[channel]*attenuation;}
    };
    if(lighting.directionalEnabled)accumulate(lighting.directionToLight,lighting.directionalRadiance,1);
    for(UINT index=0;index<lighting.pointCount;++index)
    {
        const auto& light=lighting.points[index];const auto delta=light.position-point;const double squared=double(delta.x)*delta.x+double(delta.y)*delta.y+double(delta.z)*delta.z;
        const double ratio=squared/(double(light.range)*light.range),window=std::clamp(1-ratio*ratio,0.0,1.0);accumulate(delta,light.radiance,window*window/std::max(squared,0.0025));
    }
    return total;
}
void Pixels(const Captured& image,std::array<double,3> expected,double exposure)
{
    Check(image.output.width==320&&image.hdr.width==320&&image.hdr.height==240,"HDR/output dimensions match");const auto offset=(size_t(120)*320+160)*4;
    for(size_t channel=0;channel<3;++channel)
    {
        const double actual=image.hdr.rgba32[offset+channel];
        if(std::abs(actual-expected[channel])>std::max(0.003,std::abs(expected[channel])*0.003))
        {std::cerr<<"HDR channel "<<channel<<" actual="<<actual<<" expected="<<expected[channel]<<'\n';Check(false,"linear HDR agrees with independent BRDF/attenuation oracle");}
        Check(std::abs(int(image.output.rgba8[offset+channel])-Encode(actual,exposure))<=1,"Reinhard and sRGB applied once to actual half float");
    }
    for(const float value:image.hdr.rgba32)Check(std::isfinite(value)&&value>=0&&value<=65504,"all HDR pixels finite in target range");
}
void Gpu(const Localization& text,const fs::path& output)
{
    Fixture f(text);auto& device=*f.device;auto quad=Upload(f,true);std::vector<rhi::DrawIndexed> draws;
    AppendPbrModelDraws(*quad,{}, {},draws,*f.mesh,*OrthographicRH(4,3,0.1f,100)*Translation({0,0,-3}),{0,0,3},{0,0,1},{});
    auto& draw=draws[0];draw.tint={0.65f,0.25f,0.1f,1};draw.pbr.roughness=0.5f;draw.pbr.metallic=0.2f;
    rhi::ForwardLighting lighting;lighting.sky={0.1f,0.2f,0.3f};lighting.directionalEnabled=true;lighting.directionToLight={0.4f,0.2f,1};lighting.directionalRadiance={12,8,4};
    Ok(device.EnableGpuTiming(true),"Forward pass queries");auto image=Capture(f,lighting,draws);Pixels(image,Reference(draw,lighting),lighting.exposure);Save(image.output,output/"directional.ppm");
    Check(image.hdr.rgba32[(size_t(120)*320+160)*4]>1,"HDR radiance above one survives main pass");
    // G15.09：FinalOutput 的 29 号输出布局。占位 1×1 纹理必须与 3 号布局逐字节一致；
    // Rgba16F strip 只改变显示线性输出，HDR 场景目标不变。
    {
        rhi::PipelineDescription lutDescription{rhi::PipelineKind::FinalOutput,
            GetExecutableDirectory()/"Resources/Shaders/FinalOutput.hlsl",rhi::OutputLutLayoutVersion};
        lutDescription.macros.push_back({"SE_OUTPUT_LUT","1"});
        rhi::Pipeline lutOutput;Ok(device.CreatePipeline(lutDescription,lutOutput),"FinalOutput LUT pipeline");
        const std::vector<std::uint8_t> white{255,255,255,255};
        rhi::Texture placeholder;Ok(device.CreateTexture({1,1},white,placeholder),"output LUT placeholder");
        ColorGradingSettings warm;warm.gain[0]=1.2f;warm.gain[1]=0.9f;warm.gain[2]=1.1f;
        ColorLut lut;Check(lut.Build(33,warm),"output LUT fixture builds");
        ColorLutStrip strip;Check(BakeColorLutStrip(lut,ColorLutStripFormat::Rgba16Float,strip)&&strip.Valid(),
            "output LUT strip bakes");
        rhi::TextureDescription stripDescription{strip.width,strip.height};
        stripDescription.format=rhi::TextureFormat::Rgba16Float;
        const rhi::TextureSubresource subresource{strip.bytes,0};
        rhi::Texture stripTexture;Ok(device.CreateTextureSubresources(stripDescription,std::span(&subresource,1),stripTexture),
            "output LUT strip uploads");
        const auto captureOutput=[&](rhi::TextureHandle colorLut){
            auto configured=lighting;configured.colorLut=colorLut;
            auto commands=device.BeginCommands(f.nextSource++);
            Ok(RecordForwardFrame(*commands,configured,*lutOutput,*f.sky,draws),"LUT output passes");
            Captured captured;rhi::CompletionToken token;
            Ok(device.Submit(*commands,token,&captured.output,&captured.hdr),"LUT output capture");return captured;};
        const auto placeholderImage=captureOutput(*placeholder);
        Check(placeholderImage.output.rgba8==image.output.rgba8,
            "29 layout with the 1x1 placeholder is byte-identical to the 3-layout output");
        const auto gradedImage=captureOutput(*stripTexture);
        Check(gradedImage.hdr.rgba32==image.hdr.rgba32,"output LUT does not alter the HDR scene target");
        size_t gradedDifference=0;int gradedMax=0;
        for(size_t index=0;index<gradedImage.output.rgba8.size();++index)
        {
            const int delta=std::abs(int(gradedImage.output.rgba8[index])-int(image.output.rgba8[index]));
            if(delta){++gradedDifference;gradedMax=std::max(gradedMax,delta);}
        }
        Check(gradedDifference>1000,"FinalOutput LUT changes display pixels");
        Save(gradedImage.output,output/"output-lut.ppm");
        std::cout<<"Output LUT: changed="<<gradedDifference<<" max="<<gradedMax
            <<", placeholder byte-identical=1\n";
    }
    lighting.directionalEnabled=false;lighting.pointCount=1;lighting.points[0]={{0.5f,0.2f,2},{20,30,40},5};
    for(float z:{1.0f,2.0f,4.0f,8.0f}){lighting.points[0].position.z=z;image=Capture(f,lighting,draws);Pixels(image,Reference(draw,lighting),1);}
    lighting.points[0].position={0.5f,0.2f,2};lighting.pointCount=8;
    for(UINT index=1;index<8;++index)lighting.points[index]={{float(index)*0.15f,0.5f,1.5f},{float(index),2,3},4};
    lighting.directionalEnabled=true;image=Capture(f,lighting,draws);Pixels(image,Reference(draw,lighting),1);Save(image.output,output/"eight-points.ppm");
    auto extreme=lighting;extreme.directionalEnabled=false;extreme.pointCount=1;extreme.points[0]={{0.00625f,-0.00625f,0.001f},{10000,10000,10000},0.05f};
    auto sharp=draw;sharp.pbr.roughness=0;auto bounded=Capture(f,extreme,std::span(&sharp,1));
    Check(bounded.hdr.rgba32[(size_t(120)*320+160)*4]==65504,"near punctual light and minimum roughness saturate finite half-float maximum");
    for(float value:bounded.hdr.rgba32)Check(std::isfinite(value)&&value>=0&&value<=65504,"extreme HDR does not produce infinity or NaN");
    lighting.exposure=0.25f;const auto dark=Capture(f,lighting,draws);Pixels(dark,Reference(draw,lighting),0.25);Check(dark.hdr.rgba32==image.hdr.rgba32&&dark.output.rgba8!=image.output.rgba8,"exposure changes only final output");
    lighting.skySettings.zenithColor={0.5f,0.2f,0.1f};lighting.skySettings.horizonColor={0.2f,0.35f,0.55f};
    auto empty=Capture(f,lighting,{});const size_t center=(size_t(120)*320+160)*4;
    Check(empty.hdr.rgba32[center]>0.001f&&empty.output.rgba8[center]==Encode(empty.hdr.rgba32[center],0.25),"editable Sky pass visible with no geometry");
    Check(empty.hdr.rgba32[center]!=empty.hdr.rgba32[(size_t(4)*320+160)*4],"procedural sky varies with view direction");
    auto bad=device.BeginCommands();auto invalid=lighting;invalid.pointCount=9;Check(bad->ConfigureForward(invalid,*f.output,*f.sky).error==rhi::Error::InvalidArgument,"RHI also bounds point count before copying constants");
    Ok(bad->ConfigureForward(lighting,*f.output,*f.sky),"valid Forward configuration");Check(bad->BeginPass({rhi::PassKind::Opaque}).error==rhi::Error::InvalidState,"Opaque cannot skip Clear/Sky");
    Ok(bad->BeginPass({rhi::PassKind::Clear}),"ordered clear");Ok(bad->EndPass(),"ordered clear end");Check(bad->Close().error==rhi::Error::InvalidState,"incomplete Forward frame cannot submit");bad.reset();
    const auto before=device.Stats();rhi::CompletionToken token;
    for(std::uint64_t frame=20;frame<52;++frame){auto commands=device.BeginCommands(frame);Ok(RecordForwardFrame(*commands,lighting,*f.output,*f.sky,draws),"ordinary Forward pass schedule");Ok(device.Submit(*commands,token),"ordinary Forward submit");}
    Check(device.Stats().globalWaits==before.globalWaits&&device.Stats().hdrCreations==before.hdrCreations&&device.Stats().pipelineCreations==before.pipelineCreations,"ordinary Forward frames do not flush/rebuild HDR or PSO");
    Ok(device.ExportGpuPassTimingsCsv(output/"passes.csv"),"Forward per-pass CSV");const auto samples=device.GpuTimings();Check(samples.size()==43,"all eleven capture and 32 ordinary timestamp samples including tail collected");
    for(const auto& sample:samples)
    {
        Check(sample.forward&&sample.frequency>0,"Forward sample metadata");for(size_t index=1;index<8;++index)Check(sample.passTicks[index]>=sample.passTicks[index-1],"four ordered GPU timestamp intervals");
        Check(sample.ticks[3]>=sample.passTicks[7],"whole frame includes final capture and Present transition");
    }
    const auto baselineResources=device.Stats().memoryResources;
    for(UINT cycle=0;cycle<6;++cycle)
    {
        const UINT width=cycle%2?320:512,height=cycle%2?240:256;f.Size(width,height);Ok(device.Resize(width,height),"Forward resize");
        auto resized=Capture(f,lighting,draws,100+cycle);Check(resized.hdr.width==width&&resized.hdr.height==height&&resized.output.width==width&&resized.output.height==height,"HDR/depth/output resize together");
        Check(device.Stats().memoryResources==baselineResources&&device.Stats().hdrWidth==width&&device.Stats().hdrHeight==height,"resize resource count stable");
        const auto* presentation=rhi::D3D12DiagnosticsAccess::Presentation(device);Check(presentation->depth->GetDesc().Width==width&&presentation->depth->GetDesc().Height==height,"actual depth follows HDR dimensions");
        Ok(device.Resize(0,0),"minimize is deferred");Check(device.Stats().hdrWidth==width,"zero dimensions preserve last valid targets");
    }
    NoProblems(device);
}
void GatedResize(const Localization& text)
{
    Fixture f(text);auto& device=*f.device;rhi::ForwardLighting lighting;Capture(f,lighting,{});f.Size(512,256);
    auto* presentation=rhi::D3D12DiagnosticsAccess::Presentation(device);auto old=rhi::D3D12DiagnosticsAccess::States(device)->Find(*presentation->hdr.Get());
    Check(old&&old->attached,"HDR native lifetime observed without owning resource");
    Microsoft::WRL::ComPtr<ID3D12Fence> gate;Check(SUCCEEDED(rhi::D3D12DiagnosticsAccess::NativeDevice(device)->CreateFence(0,D3D12_FENCE_FLAG_NONE,IID_PPV_ARGS(gate.GetAddressOf()))),"HDR gate");
    struct Release
    {
        Microsoft::WRL::ComPtr<ID3D12Fence> fence;HANDLE event;std::thread worker;std::atomic<bool> observed{false};
        Release(Microsoft::WRL::ComPtr<ID3D12Fence> value,std::shared_ptr<const D3D12StateRecord> state):fence(std::move(value)),event(CreateEventW(nullptr,TRUE,FALSE,nullptr))
        {Check(event!=nullptr,"HDR gate event");worker=std::thread([this,state]{WaitForSingleObject(event,3000);Sleep(80);observed=state->attached.load();fence->Signal(1);});}
        ~Release(){fence->Signal(1);SetEvent(event);if(worker.joinable())worker.join();CloseHandle(event);}
    } release{gate,old};
    Check(SUCCEEDED(presentation->queue->Wait(gate.Get(),1)),"hold HDR sample and output in flight");
    auto commands=device.BeginCommands();Ok(RecordForwardFrame(*commands,lighting,*f.output,*f.sky,{}),"gated Forward packet");rhi::CompletionToken token;
    Ok(device.Submit(*commands,token),"HDR packet in flight");Check(!device.IsComplete(token),"HDR Fence demonstrably unfinished");SetEvent(release.event);
    Ok(device.Resize(512,256),"resize waits before releasing in-flight HDR");
    Check(release.observed&&device.IsComplete(token)&&!old->attached&&!presentation->hdr,"old HDR alive before gate release and destroyed only after completion");
    auto image=Capture(f,lighting,{});Check(image.hdr.width==512&&device.Stats().debugWarnings==0&&device.Stats().debugErrors==0,"fresh HDR after gated resize is valid");
}
void Sample(const Localization& text,const fs::path& output)
{
    Fixture f(text);auto& device=*f.device;f.Size(960,720);Ok(device.Resize(960,720),"Lighting sample size");
    MaterialsDemo demo("en-US",true);auto services=demo.Services();Check(services.windowOpened(f.window.hwnd)&&services.update(0),"Lighting World setup");
    auto sphere=Upload(f,false),cube=Upload(f,false,true);Ok(device.EnableGpuTiming(true),"Lighting sample GPU queries");
    const auto render=[&](std::uint64_t source){
        FrameData data;services.extract(data);Check(data.meshes.size()==28&&data.lights.size()==4,"Lighting sample real World light extraction");
        auto camera=SelectCamera(data.cameras,{0,0,960,720},source).snapshot;Check(camera.has_value(),"Lighting sample camera");
        std::vector<AssetDiagnostic> diagnostics;auto lighting=PrepareForwardLighting(data.lights,data.exposure,data.skyColor,diagnostics);std::vector<rhi::DrawIndexed> draws;
        for(const auto& instance:data.meshes)
        {
            std::vector<std::shared_ptr<const PbrRenderMaterial>> overrides;
            for(const auto& id:instance.mesh.materials){overrides.push_back(f.cache.LoadMaterial(*data.assets,id,device,diagnostics));Check(bool(overrides.back()),"Lighting override");}
            AppendPbrModelDraws(instance.mesh.meshAsset==BuiltinSphereAssetId?*sphere:*cube,instance.world,overrides,draws,*f.mesh,camera->viewProjection,camera->position,{0,0,1},{});
        }
        return Capture(f,lighting,draws,source);
    };
    auto lit=render(1);Save(lit.output,output/"lighting.ppm");
    InputState input;input.hasFocus=true;input.keyEvents={{KeyEventType::Pressed,'P'}};services.input(input,false,false);Check(services.update(0),"Lighting point toggle");
    auto noPoints=render(2);Save(noPoints.output,output/"directional-only.ppm");Check(lit.hdr.rgba32!=noPoints.hdr.rgba32,"point lights visibly contribute");
    input.keyEvents={{KeyEventType::Pressed,'L'}};services.input(input,false,false);Check(services.update(0),"Lighting directional toggle");
    auto unlit=render(3);Check(unlit.hdr.rgba32!=noPoints.hdr.rgba32,"directional light visibly contributes");
    input.keyEvents={{KeyEventType::Pressed,VK_OEM_PLUS}};services.input(input,false,false);Check(services.update(0),"Lighting exposure control");
    auto exposed=render(4);Check(exposed.hdr.rgba32==unlit.hdr.rgba32&&exposed.output.rgba8!=unlit.output.rgba8,"sample exposure affects final output only");
    input.keyEvents={{KeyEventType::Pressed,'P'},{KeyEventType::Pressed,'L'}};input.keysDown['W']=true;services.input(input,false,false);Check(services.update(0.1),"Lighting camera and animated lights");
    auto moved=render(5);Check(moved.hdr.rgba32!=lit.hdr.rgba32,"moving camera/lights changes actual scene");
    Ok(device.ExportGpuPassTimingsCsv(output/"lighting-passes.csv"),"Lighting sample per-pass timing");
    Check(device.Stats().debugWarnings==0&&device.Stats().debugErrors==0,"Lighting sample GPU warnings/errors zero");services.windowClosing();
}
}
int wmain(int argc,wchar_t** argv)
{
    try
    {
        Check(argc==2,"supply Forward output directory");const auto output=fs::absolute(argv[1]);fs::create_directories(output);
        Localization text;std::string error;Check(text.Load(GetExecutableDirectory()/"Resources/Localization/en-US.txt",error),"Forward strings");
        Cpu();Gpu(text,output);GatedResize(text);Sample(text,output);std::cout<<"ForwardTests passed (bounded World lights, HDR oracle, output transfer, explicit passes, gated resize, completed timestamps and actual Lighting World controls); GPU warnings/errors=0.\n";return 0;
    }
    catch(const std::exception& error){std::cerr<<"ForwardTests: "<<error.what()<<'\n';return 1;}
}
