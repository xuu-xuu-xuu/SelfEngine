#include "FunctionLayer/Rendering/ShadowSystem.h"
#include "FunctionLayer/Rendering/ForwardRenderer.h"
#include "FunctionLayer/Rendering/StaticModelRender.h"
#include "FunctionLayer/Runtime/ComponentMetadata.h"
#include "ResourceLayer/Scenes/SceneSerializer.h"
#include "ResourceLayer/Assets/Primitives.h"
#include "PlatformLayer/Windows/Window.h"
#include "PlatformLayer/Windows/ExecutablePath.h"
#include "PlatformLayer/Graphics/D3D12/D3D12Diagnostics.h"
#include "CoreLayer/Localization.h"
#include <fstream>
#include <atomic>
#include <thread>
#include "PlatformLayer/Graphics/D3D12/D3D12Presentation.h"
#include "PlatformLayer/Graphics/D3D12/D3D12Shadow.h"
#include "Shadows/ShadowsDemo.h"
#include <d3d12sdklayers.h>
#include <nlohmann/json.hpp>
#include <iostream>
#include <stdexcept>

using namespace se;
namespace
{
void Check(bool value,const char* message){if(!value)throw std::runtime_error(message);}
void Ok(rhi::Result result,const char* message){if(!result)throw std::runtime_error(std::string(message)+": "+result.Key());}
struct Captured{rhi::Image image;rhi::HdrImage hdr;rhi::DepthImage map;};
void Save(const rhi::Image& image,const std::filesystem::path& path)
{
    std::ofstream stream(path,std::ios::binary);stream<<"P6\n"<<image.width<<' '<<image.height<<"\n255\n";
    for(size_t index=0;index<image.rgba8.size();index+=4)stream.write(reinterpret_cast<const char*>(image.rgba8.data()+index),3);Check(bool(stream),"save shadow color image");
}
void SaveDepth(const rhi::DepthImage& image,const std::filesystem::path& path)
{
    std::ofstream stream(path,std::ios::binary);stream<<"Pf\n"<<image.width<<' '<<image.height<<"\n-1\n";
    for(UINT row=image.height;row>0;--row)stream.write(reinterpret_cast<const char*>(image.depth.data()+size_t(row-1)*image.width),static_cast<std::streamsize>(size_t(image.width)*sizeof(float)));
    Check(bool(stream),"save actual R32 depth as little-endian PFM");
}
bool RayBox(Vec3 position,Vec3 direction,Vec3 minimum,Vec3 maximum)
{
    double first=0,last=1e9;const double p[]{position.x,position.y,position.z},d[]{direction.x,direction.y,direction.z},lo[]{minimum.x,minimum.y,minimum.z},hi[]{maximum.x,maximum.y,maximum.z};
    for(size_t axis=0;axis<3;++axis)
    {
        if(std::abs(d[axis])<1e-8){if(p[axis]<lo[axis]||p[axis]>hi[axis])return false;continue;}
        double a=(lo[axis]-p[axis])/d[axis],b=(hi[axis]-p[axis])/d[axis];if(a>b)std::swap(a,b);first=std::max(first,a);last=std::min(last,b);if(first>last)return false;
    }
    return last>0;
}
void Cpu()
{
    DirectionalShadowView view;rhi::ForwardLighting light;light.directionalEnabled=true;
    rhi::ShadowSettings settings;std::vector<AssetDiagnostic> diagnostics;
    Check(PrepareDirectionalShadow(view,light,settings,diagnostics),"prepare default orthographic light camera");
    const auto origin=settings.clipFromWorld*Vec4{0,0,0,1};
    Check(std::abs(origin.x)<1e-6f&&std::abs(origin.y)<1e-6f&&origin.z>0&&origin.z<1&&origin.w==1,"world center projects inside shadow map");
    const auto closer=settings.clipFromWorld*Vec4{0,0,1,1};Check(closer.z<origin.z,"toward light means smaller ordinary depth");
    const auto initial=settings.clipFromWorld.values;view.center={0.001f,0.002f,0};
    Check(PrepareDirectionalShadow(view,light,settings,diagnostics)&&settings.clipFromWorld.values==initial,"sub-texel translations snap to identical light projection");
    for(const Vec3 direction: {Vec3{1,1,1},Vec3{0,1,0},Vec3{0,-1,0},Vec3{0,0,-1}})
    {
        light.directionToLight=Normalize(direction);Check(PrepareDirectionalShadow(view,light,settings,diagnostics),"vertical and reversed light directions produce finite rigid view");
        const auto nearPoint=settings.clipFromWorld*Vec4{direction.x,direction.y,direction.z,1},center=settings.clipFromWorld*Vec4{0,0,0,1};
        Check(nearPoint.z<center.z,"depth direction follows selected light");
    }
    const auto previous=settings.clipFromWorld.values;auto invalid=view;invalid.resolution=1000;
    Check(!PrepareDirectionalShadow(invalid,light,settings,diagnostics)&&settings.clipFromWorld.values==previous,"unsupported shadow resolution rejected atomically");
    invalid=view;invalid.depthBias=0.03f;Check(!PrepareDirectionalShadow(invalid,light,settings,diagnostics),"excessive bias rejected");
    invalid=view;invalid.lightDistance=1;Check(!PrepareDirectionalShadow(invalid,light,settings,diagnostics),"light near plane cannot cut declared volume");
    light.directionalEnabled=false;Check(PrepareDirectionalShadow(view,light,settings,diagnostics)&&!settings.enabled,"absent main light disables shadow");
    World world;const auto entity=world.Create("Caster",42);Check(world.SetMesh(entity,{"mesh",{},false,true}),"mesh shadow flags");
    PropertyValue value;std::string error,bytes;Check(ReadProperty(world,entity,"mesh.castsShadow",value,error)&&!std::get<bool>(value),"caster metadata");
    Check(WriteProperty(world,entity,"mesh.receivesShadow",false,error),"receiver property edit");
    FrameData frame;BuildFrameData(world,nullptr,nullptr,frame);Check(frame.meshes.size()==1&&!frame.meshes[0].mesh.castsShadow&&!frame.meshes[0].mesh.receivesShadow,"immutable snapshot carries explicit flags");
    Check(SerializeScene(world,bytes,error),"save shadow flags");World restored;Check(DeserializeScene(bytes,restored,error),"read shadow flags");
    Check(!restored.GetMesh(restored.FindByStableId(42))->castsShadow&&!restored.GetMesh(restored.FindByStableId(42))->receivesShadow,"flags survive scene roundtrip");
    auto json=nlohmann::json::parse(bytes);auto& legacyEntity=json["entities"][0];
    const auto meshProperties=legacyEntity["components"][1]["properties"];legacyEntity["transform"]=legacyEntity["components"][0]["properties"];
    legacyEntity["components"]={{"mesh",meshProperties}};json["version"]=1;
    auto& mesh=legacyEntity["components"]["mesh"];mesh.erase("castsShadow");mesh.erase("receivesShadow");
    Check(DeserializeScene(json.dump(),restored,error)&&restored.GetMesh(restored.FindByStableId(42))->castsShadow&&restored.GetMesh(restored.FindByStableId(42))->receivesShadow,"old scene defaults to cast and receive");
    mesh["castsShadow"]="invalid";Check(!DeserializeScene(json.dump(),restored,error),"malformed flag rejected");
}
void Gpu(const Localization& text,const std::filesystem::path& output)
{
    WindowState window;std::shared_ptr<rhi::Device> device;StaticModelRenderCache cache{true};
    struct Cleanup{WindowState& window;std::shared_ptr<rhi::Device>& device;~Cleanup(){if(device)device->Shutdown();window.postQuitOnDestroy=false;DestroyEngineWindow(window);}} cleanup{window,device};
    Check(CreateEngineWindow(window,false),"shadow hidden window");RECT rect{0,0,320,240};AdjustWindowRectEx(&rect,static_cast<DWORD>(GetWindowLongPtrW(window.hwnd,GWL_STYLE)),FALSE,0);
    Check(SetWindowPos(window.hwnd,nullptr,0,0,rect.right-rect.left,rect.bottom-rect.top,SWP_NOMOVE|SWP_NOZORDER|SWP_NOACTIVATE)!=FALSE,"shadow window size");int exit=0;Check(PumpWindowMessages(exit),"shadow messages");
    rhi::Result result;device=rhi::Device::CreateD3D12({window.hwnd,320,240},text,result);Ok(result,"shadow device");
    rhi::Pipeline mesh,depth,final,sky;rhi::PipelineDescription description{rhi::PipelineKind::ShadowedMesh,GetExecutableDirectory()/"Resources/Shaders/ShadowedForward.hlsl",4};
    description.colorFormat=rhi::ColorFormat::Rgba16Float;Ok(device->CreatePipeline(description,mesh),"shadowed PBR pipeline");
    description={rhi::PipelineKind::ShadowDepth,GetExecutableDirectory()/"Resources/Shaders/ShadowDepth.hlsl",4};Ok(device->CreatePipeline(description,depth),"depth only PSO");
    description={rhi::PipelineKind::ShadowOutput,GetExecutableDirectory()/"Resources/Shaders/ShadowOutput.hlsl",4};Ok(device->CreatePipeline(description,final),"shadow preview PSO");
    description={rhi::PipelineKind::Sky,GetExecutableDirectory()/"Resources/Shaders/Sky.hlsl",9};description.colorFormat=rhi::ColorFormat::Rgba16Float;Ok(device->CreatePipeline(description,sky),"shadow sky PSO");
    Check(device->Stats().shaderArtifactLoads==4&&device->Stats().shaderProgramCompilations==0,"shadow uses all built artifacts");
    auto source=std::make_shared<StaticModel>();source->meshes.push_back(MakeUnitCube());source->meshes[0].material=0;source->materials.emplace_back();ModelNode node;node.name="Caster";node.meshes={0};source->nodes.push_back(node);
    std::vector<AssetDiagnostic> diagnostics;std::shared_ptr<const StaticRenderModel> cube;
    Check(cache.Upload(source,{},std::filesystem::current_path(),*device,cube,diagnostics),"shadow cube upload");
    std::vector<rhi::DrawIndexed> draws;AppendPbrModelDraws(*cube,{}, {},draws,*mesh,*OrthographicRH(4,3,0.1f,100)*Translation({0,0,-3}),{0,0,3},{0,0,1},{});
    rhi::ForwardLighting light;light.directionalEnabled=true;light.directionalRadiance={2,2,2};DirectionalShadowView view;rhi::ShadowSettings shadows;
    Check(PrepareDirectionalShadow(view,light,shadows,diagnostics),"shadow light matrix");Ok(device->EnableGpuTiming(true),"shadow timestamp queries");
    auto casters=draws;
    const auto capture=[&](std::uint64_t frame){
        auto commands=device->BeginCommands(frame);Ok(RecordShadowedFrame(*commands,light,shadows,*depth,*final,*sky,draws,casters),"record Clear/Shadow/Sky/Opaque/Output");
        Captured image;rhi::CompletionToken token;
        Ok(device->Submit(*commands,token,&image.image,&image.hdr,&image.map),"capture actual shadow and HDR");return image;
    };
    const auto initial=capture(1);const auto& map=initial.map;Check(map.width==1024&&map.height==1024&&map.depth.size()==1024*1024,"actual shadow dimensions");
    const auto expected=shadows.clipFromWorld*Vec4{0,0,0.5f,1};
    Check(std::abs(map.depth[512*1024+512]-expected.z/expected.w)<1e-6f,"actual center depth matches nearest cube face in light space");
    Check(map.depth[0]==1&&map.depth.back()==1,"unused shadow texels clear to one");
    shadows.enabled=false;const auto disabled=capture(2);Check(std::all_of(disabled.map.depth.begin(),disabled.map.depth.end(),[](float value){return value==1;}),"disabled shadow map is initialized white");
    Ok(device->ExportGpuPassTimingsCsv(output/"passes.csv"),"shadow timing CSV");const auto timings=device->GpuTimings();
    Check(timings.size()==2&&timings[0].shadow&&timings[1].shadow,"Shadow pass queries collected including tail");
    // 相机看不到 x=2 的方块，其朝原点投下的阴影仍须由独立投射者列表绘制。
    const Mat4 topCamera{{1,0,0,0,0,0,-1,0,0,1,0,0,0,5,0,1}};
    const auto vp=*OrthographicRH(2,1.5f,0.1f,100) * *InverseRigid(topCamera);
    const auto floorWorld=Translation({0,-0.05f,0})*Scaling({6,0.1f,6});draws.clear();
    AppendPbrModelDraws(*cube,floorWorld,{},draws,*mesh,vp,{0,5,0},{0,0,1},{});draws[0].castsShadow=false;draws[0].tint={0.5f,0.5f,0.5f,1};
    casters=draws;casters[0].world=Translation({2,1,0});casters[0].castsShadow=true;
    const auto outside=vp*Vec4{1.5f,0.5f,-0.5f,1};Check(outside.x/outside.w>1,"entire caster lies outside main camera frustum");
    light.directionToLight=Normalize(Vec3{4,2,0});view.enabled=true;view.pcf=false;view.normalBias=view.slopeBias=0;view.depthBias=0.0003f;
    Check(PrepareDirectionalShadow(view,light,shadows,diagnostics),"prepare independent caster reference");
    const auto hard=capture(3);shadows.enabled=false;const auto plain=capture(4);Save(hard.image,output/"hard.ppm");Save(plain.image,output/"disabled.ppm");SaveDepth(hard.map,output/"depth.pfm");
    size_t checked=0,occluded=0,lit=0;std::ofstream csv(output/"geometry-oracle.csv");csv<<"x,y,world_x,world_z,ray_occluded,hdr_shadow,hdr_without\n";
    for(UINT y=16;y<224;y+=8)for(UINT x=16;x<304;x+=8)
    {
        const Vec3 point{(float(x)+0.5f)/320*2-1,0,(float(y)+0.5f)/240*1.5f-0.75f};
        if(std::abs(std::abs(point.z)-0.5f)<0.04f)continue;
        const bool blocked=RayBox(point,light.directionToLight,{1.5f,0.5f,-0.5f},{2.5f,1.5f,0.5f});const auto index=(size_t(y)*320+x)*4;
        const float without=plain.hdr.rgba32[index],with=hard.hdr.rgba32[index];Check(without>0.01f,"reference floor is actually lit");
        const bool mismatch=blocked?!(with<without*0.02f):!(std::abs(with-without)<0.003f);
        if(mismatch)std::cerr<<"shadow oracle mismatch x="<<x<<" y="<<y<<" blocked="<<blocked
            <<" with="<<with<<" without="<<without<<'\n';
        Check(blocked?with<without*0.02f:std::abs(with-without)<0.003f,"GPU shadow matches independent ray/AABB oracle away from edges");
        ++checked;if(blocked)++occluded;else ++lit;csv<<x<<','<<y<<','<<point.x<<','<<point.z<<','<<blocked<<','<<with<<','<<without<<'\n';
    }
    Check(checked>500&&occluded>100&&lit>100&&bool(csv),"geometry oracle samples lit and shadow regions");
    shadows.enabled=true;shadows.pcf=true;const auto filtered=capture(5);Save(filtered.image,output/"pcf.ppm");size_t partial=0;
    for(size_t index=0;index<filtered.hdr.rgba32.size();index+=4)if(plain.hdr.rgba32[index]>0.01f)
    {const float ratio=filtered.hdr.rgba32[index]/plain.hdr.rgba32[index];if(ratio>0.05f&&ratio<0.95f)++partial;}
    Check(partial>100,"3x3 PCF produces intermediate visibility at hard edge");
    draws[0].pbr.receivesShadow=false;const auto receiverOff=capture(6);Check(receiverOff.hdr.rgba32==plain.hdr.rgba32,"receiver flag disables only receiving");draws[0].pbr.receivesShadow=true;
    casters[0].castsShadow=false;const auto casterOff=capture(7);Check(casterOff.hdr.rgba32==plain.hdr.rgba32,"caster flag removes shadow");casters[0].castsShadow=true;
    shadows.preview=true;const auto preview=capture(8);Save(preview.image,output/"preview.ppm");Check(preview.hdr.rgba32==filtered.hdr.rgba32,"depth preview does not change HDR scene");
    for(const auto pixel: {std::pair{0u,0u},std::pair{160u,120u},std::pair{319u,239u}})
    {
        const UINT mx=static_cast<UINT>((float(pixel.first)+0.5f)/320*shadows.resolution),my=static_cast<UINT>((float(pixel.second)+0.5f)/240*shadows.resolution);
        const int expectedCode=static_cast<int>(std::lround(preview.map.depth[size_t(my)*shadows.resolution+mx]*255));
        for(UINT channel=0;channel<3;++channel)Check(std::abs(int(preview.image.rgba8[(size_t(pixel.second)*320+pixel.first)*4+channel])-expectedCode)<=1,"preview displays actual nearest raw depth");
    }
    shadows.preview=false;casters[0].world=Translation({2,1,1});const auto moved=capture(9);const size_t center=(size_t(120)*320+160)*4;
    Check(moved.hdr.rgba32[center]>plain.hdr.rgba32[center]*0.95f&&moved.map.depth!=hard.map.depth,"moving invisible caster moves shadow");
    casters[0].world=Translation({2,1,0});light.directionToLight=Normalize(Vec3{-4,2,0});Check(PrepareDirectionalShadow(view,light,shadows,diagnostics),"rotate sun matrix");
    const auto rotated=capture(10);Check(rotated.hdr.rgba32[center]>0.01f&&rotated.map.depth!=hard.map.depth,"rotating light moves shadow outside camera");
    auto floorCaster=draws[0];floorCaster.castsShadow=true;casters.push_back(floorCaster);view.depthBias=0.0008f;view.slopeBias=0.002f;view.normalBias=0.015f;view.pcf=true;
    Check(PrepareDirectionalShadow(view,light,shadows,diagnostics),"default bias reference");const auto self=capture(11);shadows.enabled=false;const auto selfPlain=capture(12);size_t acne=0;
    for(size_t index=0;index<self.hdr.rgba32.size();index+=4)if(self.hdr.rgba32[index]<selfPlain.hdr.rgba32[index]*0.95f)++acne;
    Check(acne<320*240/100,"flat caster/receiver has less than one percent erroneous dark samples");
    light.directionalRadiance={};light.pointCount=1;light.points[0]={{0,2,0},{1,2,3},5};shadows.enabled=true;const auto pointOn=capture(13);shadows.enabled=false;const auto pointOff=capture(14);
    Check(pointOn.hdr.rgba32==pointOff.hdr.rgba32,"directional shadow does not alter point light contribution");
    Ok(device->ExportGpuPassTimingsCsv(output/"geometry-passes.csv"),"geometry shadow timings");
    light.pointCount=0;light.directionalRadiance={2,2,2};shadows.enabled=true;
    Ok(device->WaitIdle(),"before controlled shadow version switch");
    auto* presentation=rhi::D3D12DiagnosticsAccess::Presentation(*device);Microsoft::WRL::ComPtr<ID3D12Fence> gate;
    Check(SUCCEEDED(rhi::D3D12DiagnosticsAccess::NativeDevice(*device)->CreateFence(0,D3D12_FENCE_FLAG_NONE,IID_PPV_ARGS(gate.GetAddressOf()))),"shadow resolution gate");
    struct Gate
    {
        Microsoft::WRL::ComPtr<ID3D12Fence> fence;HANDLE event;std::thread worker;std::atomic<bool> timedOut{false};
        explicit Gate(Microsoft::WRL::ComPtr<ID3D12Fence> value):fence(std::move(value)),event(CreateEventW(nullptr,TRUE,FALSE,nullptr))
        {Check(event!=nullptr,"shadow gate event");worker=std::thread([this]{if(WaitForSingleObject(event,3000)==WAIT_TIMEOUT){timedOut=true;fence->Signal(1);}});}
        void Release(){fence->Signal(1);SetEvent(event);}
        ~Gate(){Release();if(worker.joinable())worker.join();CloseHandle(event);}
    } release{gate};
    Check(SUCCEEDED(presentation->queue->Wait(gate.Get(),1)),"hold both shadow versions on actual queue");
    const auto waits=device->Stats().globalWaits;auto smallSettings=shadows;smallSettings.resolution=512;
    auto packet=device->BeginCommands(30);Ok(RecordShadowedFrame(*packet,light,smallSettings,*depth,*final,*sky,draws,casters),"record old shadow resolution");
    rhi::CompletionToken oldToken,newToken;Ok(device->Submit(*packet,oldToken),"submit old resolution in flight");
    auto oldState=rhi::D3D12DiagnosticsAccess::States(*device)->Find(*presentation->shadow->depth.Get());const auto oldSlot=presentation->shadow->srv->Cpu().ptr;
    auto largeSettings=smallSettings;largeSettings.resolution=2048;packet=device->BeginCommands(31);Ok(RecordShadowedFrame(*packet,light,largeSettings,*depth,*final,*sky,draws,casters),"record replacement shadow resolution");
    Ok(device->Submit(*packet,newToken),"submit new resolution without draining old version");
    Check(!device->IsComplete(oldToken)&&!device->IsComplete(newToken)&&!release.timedOut&&oldState->attached
        &&presentation->shadow->resolution==2048&&presentation->shadow->srv->Cpu().ptr!=oldSlot&&device->Stats().globalWaits==waits,"in-flight old shadow and SRV survive replacement without global wait or slot reuse");
    release.Release();Ok(device->WaitIdle(),"finish both shadow versions");device->Collect();Check(!oldState->attached,"old shadow actually destroyed after Fence");
    shadows=smallSettings;capture(32);Check(presentation->shadow->srv->Cpu().ptr==oldSlot,"completed shadow CPU descriptor slot is reused");
    const auto baselineResources=device->Stats().memoryResources;const auto baselineDescriptors=device->Stats().persistentDescriptors;
    for(UINT cycle=0;cycle<4;++cycle)
    {
        const UINT width=cycle%2?320:512,height=cycle%2?240:256;RECT size{0,0,LONG(width),LONG(height)};
        AdjustWindowRectEx(&size,static_cast<DWORD>(GetWindowLongPtrW(window.hwnd,GWL_STYLE)),FALSE,0);
        Check(SetWindowPos(window.hwnd,nullptr,0,0,size.right-size.left,size.bottom-size.top,SWP_NOMOVE|SWP_NOZORDER|SWP_NOACTIVATE)!=FALSE,"shadow window resize");Check(PumpWindowMessages(exit),"shadow resize messages");
        Ok(device->Resize(width,height),"resize HDR and main depth with independent shadow");const auto resized=capture(40+cycle);
        Check(resized.image.width==width&&resized.hdr.height==height&&resized.map.width==512&&device->Stats().memoryResources==baselineResources
            &&device->Stats().persistentDescriptors==baselineDescriptors,"window resize keeps fixed shadow size and resource counts stable");
        Ok(device->Resize(0,0),"defer minimized surface");Check(device->Stats().shadowResolution==512,"minimize does not invalidate shadow target");
    }
    const auto ordinary=device->Stats();
    for(std::uint64_t frame=100;frame<132;++frame)
    {auto commands=device->BeginCommands(frame);Ok(RecordShadowedFrame(*commands,light,shadows,*depth,*final,*sky,draws,casters),"ordinary shadow frame");rhi::CompletionToken token;Ok(device->Submit(*commands,token),"ordinary shadow submit");}
    Check(device->Stats().globalWaits==ordinary.globalWaits&&device->Stats().shadowCreations==ordinary.shadowCreations
        &&device->Stats().pipelineCreations==ordinary.pipelineCreations&&device->Stats().shaderProgramCompilations==ordinary.shaderProgramCompilations,"ordinary shadow frames neither flush nor recreate target/PSO/program");
    Ok(device->ExportGpuPassTimingsCsv(output/"stress-passes.csv"),"stress shadow timings");
    // 与实际示例共用 World/材质/相机/输入，所有输出仍由实际 RHI 提交与读回。
    RECT big{0,0,960,720};AdjustWindowRectEx(&big,static_cast<DWORD>(GetWindowLongPtrW(window.hwnd,GWL_STYLE)),FALSE,0);
    Check(SetWindowPos(window.hwnd,nullptr,0,0,big.right-big.left,big.bottom-big.top,SWP_NOMOVE|SWP_NOZORDER|SWP_NOACTIVATE)!=FALSE,"Shadows sample dimensions");Check(PumpWindowMessages(exit),"sample messages");Ok(device->Resize(960,720),"sample HDR size");
    auto sphereSource=std::make_shared<StaticModel>(*source);sphereSource->meshes[0]=MakeUnitSphere();sphereSource->meshes[0].material=0;std::shared_ptr<const StaticRenderModel> sphere;
    Check(cache.Upload(sphereSource,{},std::filesystem::current_path(),*device,sphere,diagnostics),"sample sphere upload");
    ShadowsDemo demo("en-US");auto services=demo.Services();Check(services.windowOpened(window.hwnd)&&services.update(0),"actual Shadows World setup");
    const auto scene=[&](std::uint64_t frame){
        FrameData data;services.extract(data);Check(data.meshes.size()==3&&data.lights.size()==2,"Shadows World extracts ground, cube, sphere and two lights");
        auto camera=SelectCamera(data.cameras,{0,0,960,720},frame).snapshot;Check(camera.has_value(),"sample camera");
        light=PrepareForwardLighting(data.lights,data.exposure,data.skyColor,diagnostics);Check(PrepareDirectionalShadow(data.shadow,light,shadows,diagnostics),"sample shadow view");draws.clear();
        for(const auto& instance:data.meshes)
        {
            std::vector<std::shared_ptr<const PbrRenderMaterial>> overrides;for(const auto& id:instance.mesh.materials){overrides.push_back(cache.LoadMaterial(*data.assets,id,*device,diagnostics));Check(bool(overrides.back()),"sample material override");}
            const auto first=draws.size();AppendPbrModelDraws(instance.mesh.meshAsset==BuiltinSphereAssetId?*sphere:*cube,instance.world,overrides,draws,*mesh,camera->viewProjection,camera->position,{0,0,1},{});
            for(auto index=first;index<draws.size();++index){draws[index].castsShadow=instance.mesh.castsShadow;draws[index].pbr.receivesShadow=instance.mesh.receivesShadow;}
        }
        casters=draws;return capture(frame);
    };
    const auto sample=scene(200);Save(sample.image,output/"shadows.ppm");SaveDepth(sample.map,output/"sample-depth.pfm");
    InputState input;input.hasFocus=true;input.keyEvents={{KeyEventType::Pressed,'H'}};services.input(input,false,false);Check(services.update(0),"sample shadow toggle");
    const auto sampleOff=scene(201);Save(sampleOff.image,output/"shadows-off.ppm");size_t darkened=0;
    for(size_t index=0;index<sample.hdr.rgba32.size();index+=4)
    {Check(sample.hdr.rgba32[index]<=sampleOff.hdr.rgba32[index]+0.003f,"enabling shadows only reduces directional lighting");if(sampleOff.hdr.rgba32[index]-sample.hdr.rgba32[index]>0.02f)++darkened;}
    Check(darkened>100,"real World has visible cast shadows");
    input.keyEvents={{KeyEventType::Pressed,'H'},{KeyEventType::Pressed,VK_F2}};services.input(input,false,false);Check(services.update(0),"sample depth preview control");
    const auto samplePreview=scene(202);Save(samplePreview.image,output/"sample-preview.ppm");Check(samplePreview.hdr.rgba32==sample.hdr.rgba32,"sample preview only changes final visualization");
    input.keyEvents={{KeyEventType::Pressed,VK_F2},{KeyEventType::Pressed,VK_F3}};services.input(input,false,false);Check(services.update(0),"sample PCF control");
    const auto samplePoint=scene(203);Check(samplePoint.map.depth==sample.map.depth,"PCF toggle does not rerender different geometry");
    input.keyEvents={{KeyEventType::Pressed,'3'},{KeyEventType::Pressed,VK_RIGHT}};services.input(input,false,false);Check(services.update(0),"sample resolution and sun controls");
    const auto sampleLarge=scene(204);Check(sampleLarge.map.width==2048&&sampleLarge.hdr.rgba32!=sample.hdr.rgba32,"sample resolution and moving sun affect actual render");
    Ok(device->ExportGpuPassTimingsCsv(output/"sample-passes.csv"),"sample shadow timings");services.windowClosing();
    const auto stats=device->Stats();if(stats.debugWarnings||stats.debugErrors)
    {
        Microsoft::WRL::ComPtr<ID3D12InfoQueue> queue;rhi::D3D12DiagnosticsAccess::NativeDevice(*device)->QueryInterface(IID_PPV_ARGS(queue.GetAddressOf()));
        for(UINT64 index=0;queue&&index<queue->GetNumStoredMessages();++index)
        {SIZE_T bytes=0;queue->GetMessage(index,nullptr,&bytes);std::vector<std::byte> storage(bytes);auto* message=reinterpret_cast<D3D12_MESSAGE*>(storage.data());
            if(SUCCEEDED(queue->GetMessage(index,message,&bytes))&&message->Severity<=D3D12_MESSAGE_SEVERITY_WARNING)std::cerr<<"D3D12 "<<message->ID<<": "<<message->pDescription<<'\n';}
    }
    Check(stats.debugWarnings==0&&stats.debugErrors==0,"shadow GPU warnings/errors zero");
}
}
int wmain(int argc,wchar_t** argv)
{
    try
    {
        Check(argc==2,"supply shadow output directory");const auto output=std::filesystem::absolute(argv[1]);std::filesystem::create_directories(output);
        Localization text;std::string error;Check(text.Load(GetExecutableDirectory()/"Resources/Localization/en-US.txt",error),"shadow strings");
        Cpu();Gpu(text,output);std::cout<<"ShadowTests passed (light camera, depth and ray oracle, independent casters, bias/PCF/preview, flags, in-flight resolution switch, resize and World controls); GPU warnings/errors=0.\n";return 0;
    }
    catch(const std::exception& error){std::cerr<<"ShadowTests: "<<error.what()<<'\n';return 1;}
}
