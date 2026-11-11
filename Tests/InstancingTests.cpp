#include "FunctionLayer/Rendering/Instancing.h"
#include "FunctionLayer/Rendering/SandboxRenderer.h"
#include "FunctionLayer/Application/SandboxApp.h"
#include "FunctionLayer/Application/FrameData.h"
#include "FunctionLayer/Runtime/World.h"
#include "Benchmark/BenchmarkScene.h"
#include "PlatformLayer/Graphics/D3D12/D3D12Diagnostics.h"
#include "PlatformLayer/Graphics/D3D12/D3D12Presentation.h"
#include "PlatformLayer/Windows/Window.h"
#include "PlatformLayer/Windows/ExecutablePath.h"
#include "ResourceLayer/Assets/Primitives.h"
#include "ResourceLayer/Assets/AssetIO.h"
#include "ResourceLayer/Assets/ImageWriter.h"
#include "CoreLayer/Localization.h"
#include <algorithm>
#include <cstring>
#include <cstdlib>
#include <iostream>
#include <limits>
#include <stdexcept>

using namespace se;
namespace
{
void Check(bool value,const char* message){if(!value)throw std::runtime_error(message);}
void Ok(rhi::Result value,const char* message){if(!value)throw std::runtime_error(std::string(message)+": "+value.Key());}
void Cpu()
{
    PbrInstanceBatcher batcher;rhi::DrawIndexed draw;draw.pipeline={1,2,1};draw.texture={1,3,1};draw.vertices={1,4,1};draw.indices={1,5,1};
    std::vector<rhi::DrawIndexed> inputs;
    for(int index=0;index<1000;++index){auto item=draw;item.world=Translation({float(index),0,0});inputs.push_back(item);}
    auto batches=batcher.Build(inputs);Check(batches.size()==1&&batches[0].instances.size()==1000,"compatible parts become one actual batch");
    for(size_t index=0;index<1000;++index)Check(batches[0].instances[index].world(0,3)==float(index),"instance order preserves transform association");
    const auto capacity=batcher.InstanceCapacity();inputs.resize(17);batches=batcher.Build(inputs);Check(batches.size()==1&&batcher.InstanceCapacity()==capacity,"smaller visible list reuses CPU capacity");
    for(int variant=0;variant<6;++variant){auto changed=draw;switch(variant){case 0:++changed.pipeline.generation;break;case 1:++changed.texture.index;break;case 2:changed.tint.x=0.7f;break;
        case 3:++changed.vertices.index;break;case 4:changed.pbr.radiance.y=4;break;default:changed.viewProjection(0,0)=2;break;}const std::array pair{draw,changed};Check(batcher.Build(pair).size()==2,"different PSO/texture/material/mesh/shared light/view never merges");}
    Check(batcher.Build({}).empty(),"zero visible instances produces zero batches");batcher.Clear();Check(batcher.InstanceCapacity()==0,"shutdown clears CPU cache");
}
struct Fixture
{
    WindowState window;std::shared_ptr<rhi::Device> device;StaticModelRenderCache models{true};std::shared_ptr<const StaticRenderModel> model;
    rhi::Pipeline classic,instanced;rhi::Texture normal;std::uint64_t frame=0;UINT width=320,height=240;
    explicit Fixture(const Localization& text)
    {
        Check(CreateEngineWindow(window,false)&&SetEngineWindowClientSize(window,width,height),"instance hidden window");int exit=0;Check(PumpWindowMessages(exit),"instance messages");rhi::Result result;
        device=rhi::Device::CreateD3D12({window.hwnd,width,height},text,result);Ok(result,"instance device");const auto shaders=GetExecutableDirectory()/"Resources/Shaders";
        Ok(device->CreatePipeline({rhi::PipelineKind::PbrMesh,shaders/"Pbr.hlsl",2},classic),"classic PBR pipeline");
        Ok(device->CreatePipeline({rhi::PipelineKind::PbrInstancedMesh,shaders/"PbrInstanced.hlsl",7},instanced),"raw instance pipeline/validated reflection");
        auto cpu=std::make_shared<StaticModel>();auto cube=MakeUnitCube();cube.material=0;cpu->meshes.push_back(std::move(cube));cpu->materials.emplace_back();ModelNode node;node.meshes={0};cpu->nodes.push_back(node);std::vector<AssetDiagnostic> diagnostics;
        Check(models.Upload(cpu,{},GetExecutableDirectory()/"Resources",*device,model,diagnostics),"instance mesh/three textures");
        const std::vector<std::uint8_t> pixels{180,75,225,255,75,180,225,255,128,128,255,255,195,128,225,255};
        Ok(device->CreateTexture({2,2,1,1,rhi::TextureFormat::Rgba8,"Instance.Normal"},pixels,normal),"test normal map");Ok(device->EnableGpuTiming(true),"instance queries");
    }
    ~Fixture(){if(device)device->Shutdown();window.postQuitOnDestroy=false;DestroyEngineWindow(window);}
    Mat4 ViewProjection() const{return *OrthographicRH(8,float(height)*8/width,0.1f,100)*Translation({0,0,-6});}
    rhi::DrawIndexed Template(bool instances) const
    {
        std::vector<rhi::DrawIndexed> draws;AppendPbrModelDraws(*model,Mat4{},{},draws,instances?*instanced:*classic,ViewProjection(),{0,0,6},{0.3f,0.5f,0.8f},{3,2,1});
        auto result=draws[0];result.normalTexture=*normal;return result;
    }
    void Resize(UINT w,UINT h){Check(SetEngineWindowClientSize(window,w,h),"instance resize client");width=w;height=h;Ok(device->Resize(w,h),"instance resize buffers");}
    rhi::Image Render(std::span<const rhi::InstanceParameters> values,bool instances,bool capture,rhi::CompletionToken* completion=nullptr,bool discardInput=false)
    {
        auto commands=device->BeginCommands(++frame);Check(bool(commands),"instance commands");Ok(commands->BeginPass({rhi::PassKind::Clear}),"instance clear");Ok(commands->EndPass(),"end clear");
        Ok(commands->BeginPass({rhi::PassKind::Opaque,rhi::ResourceUse::ColorWrite,rhi::ResourceUse::DepthWrite,"Opaque",rhi::ResourceUse::None,true}),"instance opaque");auto draw=Template(instances);
        if(instances&&!values.empty())
        {
            auto owned=std::vector<rhi::InstanceParameters>(values.begin(),values.end());Ok(commands->DrawInstanced(draw,owned),"instance batch recording");
            if(discardInput){for(auto& value:owned)value.world(0,0)=std::numeric_limits<float>::quiet_NaN();owned.clear();owned.shrink_to_fit();}
        }
        if(!instances)for(const auto& value:values)
        {auto item=draw;item.world=value.world;item.tint=value.tint;item.unlit=value.unlit;item.pbr.metallic=value.metallic;item.pbr.roughness=value.roughness;item.pbr.normalScale=value.normalScale;
            item.pbr.normalMapping=value.normalMapping;item.pbr.doubleSided=value.doubleSided;item.pbr.receivesShadow=value.receivesShadow;Ok(commands->Draw(item),"independent classic per-object reference");}
        Ok(commands->EndPass(),"end instance opaque");Ok(commands->Close(),"close instance commands");rhi::CompletionToken token;rhi::Image image;Ok(device->Submit(*commands,token,capture?&image:nullptr),"submit instance frame");if(completion)*completion=token;
        Check(device->Stats().debugWarnings==0&&device->Stats().debugErrors==0,"instance GPU diagnostics zero");return image;
    }
};
std::vector<rhi::InstanceParameters> Parameters(size_t count)
{
    std::vector<rhi::InstanceParameters> result;result.reserve(count);
    for(size_t index=0;index<count;++index)
    {
        rhi::InstanceParameters p;const float x=-3+float(index%8)*0.85f,y=-1.8f+float((index/8)%6)*0.7f,z=-float(index/48)*0.4f;
        p.world=Translation({x,y,z})*Rotation(AxisAngle({0,1,0},float(index)*0.37f))*Scaling({index%2?-0.5f:0.5f,0.35f+float(index%3)*0.05f,0.25f+float(index%4)*0.03f});
        if(index%9==0)p.world(0,1)+=0.15f;p.tint={0.25f+float(index%4)*0.2f,0.35f+float(index%3)*0.15f,0.9f-float(index%5)*0.1f,1};p.metallic=float(index%3)*0.5f;p.roughness=0.2f+float(index%5)*0.15f;
        p.normalScale=index%4==0?0:index%2?-0.6f:1;p.normalMapping=index%3!=0;p.unlit=index%7==0;p.doubleSided=index%4==0;result.push_back(p);
    }
    return result;
}
void Invalid(Fixture& fixture)
{
    auto commands=fixture.device->BeginCommands(++fixture.frame);Ok(commands->BeginPass({rhi::PassKind::Clear}),"invalid test clear");Ok(commands->EndPass(),"invalid clear end");
    Ok(commands->BeginPass({rhi::PassKind::Opaque}),"invalid opaque");auto draw=fixture.Template(true);const auto valid=Parameters(1);
    Check(commands->Draw(draw).error==rhi::Error::Unsupported,"instance pipeline rejects ordinary Draw");Check(commands->DrawInstanced(fixture.Template(false),valid).error==rhi::Error::Unsupported,"classic pipeline rejects instance data");
    auto bad=valid;bad[0].world(0,0)=std::numeric_limits<float>::quiet_NaN();Check(commands->DrawInstanced(draw,bad).error==rhi::Error::InvalidArgument,"nonfinite instance rejected atomically");
    bad=valid;bad[0].roughness=2;Check(commands->DrawInstanced(draw,bad).error==rhi::Error::InvalidArgument,"invalid per-instance material rejected");
    std::vector<rhi::InstanceParameters> tooMany(rhi::MaximumFrameInstances+1);Check(commands->DrawInstanced(draw,tooMany).error==rhi::Error::CapacityExceeded,"instance hard budget rejected before upload");
    Ok(commands->DrawInstanced(draw,valid),"same recording recovers after all rejected inputs");Ok(commands->EndPass(),"valid after rejection end");Ok(commands->Close(),"valid after rejection close");rhi::CompletionToken token;Ok(fixture.device->Submit(*commands,token),"valid after rejection submit");
    Check(fixture.device->Stats().frameInstances==1&&fixture.device->Stats().opaqueBindings.instanceBuffers==1,"failed records did not add half batches");
}
void Gated(Fixture& fixture)
{
    Ok(fixture.device->WaitIdle(),"before gate");auto& p=*rhi::D3D12DiagnosticsAccess::Presentation(*fixture.device);auto* native=rhi::D3D12DiagnosticsAccess::NativeDevice(*fixture.device);
    Microsoft::WRL::ComPtr<ID3D12Fence> gate;Check(SUCCEEDED(native->CreateFence(0,D3D12_FENCE_FLAG_NONE,IID_PPV_ARGS(gate.GetAddressOf()))),"create instance gate");
    struct Release{ID3D12Fence* gate;~Release(){gate->Signal(1);}} release{gate.Get()};Check(SUCCEEDED(p.queue->Wait(gate.Get(),1)),"queue actual unfinished work");
    const auto firstFrame=p.frameCursor;auto values=Parameters(257);rhi::CompletionToken first,second;fixture.Render(values,true,false,&first,true);
    Check(!fixture.device->IsComplete(first)&&p.frames[firstFrame].instanceSlices.size()==1,"first instance frame really in flight");const auto slice=p.frames[firstFrame].instanceSlices[0];
    const auto bytes=std::vector<std::byte>(slice.cpu,slice.cpu+slice.size);for(auto& value:values){value.world(0,3)+=0.3f;value.tint.x=0.1f;}
    fixture.Render(values,true,false,&second);Check(!fixture.device->IsComplete(second),"two instance contexts actually pending");
    Check(std::memcmp(slice.cpu,bytes.data(),bytes.size())==0,"second frame cannot overwrite first frame instance buffer before Fence");
    Check(SUCCEEDED(gate->Signal(1)),"release instance gate");Ok(fixture.device->WaitIdle(),"retire instance work");Check(fixture.device->IsComplete(first)&&fixture.device->IsComplete(second),"instance frame retirement");
}
void Direct(const Localization& text,const std::filesystem::path& output)
{
    Fixture fixture(text);std::vector<AssetDiagnostic> diagnostics;
    for(const size_t count:{size_t(0),size_t(1),size_t(17),size_t(257),size_t(1024),size_t(31)})
    {
        const auto values=Parameters(count);const auto reference=fixture.Render(values,false,true),actual=fixture.Render(values,true,true,nullptr,true);
        Check(reference.rgba8==actual.rgba8,"all GPU pixels equal for independent classic root constants vs raw per-instance matrices/materials/normal/sign flags");
        const auto stats=fixture.device->Stats();Check(stats.frameInstances==count&&stats.frameInstanceBytes==count*rhi::InstanceGpuStride&&stats.opaqueBindings.instanceBuffers==(count?1u:0u),"real instance byte/count/root SRV statistics");
        Check(WritePpmImage(output/("instances-"+std::to_string(count)+".ppm"),actual.width,actual.height,actual.rgba8,diagnostics),"save actual instance comparison");
    }
    Invalid(fixture);Gated(fixture);for(const auto size:{std::pair{257u,193u},std::pair{160u,120u},std::pair{320u,240u}})
    {fixture.Resize(size.first,size.second);const auto values=Parameters(17);Check(fixture.Render(values,false,true).rgba8==fixture.Render(values,true,true).rgba8,"resized instance/reference image equality");}
    const auto values=Parameters(17);fixture.Render(values,true,false);fixture.Render(values,true,false);const auto before=fixture.device->Stats();
    for(int index=0;index<32;++index)fixture.Render(values,true,false);const auto after=fixture.device->Stats();
    Check(before.globalWaits==after.globalWaits&&before.uploadPagesCreated==after.uploadPagesCreated&&before.memoryResourceCreations==after.memoryResourceCreations&&before.pipelineCreations==after.pipelineCreations,"ordinary instance frames reuse capacity without buffer creation/global waits");
    Ok(fixture.device->WaitIdle(),"instance final readback");const auto timings=fixture.device->GpuTimingsSince(0);Check(!timings.empty()&&timings.back().draws==1,"GPU Draw query reports actual batch count");
}
class Observed final:public Renderer
{
public:
    explicit Observed(SandboxScene scene=SandboxScene::Benchmark):renderer(scene){}
    SandboxRenderer renderer;rhi::Statistics stats;SandboxDrawStatistics draws;
    std::filesystem::path sequence;
    bool waitIdle=false; // 只给需要“帧后立即读回计数”的用例；普通 A/B 不强制同步。
    bool Initialize(const RenderSurface& surface,const Localization& text) override{return renderer.Initialize(surface,text);}
    RenderResult Render(const RenderFrame& frame,const Localization& text) override
    {if(!sequence.empty())renderer.SetCaptureOutput(sequence/(std::to_string(frame.updateIndex)+".ppm"),frame.updateIndex);
        const auto result=renderer.Render(frame,text);if(waitIdle)renderer.WaitIdle();stats=renderer.Statistics();draws=renderer.DrawStatistics();return result;}
    void Shutdown() noexcept override{renderer.Shutdown();}
};
void WorldCases(const std::filesystem::path& output)
{
    for(int test=0;test<3;++test)
    {
        std::vector<std::uint8_t> expected;
        for(bool enabled:{false,true})
        {
            BenchmarkSession session;session.captureMode=true;auto& c=session.config;c.kind=test==0?BenchmarkKind::Repeated:BenchmarkKind::Mixed;c.objects=test==2?1000:192;c.width=320;c.height=240;
            c.visibility=test==2?BenchmarkVisibility::MostlyOutside:BenchmarkVisibility::AllVisible;c.radiusMultiplier=5;c.frustumCulling=test==2;c.instancing=enabled;c.cacheOpaqueBindings=true;
            BenchmarkScene scene(session);auto services=scene.Services();services.maximumFrames=4;services.fixedDeltaSeconds=1.0/60;Observed observed;
            const auto path=output/("world-"+std::to_string(test)+(enabled?"-on":"-off")+".ppm");observed.renderer.SetCaptureOutput(path,3);
            Check(RunSandboxApp("en-US",observed,false,&services)==0&&observed.renderer.CaptureSucceeded(),"production World instancing path");std::vector<std::uint8_t> bytes;Check(asset_io::Read(path,bytes),"World image read");
            if(enabled)Check(bytes==expected,"production culling/instancing lists preserve all GPU pixels");else expected=bytes;
            Check(observed.draws.visible==(test==2?100u:192u)&&observed.draws.instances==observed.draws.visible,"entity/instance association after culling");
            if(enabled){Check(observed.draws.draws==observed.draws.instanceBatches&&observed.draws.instanceCapacity>=observed.draws.instances,"actual batch count and reused CPU capacity");
                if(test==0)Check(observed.draws.draws==1,"repeated objects actually use one DrawIndexedInstanced");if(test==1)Check(observed.draws.draws==48,"mixed objects stay in 48 compatible groups");}
            Check(observed.stats.debugWarnings==0&&observed.stats.debugErrors==0,"World instance GPU diagnostics zero");
        }
    }
}
void DynamicWorld(const std::filesystem::path& output)
{
    for(bool enabled:{false,true})
    {
        const auto directory=output/(enabled?"dynamic-on":"dynamic-off");std::filesystem::create_directories(directory);
        BenchmarkSession session;session.captureMode=true;auto& config=session.config;config.kind=BenchmarkKind::Mixed;config.objects=192;config.width=320;config.height=240;
        config.visibility=BenchmarkVisibility::AllVisible;config.radiusMultiplier=5;config.instancing=enabled;config.frustumCulling=true;config.cacheOpaqueBindings=true;
        BenchmarkScene scene(session);auto services=scene.Services();services.maximumFrames=8;services.fixedDeltaSeconds=1.0/60;const auto original=services.extract;std::uint64_t step=0;
        services.extract=[&](FrameData& data){original(data);++step;auto camera=session.camera;camera.yaw+=float(step)*0.03f;data.cameras[0].world=FreeCameraTransform(camera).Matrix();
            if(step%4==0)data.meshes.clear();else if(step%4==1)data.meshes.resize(17);
            else for(size_t index=0;index<data.meshes.size();++index){if(step%4==2&&index%3!=0)data.meshes[index].world=Translation({400,400,0})*data.meshes[index].world;
                else data.meshes[index].world=data.meshes[index].world*Rotation(AxisAngle({0,1,0},float(step)*0.1f));}};
        Observed observed;observed.sequence=directory;Check(RunSandboxApp("en-US",observed,false,&services)==0&&observed.renderer.CaptureSucceeded(),"dynamic culled instance list including zero and reappearance");
        Check(observed.stats.debugWarnings==0&&observed.stats.debugErrors==0,"dynamic World instance diagnostics");
    }
    for(int step=1;step<=8;++step){std::vector<std::uint8_t> off,on;const auto name=std::to_string(step)+".ppm";
        Check(asset_io::Read(output/"dynamic-off"/name,off)&&asset_io::Read(output/"dynamic-on"/name,on)&&off==on,"every dynamic/camera/empty/reappearing World frame preserves exact image and instance association");}
}
}
void Occlusion(const Localization& text,const std::filesystem::path& output)
{
    // G06.08：RHI 遮挡批次。每帧 prepass → HZB → ExecuteIndirect；关闭遮挡（普通实例管线 + 写深度）
    // 与开启遮挡的图像必须逐字节一致。互不重叠的实例避免同深度表面先后顺序差异。
    Fixture fixture(text);
    rhi::Pipeline color;Ok(fixture.device->CreatePipeline({rhi::PipelineKind::PbrInstancedMesh,
        GetExecutableDirectory()/"Resources/Shaders/PbrInstanced.hlsl",rhi::InstancingLayoutVersion,
        rhi::VertexLayoutVersionLegacy,rhi::DepthMode::ReadOnlyLessEqual},color),"occlusion color pipeline");
    const auto values=[&]
    {
        // 遮挡对照使用互不重叠的网格：prepass 写 LESS、主 Pass 用 LESS_EQUAL 时，
        // 两个不同表面恰好同深度会产生谁先谁后的差异；保持无同深度重叠才能做逐字节对照。
        std::vector<rhi::InstanceParameters> result;result.reserve(257);
        for(std::size_t index=0;index<257;++index)
        {
            rhi::InstanceParameters p;
            p.world=Translation({-3.2f+float(index%8)*0.9f,-1.8f+float((index/8)%6)*0.72f,-float(index/48)*0.6f})
                *Scaling({0.35f,0.3f,0.25f});
            p.tint={0.25f+float(index%4)*0.2f,0.35f+float(index%3)*0.15f,0.9f-float(index%5)*0.1f,1};
            p.metallic=float(index%3)*0.5f;p.roughness=0.2f+float(index%5)*0.15f;
            p.normalScale=index%2?-0.6f:1;p.normalMapping=index%3!=0;p.unlit=index%7==0;
            result.push_back(p);
        }
        return result;
    }();
    std::vector<VisibilityItem> bounds;bounds.reserve(values.size());
    for(std::size_t index=0;index<values.size();++index)
    {
        const auto box=TransformBounds(Aabb{{-.5f,-.5f,-.5f},{.5f,.5f,.5f}},values[index].world);
        Check(bool(box),"occlusion instance world bound");
        bounds.push_back({box->minimum,static_cast<std::uint32_t>(index),box->maximum,0});
    }
    const auto quantize=[&](VisibilityFrustum& frustum)
    {
        const auto view=BuildFrustum(fixture.ViewProjection());
        return view&&QuantizeVisibilityFrustum(*view,frustum);
    };
    VisibilityFrustum frustum;Check(quantize(frustum),"occlusion frustum");
    const auto render=[&](bool occluded,bool hiz=true,bool counters=false)->rhi::Image
    {
        auto commands=fixture.device->BeginCommands(++fixture.frame);Check(bool(commands),"occlusion commands");
        Ok(commands->BeginPass({rhi::PassKind::Clear}),"occlusion clear");Ok(commands->EndPass(),"occlusion clear end");
        auto draw=fixture.Template(true);
        if(occluded){draw.pipeline=*color;Ok(commands->ConfigureOcclusion(bounds,frustum,{hiz,1e-6f,counters}),"configure occlusion batch");}
        Ok(commands->BeginPass({rhi::PassKind::Opaque,rhi::ResourceUse::ColorWrite,rhi::ResourceUse::DepthWrite,"Opaque",rhi::ResourceUse::None,true}),"occlusion opaque");
        Ok(commands->DrawInstanced(draw,values),"occlusion instanced draw");Ok(commands->EndPass(),"occlusion end");
        Ok(commands->Close(),"occlusion close");rhi::CompletionToken token;rhi::Image image;
        Ok(fixture.device->Submit(*commands,token,&image),"occlusion submit");
        Check(fixture.device->Stats().debugWarnings==0&&fixture.device->Stats().debugErrors==0,"occlusion GPU diagnostics zero");return image;
    };
    const auto reference=render(false);const auto first=render(true);
    Check(first.rgba8==reference.rgba8,"first occluded frame (prepass/HZB/indirect) preserves the image");
    for(int frame=0;frame<4;++frame)Check(render(true).rgba8==reference.rgba8,"HZB occluded frame preserves the image byte for byte");
    const auto stats=fixture.device->Stats();
    Check(stats.occlusionFrames==5&&stats.occlusionInstances==std::uint64_t{5}*values.size(),"occlusion batch statistics count every indirect frame");
    std::vector<AssetDiagnostic> diagnostics;
    Check(WritePpmImage(output/"occlusion.ppm",first.width,first.height,first.rgba8,diagnostics),"save occlusion image");
    // G06.09：显式请求后，间接参数 48B 回读给出 GPU 端真实可见数；全视锥场景应等于候选数。
    for(int frame=0;frame<3;++frame)render(true,true,true);
    Ok(fixture.device->WaitIdle(),"occlusion counters completion");
    const auto counted=fixture.device->Stats().occlusion;
    Check(counted.valid&&counted.candidates==values.size()&&counted.visible==values.size()
        &&counted.culled==0&&!counted.overflow&&counted.frame>0,"occlusion readback reports every visible instance");
    // 同一批实例用更窄视锥重放一次，只验证计数比例，不与参考图比较（可见集本应更小）。
    VisibilityFrustum narrowFrustum{};
    const auto narrowView=BuildFrustum(*OrthographicRH(2,1.5f,.1f,100)*Translation({0,0,-6}));
    Check(narrowView&&QuantizeVisibilityFrustum(*narrowView,narrowFrustum),"narrow occlusion frustum");
    {
        auto commands=fixture.device->BeginCommands(++fixture.frame);Check(bool(commands),"narrow occlusion commands");
        Ok(commands->BeginPass({rhi::PassKind::Clear}),"narrow occlusion clear");Ok(commands->EndPass(),"narrow occlusion clear end");
        auto draw=fixture.Template(true);draw.pipeline=*color;
        Ok(commands->ConfigureOcclusion(bounds,narrowFrustum,{true,1e-6f,true}),"narrow occlusion configuration");
        Ok(commands->BeginPass({rhi::PassKind::Opaque,rhi::ResourceUse::ColorWrite,rhi::ResourceUse::DepthWrite,"Opaque",rhi::ResourceUse::None,true}),"narrow occlusion opaque");
        Ok(commands->DrawInstanced(draw,values),"narrow occlusion draw");Ok(commands->EndPass(),"narrow occlusion end");
        Ok(commands->Close(),"narrow occlusion close");rhi::CompletionToken token;
        Ok(fixture.device->Submit(*commands,token),"narrow occlusion submit");
    }
    Ok(fixture.device->WaitIdle(),"narrow occlusion counters completion");
    const auto narrowCounters=fixture.device->Stats().occlusion;
    Check(narrowCounters.valid&&narrowCounters.candidates==values.size()&&narrowCounters.visible>0
        &&narrowCounters.visible<values.size()&&narrowCounters.culled==values.size()-narrowCounters.visible,
        "occlusion readback distinguishes culled instances from candidates");
    std::cout<<"Occlusion readback: all="<<counted.candidates<<"/"<<counted.visible<<"/"<<counted.culled
        <<", narrow="<<narrowCounters.candidates<<"/"<<narrowCounters.visible<<"/"<<narrowCounters.culled<<'\n';
    const auto warm=fixture.device->Stats();for(int frame=0;frame<8;++frame)render(true);
    const auto steady=fixture.device->Stats();
    Check(steady.memoryResourceCreations==warm.memoryResourceCreations&&steady.buffers==warm.buffers,
        "persistent occlusion kernels/HZB/output buffers are reused per frame");
    fixture.Resize(257,193);Check(quantize(frustum),"occlusion frustum after resize");
    const auto resizedReference=render(false);
    const auto resizedOn=render(true);
    const auto describe=[](const rhi::Image& value,const rhi::Image& reference)
    {
        std::size_t changed=0;int maximum=0;std::string first;
        for(std::size_t index=0;index<value.rgba8.size()&&index<reference.rgba8.size();++index)
        {
            const int delta=std::abs(int(value.rgba8[index])-int(reference.rgba8[index]));
            if(delta)
            {
                ++changed;maximum=std::max(maximum,delta);
                if(first.size()<80){const auto pixel=index/4;first+=" ("+std::to_string(pixel%value.width)+","+std::to_string(pixel/value.width)+")"
                    +std::to_string(int(value.rgba8[pixel*4]))+"/"+std::to_string(int(reference.rgba8[pixel*4]));}
            }
        }
        return std::to_string(value.width)+"x"+std::to_string(value.height)+"/"+std::to_string(changed)+"/"+std::to_string(maximum)+first;
    };
    std::cerr<<"resize diagnostics: ref="<<describe(resizedReference,resizedReference)
        <<", hzb="<<describe(resizedOn,resizedReference)<<'\n';
    Check(resizedOn.rgba8==resizedReference.rgba8,"first frame after resize rebuilds HZB and matches");
    for(int frame=0;frame<2;++frame)render(true,true,true);
    Ok(fixture.device->WaitIdle(),"resize occlusion counters completion");
    const auto resizedCounters=fixture.device->Stats().occlusion;
    Check(resizedCounters.valid&&resizedCounters.frame>narrowCounters.frame
        &&resizedCounters.candidates==values.size()&&resizedCounters.visible==values.size()
        &&resizedCounters.culled==0,"occlusion readback survives resize and reports the new frame");
    {
        auto commands=fixture.device->BeginCommands(++fixture.frame);Ok(commands->BeginPass({rhi::PassKind::Clear}),"occlusion invalid clear");
        Ok(commands->EndPass(),"occlusion invalid clear end");
        Check(commands->ConfigureOcclusion(std::span<const VisibilityItem>{},frustum,{}).error==rhi::Error::InvalidArgument,"empty occlusion bounds rejected");
        auto bad=bounds;bad[0].id=7;
        Check(commands->ConfigureOcclusion(bad,frustum,{}).error==rhi::Error::InvalidArgument,"occlusion ids must match instance order");
        Check(commands->ConfigureOcclusion(bounds,frustum,{true,-1}).error==rhi::Error::InvalidArgument,"negative occlusion depth bias rejected");
        // G06.10：多批次声明逐批校验；任一批次为空或局部 id 错位都拒绝整帧。
        const rhi::OcclusionBatch emptyBatch[]{ {bounds}, {std::span<const VisibilityItem>{}} };
        Check(commands->ConfigureOcclusion(emptyBatch,frustum,{}).error==rhi::Error::InvalidArgument,"empty batch rejected");
        auto misaligned=bounds;misaligned[0].id=7;
        const rhi::OcclusionBatch badBatch[]{ {misaligned} };
        Check(commands->ConfigureOcclusion(badBatch,frustum,{}).error==rhi::Error::InvalidArgument,"per-batch instance ids must be local and ordered");
        auto draw=fixture.Template(true);Ok(commands->BeginPass({rhi::PassKind::Opaque}),"occlusion fallback opaque");
        Check(commands->ConfigureOcclusion(bounds,frustum,{}).error==rhi::Error::InvalidState,"occlusion declaration must precede the opaque pass");
        Ok(commands->DrawInstanced(draw,values),"occlusion fallback draw");Ok(commands->EndPass(),"occlusion fallback end");
        Ok(commands->Close(),"occlusion fallback close");rhi::CompletionToken token;Ok(fixture.device->Submit(*commands,token),"occlusion fallback submit");
    }
    std::cout<<"Occlusion: indirect frames="<<stats.occlusionFrames<<", instances="<<stats.occlusionInstances
        <<", byte-identical="<<(first.rgba8==reference.rgba8)<<'\n';
}

void SandboxOcclusion(const std::filesystem::path& output)
{
    // G06.08：SandboxRenderer 常驻路径。同一固定相机下 SELFENGINE_OCCLUSION 关闭/开启的截图必须逐字节一致，
    // 且开启后实际走 RHI 遮挡批次（single batch → prepass/HZB/ExecuteIndirect）。
    std::vector<std::uint8_t> expected;
    for(bool enabled:{false,true})
    {
        Check(SetEnvironmentVariableW(L"SELFENGINE_OCCLUSION",enabled?L"1":nullptr)!=FALSE,"toggle SELFENGINE_OCCLUSION");
        BenchmarkSession session;session.captureMode=true;auto& config=session.config;
        config.kind=BenchmarkKind::Repeated;config.objects=192;config.width=320;config.height=240;
        config.visibility=BenchmarkVisibility::AllVisible;config.radiusMultiplier=5;
        config.instancing=true;config.cacheOpaqueBindings=true;
        BenchmarkScene scene(session);auto services=scene.Services();services.maximumFrames=4;services.fixedDeltaSeconds=1.0/60;Observed observed;
        const auto path=output/(enabled?"sandbox-occlusion-on.ppm":"sandbox-occlusion-off.ppm");
        observed.renderer.SetCaptureOutput(path,3);
        Check(RunSandboxApp("en-US",observed,false,&services)==0&&observed.renderer.CaptureSucceeded(),"production occlusion path");
        std::vector<std::uint8_t> bytes;Check(asset_io::Read(path,bytes),"sandbox occlusion image read");
        if(enabled)
        {
            Check(bytes==expected,"sandbox occlusion image equals the normal instanced frame");
            Check(observed.draws.occludedInstances==192&&observed.stats.occlusionFrames>=1,"sandbox actually submitted the RHI occlusion batch");
        }
        else expected=bytes;
        Check(observed.stats.debugWarnings==0&&observed.stats.debugErrors==0,"sandbox occlusion GPU diagnostics zero");
    }
    // G06.09：运行时 HUD。默认关闭时 A/B 逐字节一致；开启后只多出覆盖层，
    // 并由 RHI 的 48B 参数回读提供候选/可见/剔除，不再用 CPU 估算。
    Check(SetEnvironmentVariableW(L"SELFENGINE_OCCLUSION",L"1")!=FALSE,"enable occlusion for HUD");
    Check(SetEnvironmentVariableW(L"SELFENGINE_OCCLUSION_HUD",L"1")!=FALSE,"enable occlusion HUD");
    {
        BenchmarkSession session;session.captureMode=true;auto& config=session.config;
        config.kind=BenchmarkKind::Repeated;config.objects=192;config.width=320;config.height=240;
        config.visibility=BenchmarkVisibility::AllVisible;config.radiusMultiplier=5;
        config.instancing=true;config.cacheOpaqueBindings=true;
        BenchmarkScene scene(session);auto services=scene.Services();services.maximumFrames=8;services.fixedDeltaSeconds=1.0/60;Observed observed;
        const auto path=output/"sandbox-occlusion-hud.ppm";
        observed.waitIdle=true;observed.renderer.SetCaptureOutput(path,7);
        Check(RunSandboxApp("en-US",observed,false,&services)==0&&observed.renderer.CaptureSucceeded(),"production occlusion HUD path");
        const auto counters=observed.stats.occlusion;
        Check(counters.valid&&counters.candidates==192&&counters.visible==192&&counters.culled==0,
            "occlusion HUD consumes the GPU readback counts");
        std::vector<std::uint8_t> hud;Check(asset_io::Read(path,hud),"sandbox occlusion HUD image read");
        std::size_t changed=0;
        for(std::size_t index=0;index<hud.size()&&index<expected.size();++index)changed+=hud[index]!=expected[index];
        Check(changed>500,"occlusion HUD adds a visible overlay without changing the scene counts");
        Check(observed.stats.debugWarnings==0&&observed.stats.debugErrors==0,"sandbox occlusion HUD GPU diagnostics zero");
        std::cout<<"Occlusion HUD: candidates="<<counters.candidates<<", visible="<<counters.visible
            <<", culled="<<counters.culled<<", changed-bytes="<<changed<<'\n';
    }
    Check(SetEnvironmentVariableW(L"SELFENGINE_OCCLUSION_HUD",nullptr)!=FALSE,"clear SELFENGINE_OCCLUSION_HUD");
    Check(SetEnvironmentVariableW(L"SELFENGINE_OCCLUSION",nullptr)!=FALSE,"clear SELFENGINE_OCCLUSION");
}

void ForwardMultiBatch(const std::filesystem::path& output)
{
    // G06.10：Forward 实例化 + HDR 输出 + 多批次 ExecuteIndirect。
    // 固定世界只有球/立方两种网格，位置互不重叠，避免 prepass LESS 与主 Pass LESS_EQUAL
    // 在等深表面上的已知 tie-break 差异；遮挡关闭/开启的 HDR 显示结果必须逐字节一致。
    struct Host
    {
        World world;
        Host()
        {
            const auto camera=world.Create("Camera",1),light=world.Create("DirectionalLight",2);
            Check(bool(camera)&&bool(light),"forward host camera/light");
            Check(world.SetCamera(camera,{Pi/3,0.1f,100,true}),"forward host camera lens");
            Check(world.SetTransform(camera,FreeCameraTransform({{0,0,6},0,0})),"forward host camera transform");
            Check(world.SetLight(light,{{1,0.96f,0.9f},3}),"forward host light");
            Check(world.SetTransform(light,{{},AxisAngle({1,0,0},-0.5f)*AxisAngle({0,1,0},0.4f),{1,1,1}}),"forward host light transform");
            for(int index=0;index<4;++index)
            {
                const auto sphere=world.Create("Sphere",10+index),cube=world.Create("Cube",20+index);
                Check(bool(sphere)&&bool(cube),"forward host mesh entity");
                Check(world.SetMesh(sphere,{BuiltinSphereAssetId,{}}),"forward host sphere mesh");
                Check(world.SetTransform(sphere,{{-2.1f+float(index)*1.4f,0.7f,0}}),"forward host sphere transform");
                Check(world.SetMesh(cube,{BuiltinCubeAssetId,{}}),"forward host cube mesh");
                Check(world.SetTransform(cube,{{-2.1f+float(index)*1.4f,-1.1f,0.35f}}),"forward host cube transform");
            }
        }
        ApplicationServices Services()
        {
            ApplicationServices services;
            services.initialWidth=320;services.initialHeight=240;
            services.maximumFrames=4;services.fixedDeltaSeconds=1.0/60;
            services.extract=[this](FrameData& data)
            {
                BuildFrameData(world,nullptr,nullptr,data);
                data.instancing=true;data.frustumCulling=false;data.sortOpaque=false;data.cacheOpaqueBindings=true;
            };
            return services;
        }
    };
    Check(SetEnvironmentVariableW(L"SELFENGINE_OCCLUSION_HUD",nullptr)!=FALSE,"forward test clears HUD");
    std::vector<std::uint8_t> expected;
    for(bool enabled:{false,true})
    {
        Check(SetEnvironmentVariableW(L"SELFENGINE_OCCLUSION",enabled?L"1":nullptr)!=FALSE,"toggle forward occlusion");
        Host host;auto services=host.Services();
        Observed observed(SandboxScene::Lighting);observed.waitIdle=true;
        const auto path=output/(enabled?"forward-occlusion-on.ppm":"forward-occlusion-off.ppm");
        observed.renderer.SetCaptureOutput(path,3);
        Check(RunSandboxApp("en-US",observed,false,&services)==0&&observed.renderer.CaptureSucceeded(),"forward instancing path");
        std::vector<std::uint8_t> bytes;Check(asset_io::Read(path,bytes),"forward image read");
        if(enabled)
        {
            Check(bytes==expected,"forward multi-batch occlusion preserves the HDR image");
            Check(observed.draws.instanceBatches>=2,"forward fixed world actually uses multiple instance batches");
            Check(observed.draws.instances==8&&observed.draws.occludedInstances==8,
                "forward occlusion covers every visible instance across batches");
            Check(observed.stats.occlusionFrames>=1,"forward frame actually submitted multi-batch indirect draws");
            std::cout<<"Forward occlusion: batches="<<observed.draws.instanceBatches<<", instances="<<observed.draws.instances<<'\n';
        }
        else
        {
            expected=bytes;
            Check(observed.stats.occlusionFrames==0,"forward occlusion off does not submit indirect batches");
            Check(!observed.stats.occlusionTiming.valid,"forward occlusion off does not record timing queries");
        }
        Check(observed.stats.debugWarnings==0&&observed.stats.debugErrors==0,"forward instance GPU diagnostics zero");
    }
    // 计数回读只在显式 HUD/诊断请求时开启；第三次运行验证多批次聚合计数，不参与 A/B 图像比较。
    Check(SetEnvironmentVariableW(L"SELFENGINE_OCCLUSION",L"1")!=FALSE,"forward counters enable occlusion");
    Check(SetEnvironmentVariableW(L"SELFENGINE_OCCLUSION_HUD",L"1")!=FALSE,"forward counters enable HUD readback");
    Check(SetEnvironmentVariableW(L"SELFENGINE_OCCLUSION_TIMING",L"1")!=FALSE,"forward counters enable timing");
    {
        Host host;auto services=host.Services();services.maximumFrames=8;
        Observed observed(SandboxScene::Lighting);observed.waitIdle=true;
        observed.renderer.SetCaptureOutput(output/"forward-occlusion-hud.ppm",7);
        Check(RunSandboxApp("en-US",observed,false,&services)==0&&observed.renderer.CaptureSucceeded(),"forward HUD path");
        const auto counters=observed.stats.occlusion;
        std::cout<<"Forward counters: candidates="<<counters.candidates<<", visible="<<counters.visible
            <<", draws="<<observed.draws.instances<<", batches="<<observed.draws.instanceBatches<<'\n';
        Check(counters.valid&&counters.candidates==observed.draws.instances&&counters.visible<=counters.candidates,
            "forward aggregate readback sums every batch");
        // G06.11：多批次 cull 分段聚合；prepass/HZB/main 由呈现层边界查询提供。
        const auto timing=observed.stats.occlusionTiming;
        std::cout<<"Forward timing: prepass="<<timing.prepassMs<<"ms, hzb="<<timing.hzbMs
            <<"ms, cull="<<timing.cullMs<<"ms, main="<<timing.mainMs<<"ms, total="<<timing.totalMs<<"ms\n";
        Check(timing.valid&&timing.frame>0&&timing.frequency>0,"forward occlusion timing completes");
        Check(timing.prepassMs>=0&&timing.hzbMs>=0&&timing.cullMs>=0&&timing.mainMs>=0&&timing.totalMs>0,
            "forward occlusion timing segments are non-negative");
        Check(timing.totalMs+1e-6>=timing.prepassMs+timing.hzbMs+timing.cullMs+timing.mainMs,
            "forward timing segments fit inside the total");
        Check(observed.stats.debugWarnings==0&&observed.stats.debugErrors==0,"forward HUD GPU diagnostics zero");
    }
    Check(SetEnvironmentVariableW(L"SELFENGINE_OCCLUSION_TIMING",nullptr)!=FALSE,"forward clears timing");
    Check(SetEnvironmentVariableW(L"SELFENGINE_OCCLUSION_HUD",nullptr)!=FALSE,"forward clears HUD");
    Check(SetEnvironmentVariableW(L"SELFENGINE_OCCLUSION",nullptr)!=FALSE,"forward test clears occlusion");
}

int wmain(int argc,wchar_t** argv)
{
    try{Check(argc==2,"supply instance output");const auto output=std::filesystem::absolute(argv[1]);std::filesystem::create_directories(output);Localization text;std::string error;Check(text.Load(GetExecutableDirectory()/"Resources/Localization/en-US.txt",error),"instance localization");
        Cpu();Direct(text,output);Occlusion(text,output);SandboxOcclusion(output);ForwardMultiBatch(output);WorldCases(output);DynamicWorld(output);std::cout<<"InstancingTests passed (independent GPU reference, per-instance material/normal, capacity/Fence, invalid recovery, resize, RHI occlusion batch, forward HDR multi-batch occlusion, SandboxRenderer occlusion path and dynamic culled World lists).\n";return 0;}
    catch(const std::exception& error){std::cerr<<"InstancingTests: "<<error.what()<<'\n';return 1;}
}
