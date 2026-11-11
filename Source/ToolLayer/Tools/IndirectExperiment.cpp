#include "IndirectExperiment.h"
#include "PlatformLayer/Graphics/D3D12/D3D12Context.h"
#include "PlatformLayer/Graphics/D3D12/D3D12Indirect.h"
#include "PlatformLayer/Graphics/D3D12/D3D12HiZ.h"
#include "PlatformLayer/Graphics/D3D12/D3D12DepthPrepass.h"
#include "FunctionLayer/Rendering/HiZ.h"
#include "PlatformLayer/Graphics/D3D12/D3D12Presentation.h"
#include "PlatformLayer/Graphics/D3D12/D3D12GpuTimer.h"
#include "PlatformLayer/Graphics/D3D12/D3D12StateTracker.h"
#include "PlatformLayer/Graphics/D3D12/D3D12Memory.h"
#include "PlatformLayer/Windows/Window.h"
#include "PlatformLayer/Windows/ExecutablePath.h"
#include "ResourceLayer/Assets/Primitives.h"
#include "CoreLayer/Localization.h"
#include <d3d12sdklayers.h>
#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstring>
#include <map>
#include <stdexcept>

using Microsoft::WRL::ComPtr;
namespace se
{
namespace
{
using Clock=std::chrono::steady_clock;
double Ms(Clock::time_point start){return std::chrono::duration<double,std::milli>(Clock::now()-start).count();}
void Check(bool value,const char* message){if(!value)throw std::runtime_error(message);}
void Hr(HRESULT result,const char* message){Check(SUCCEEDED(result),message);}
D3D12_RESOURCE_DESC Buffer(UINT64 bytes)
{
    D3D12_RESOURCE_DESC d{};d.Dimension=D3D12_RESOURCE_DIMENSION_BUFFER;d.Width=bytes;
    d.Height=d.DepthOrArraySize=d.MipLevels=1;d.SampleDesc={1,0};d.Layout=D3D12_TEXTURE_LAYOUT_ROW_MAJOR;return d;
}
}
IndirectScene BuildIndirectScene(std::uint32_t count,std::uint32_t rate,bool equalDepth)
{
    Check(count<=MaximumVisibilityItems&&rate<3,"indirect scene arguments");IndirectScene scene;
    scene.viewProjection=*OrthographicRH(8,6,.1f,100)*Translation({0,0,-6});
    const auto frustum=BuildFrustum(scene.viewProjection);Check(frustum&&QuantizeVisibilityFrustum(*frustum,scene.frustum),"indirect scene frustum");
    const UINT visible=rate==0?0:rate==1?(count+9)/10:count;
    const UINT columns=std::max(1u,static_cast<UINT>(std::ceil(std::sqrt(double(visible)*7/5))));
    const UINT rows=std::max(1u,(visible+columns-1)/columns);UINT placed=0;
    for(UINT i=0;i<count;++i)
    {
        const bool inside=rate==2||(rate==1&&i%10==0);rhi::InstanceParameters p;
        const float size=equalDepth?.8f:std::min(.4f,std::min(7.0f/columns,5.0f/rows)*.65f);
        const float x=inside?(equalDepth?0:-3.5f+7.0f*(float(placed%columns)+.5f)/columns):20+float(i%19);
        const float y=inside?(equalDepth?0:-2.5f+5.0f*(float(placed/columns)+.5f)/rows):0;
        p.world=Translation({x,y,0})*Rotation(AxisAngle({0,1,0},equalDepth?0:float(i%13)*.11f))*Scaling({size*(equalDepth||i%2?1.0f:-1.0f),size*.8f,size*.7f});
        p.tint={.25f+float(i%4)*.2f,.3f+float(i%3)*.2f,.85f-float(i%5)*.1f,1};
        p.metallic=float(i%3)*.4f;p.roughness=.2f+float(i%5)*.15f;p.normalScale=i%2?1.0f:-.6f;p.normalMapping=i%3!=0;p.unlit=equalDepth||i%7==0;
        if(equalDepth&&i==0)p.tint={.9f,.1f,.1f,1};if(equalDepth&&i+1==count)p.tint={.1f,.9f,.1f,1};
        const auto box=TransformBounds(Aabb{{-.5f,-.5f,-.5f},{.5f,.5f,.5f}},p.world);Check(bool(box),"indirect valid world bound");
        scene.instances.push_back(p);scene.bounds.push_back({box->minimum,i,box->maximum,0});if(inside)++placed;
    }
    return scene;
}
IndirectScene BuildOcclusionScene()
{
    IndirectScene scene;
    scene.viewProjection=*OrthographicRH(8,6,.1f,100)*Translation({0,0,-6});
    const auto frustum=BuildFrustum(scene.viewProjection);Check(frustum&&QuantizeVisibilityFrustum(*frustum,scene.frustum),"occlusion scene frustum");
    const auto add=[&](const Mat4& world,Vec4 tint)
    {
        rhi::InstanceParameters instance;instance.world=world;instance.tint=tint;instance.unlit=false;
        instance.metallic=.8f;instance.roughness=.25f;
        const auto box=TransformBounds(Aabb{{-.5f,-.5f,-.5f},{.5f,.5f,.5f}},world);
        Check(bool(box),"occlusion valid world bound");
        scene.instances.push_back(instance);
        scene.bounds.push_back({box->minimum,UINT(scene.bounds.size()),box->maximum,0});
    };
    // 先画后方实例、再画遮挡体：关闭遮挡时这些实例真实着色，开启后由 HZB 剔除。
    for(UINT y=0;y<20;++y)for(UINT x=0;x<20;++x)
        add(Translation({-2.85f+float(x)*0.3f,-1.9f+float(y)*0.2f,-2.5f})*Scaling({0.15f,0.1f,0.15f}),
            {0.9f,0.2f,0.2f,1});
    add(Scaling({6.0f,4.0f,0.2f}),{0.55f,0.55f,0.58f,1});
    // 遮挡体前方的实例与遮挡体外侧的实例必须保留。
    for(UINT i=0;i<3;++i)add(Translation({-2.0f+float(i)*2.0f,0,2.0f})*Scaling({0.4f,0.4f,0.4f}),{0.2f,0.9f,0.2f,1});
    for(UINT i=0;i<4;++i)add(Translation({(i%2?1.0f:-1.0f)*3.6f,(i<2?1.5f:-1.5f),-2.5f})*Scaling({0.4f,0.4f,0.4f}),
        {0.2f,0.3f,0.9f,1});
    return scene;
}
IndirectScene BuildDenseOcclusionScene()
{
    IndirectScene scene;
    scene.viewProjection=*OrthographicRH(8,6,.1f,100)*Translation({0,0,-6});
    const auto frustum=BuildFrustum(scene.viewProjection);Check(frustum&&QuantizeVisibilityFrustum(*frustum,scene.frustum),"dense occlusion frustum");
    const auto add=[&](const Mat4& world,Vec4 tint,bool unlit)
    {
        rhi::InstanceParameters instance;instance.world=world;instance.tint=tint;instance.unlit=unlit;
        instance.metallic=.8f;instance.roughness=.25f;
        const auto box=TransformBounds(Aabb{{-.5f,-.5f,-.5f},{.5f,.5f,.5f}},world);
        Check(bool(box),"dense occlusion valid world bound");
        scene.instances.push_back(instance);
        scene.bounds.push_back({box->minimum,UINT(scene.bounds.size()),box->maximum,0});
    };
    // 400 个后方实例分成 4 个深度层，每层 10×10 铺满视口；层从远到近绘制，
    // 关闭遮挡时每层都会真实着色（约 4 层全屏 overdraw），开启后整批由 HZB 剔除。
    // 层内同深度只着色一层，避免无意义的 400 倍 overdraw；遮挡体前表面在 -2.6。
    for(UINT layer=0;layer<4;++layer)for(UINT y=0;y<10;++y)for(UINT x=0;x<10;++x)
        add(Translation({-3.6f+float(x)*0.8f,-2.7f+float(y)*0.6f,-4.5f+float(layer)*0.5f})*Scaling({1.0f,0.8f,0.12f}),
            {0.9f,0.2f,0.2f,1},false);
    // 遮挡体覆盖 8.5×6.5 世界单位，完整包住 8×6 视口；晚于后方实例绘制。
    add(Scaling({8.5f,6.5f,0.2f}),{0.55f,0.55f,0.58f,1},false);
    // 400 个前方可见实例使遮挡计数比例约为 400/801=49.9%，同时增加两种路径共同承担的绘制量。
    for(UINT y=0;y<20;++y)for(UINT x=0;x<20;++x)
        add(Translation({-3.8f+float(x)*0.4f,-2.85f+float(y)*0.3f,2.0f})*Scaling({0.12f,0.10f,0.08f}),
            {0.2f,0.9f,0.2f,1},false);
    return scene;
}
struct IndirectExperiment::Impl
{
    struct Slot
    {
        D3D12IndirectOutput output;ComPtr<ID3D12QueryHeap> queries;ComPtr<ID3D12Resource> readback;
        UINT64 sourceFrame=0,fence=0;
    };
    Localization text;UINT capacity=0;DWORD owner=GetCurrentThreadId();WindowState window;D3D12Context context;
    D3D12Presentation presentation;D3D12MeshPipeline pipeline;D3D12MeshPipeline colorPipeline;D3D12Mesh mesh;D3D12Indirect kernel;
    D3D12DepthPrepass prepass;D3D12HiZ hiz;D3D12HiZOutput hizOutput;HiZHistory hizHistory;std::array<Slot,2> slots;
    Profiler profiler;std::vector<UINT> visible;std::vector<rhi::InstanceParameters> selected;
    std::deque<IndirectFrame> frames;std::map<UINT64,std::array<UINT64,8>> indirectTimings;
    UINT64 sequence=0,dropped=0,imageReadbacks=0,timingReads=0;bool stopped=false,gpuReady=false,toggle=false;
    // G06.06：最近一帧的间接参数读回，用于报告实际剔除后的实例数。
    UINT lastSlotIndex=0;bool lastSlotValid=false;
    ComPtr<ID3D12CommandAllocator> visibilityAllocator;ComPtr<ID3D12GraphicsCommandList> visibilityCommands;
    ComPtr<ID3D12Resource> visibilityReadback;
    double initialization=0,gpuInitialization=0;
    std::string adapter;
    Impl(const Localization& language,UINT maximum):text(language),capacity(maximum){}
    ~Impl()
    {
        // 原Presentation终止协议先证明在途完成，之后成员才自动释放。
        if(presentation.queue)ReleaseD3D12Presentation(presentation);
        window.postQuitOnDestroy=false;DestroyEngineWindow(window);
    }
    void Owner() const{Check(owner==GetCurrentThreadId(),"indirect experiment wrong thread");}
    void Initialize(bool show)
    {
        const auto started=Clock::now();Check(capacity<=MaximumVisibilityItems,"indirect capacity");
        Check(CreateEngineWindow(window,show)&&SetEngineWindowClientSize(window,320,240),"indirect window");
        window.messageHandler=[this](HWND,UINT message,WPARAM key,LPARAM flags)->std::optional<LRESULT>{
            if(message==WM_KEYDOWN&&key==VK_F1&&(flags&(1ll<<30))==0)toggle=true;
            if(message==WM_KEYDOWN&&key==VK_ESCAPE)window.closeRequested=true;return std::nullopt;};
        int code=0;Check(PumpWindowMessages(code),"indirect initial messages");
        Check(InitializeD3D12Context(context,text)&&InitializeD3D12Presentation(presentation,context,window.hwnd,text),"indirect presentation");
        DXGI_ADAPTER_DESC1 description{};Hr(context.adapter->GetDesc1(&description),"indirect adapter description");
        const int bytes=WideCharToMultiByte(CP_UTF8,WC_ERR_INVALID_CHARS,description.Description,-1,nullptr,0,nullptr,nullptr);
        Check(bytes>0,"indirect adapter UTF-8 length");adapter.resize(static_cast<size_t>(bytes));
        Check(WideCharToMultiByte(CP_UTF8,WC_ERR_INVALID_CHARS,description.Description,-1,adapter.data(),bytes,nullptr,nullptr)>0,"indirect adapter UTF-8");adapter.pop_back();
        Check(InitializeD3D12MeshPipeline(pipeline,context,{rhi::PipelineKind::PbrInstancedMesh,GetExecutableDirectory()/"Resources/Shaders/PbrInstanced.hlsl",rhi::InstancingLayoutVersion},text),"indirect existing PBR instance pipeline");
        Check(InitializeD3D12MeshPipeline(colorPipeline,context,{rhi::PipelineKind::PbrInstancedMesh,GetExecutableDirectory()/"Resources/Shaders/PbrInstanced.hlsl",
            rhi::InstancingLayoutVersion,rhi::VertexLayoutVersionLegacy,rhi::DepthMode::ReadOnlyLessEqual},text),"occlusion color pipeline");
        Hr(prepass.Initialize(*context.device.Get(),GetExecutableDirectory()/"Experiments/DepthPrepass.hlsl",*pipeline.rootSignature.Get(),text),"occlusion depth prepass");
        Hr(hiz.Initialize(context,GetExecutableDirectory()/"Experiments/HiZ.hlsl",text),"occlusion HZB kernel");
        const auto cube=MakeUnitCube();std::vector<D3D12MeshVertex> vertices;for(const auto& v:cube.vertices)vertices.push_back({v.position,v.normal,v.uv,v.tangent});
        const std::array<std::uint8_t,4> base{180,180,180,255},normal{160,90,225,255},material{255,210,180,255};
        Check(InitializeD3D12Mesh(mesh,context,presentation,vertices,cube.indices,{1,1,base},text),"indirect shared cube and base texture");
        Check(InitializeD3D12Texture(mesh.pbrTextures[0],context,presentation,{1,1,normal},text)
            &&InitializeD3D12Texture(mesh.pbrTextures[1],context,presentation,{1,1,material},text),"indirect shared material textures");
        Check(SetD3D12GpuTiming(presentation,true,text),"indirect normal frame timing");profiler.SetEnabled(true);presentation.cpuProfiler=&profiler;
        initialization=Ms(started);
    }
    void Readback(UINT64 bytes,ComPtr<ID3D12Resource>& output,const char* name)
    {
        D3D12_HEAP_PROPERTIES heap{};heap.Type=D3D12_HEAP_TYPE_READBACK;heap.CreationNodeMask=heap.VisibleNodeMask=1;const auto d=Buffer(bytes);
        Hr(context.device->CreateCommittedResource(&heap,D3D12_HEAP_FLAG_NONE,&d,D3D12_RESOURCE_STATE_COPY_DEST,nullptr,IID_PPV_ARGS(output.GetAddressOf())),"indirect readback allocation");
        Hr(context.states->Register(*output.Get(),D3D12_RESOURCE_STATE_COPY_DEST,D3D12TrackedHeap::Readback,name),"indirect readback registration");
    }
    void InitializeGpu()
    {
        if(gpuReady)return;const auto started=Clock::now();
        Hr(kernel.Initialize(context,GetExecutableDirectory()/"Experiments/Indirect.hlsl",text),"indirect compute initialization");
        for(auto& slot:slots)
        {
            Hr(kernel.CreateOutput(capacity,capacity,slot.output),"indirect slot buffers");
            D3D12_QUERY_HEAP_DESC q{};q.Type=D3D12_QUERY_HEAP_TYPE_TIMESTAMP;q.Count=8;
            Hr(context.device->CreateQueryHeap(&q,IID_PPV_ARGS(slot.queries.GetAddressOf())),"indirect timing heap");Readback(64,slot.readback,"Indirect.SampleTiming");
        }
        gpuInitialization=Ms(started);gpuReady=true;
    }
    void Collect(Slot& slot)
    {
        if(!slot.fence)return;
        Check(WaitD3D12Submission(presentation,slot.fence,text,D3D12WaitReason::FrameReuse),"indirect slot completion");
        CpuScope scope(profiler,"CPU.IndirectQueryRead");std::array<UINT64,8> ticks{};void* mapped=nullptr;
        D3D12_RANGE range{0,64},none{0,0};Hr(slot.readback->Map(0,&range,&mapped),"indirect timing map");std::memcpy(ticks.data(),mapped,64);slot.readback->Unmap(0,&none);
        Check(ticks[0]>0&&std::is_sorted(ticks.begin(),ticks.begin()+5)&&std::is_sorted(ticks.begin()+5,ticks.end()),
            "indirect and HZB measured timestamp order");indirectTimings[slot.sourceFrame]=ticks;slot.fence=0;++timingReads;
    }
    rhi::Image Render(const IndirectScene& scene,bool gpu,bool capture,bool occlusion)
    {
        const auto started=Clock::now();
        Owner();Check(!stopped&&scene.instances.size()<=capacity&&scene.instances.size()==scene.bounds.size()&&ValidVisibilityFrustum(scene.frustum),"indirect scene contract");
        for(size_t i=0;i<scene.bounds.size();++i)Check(scene.bounds[i].id==i,"indirect index IDs match instances");
        if(occlusion)Check(gpu,"occlusion requires the GPU indirect path");
        IndirectFrame measured;measured.sequence=++sequence;measured.gpu=gpu;measured.inputCount=static_cast<UINT>(scene.instances.size());
        measured.cpuValidationMs=Ms(started);Check(profiler.BeginFrame(sequence),"indirect CPU frame begin");
        struct FrameLifetime{Profiler& profiler;bool ended=false;~FrameLifetime(){if(!ended)profiler.EndFrame();}} cpuFrame{profiler};
        Check(profiler.RecordCpuDuration("CPU.Validation",measured.cpuValidationMs),"indirect validation timing");
        const UINT index=presentation.frameCursor;Collect(slots[index]);
        if(gpu)InitializeGpu();
        bool useOcclusion=false;
        if(occlusion)
        {
            HiZFrameState state;state.viewProjection=scene.viewProjection;state.cameraPosition={0,0,6};state.cameraForward={0,0,-1};
            state.verticalFov=Pi/3;state.width=presentation.width;state.height=presentation.height;
            useOcclusion=hizHistory.Update(state)==HiZInvalidation::None;
            if(useOcclusion&&(!hizOutput.depth||hizOutput.depth.Get()!=presentation.depth.Get()
                ||hizOutput.width!=presentation.width||hizOutput.height!=presentation.height))
                Hr(hiz.CreateOutput(*presentation.depth.Get(),presentation.width,presentation.height,hizOutput),"occlusion HZB output");
        }
        measured.occlusion=useOcclusion;
        if(!gpu)
        {
            auto phase=Clock::now();Check(CullVisibilityReference(scene.bounds,scene.frustum,visible),"indirect CPU reference");measured.cpuCullMs=Ms(phase);
            phase=Clock::now();selected.clear();selected.reserve(visible.size());for(auto id:visible)selected.push_back(scene.instances[id]);measured.cpuGatherMs=Ms(phase);measured.cpuVisible=static_cast<UINT>(visible.size());
        }
        D3D12MeshDraw draw;draw.mesh=&mesh;draw.pbr.cameraPosition={0,0,6};draw.pbr.directionToLight={.3f,.5f,.8f};draw.pbr.radiance={3,2,1};
        draw.instances=gpu?std::span<const rhi::InstanceParameters>(scene.instances):std::span<const rhi::InstanceParameters>(selected);
        auto& slot=slots[index];D3D12IndirectBatch batch{&kernel,&slot.output,scene.bounds,scene.frustum,{slot.queries.Get(),8},0,slot.readback.Get()};
        if(useOcclusion)
        {
            batch.hizKernel=&hiz;batch.hiz=&hizOutput;batch.prepass=&prepass;batch.depthBias=1e-6f;
            batch.hizQueries=slot.queries.Get();batch.hizQueryCapacity=8;batch.hizFirstQuery=5;
        }
        D3D12FrameContent content;content.meshPipeline=useOcclusion?&colorPipeline:&pipeline;content.viewProjection=scene.viewProjection;content.sourceFrame=sequence;content.cacheOpaqueBindings=true;
        if(gpu||!selected.empty())content.meshes={&draw,1};if(gpu){content.indirects=&batch;content.indirectCount=1;}
        ComPtr<ID3D12Resource> copy;D3D12FrameCapture request;UINT64 total=0;
        // 此守卫必须先于copy析构，异常时仍由原呈现协议证明安全。
        struct CopyLifetime{D3D12Presentation& p;bool pending=false;~CopyLifetime(){if(pending)ReleaseD3D12Presentation(p);}} lifetime{presentation};
        if(capture)
        {
            const auto d=presentation.backBuffers[presentation.swapChain->GetCurrentBackBufferIndex()]->GetDesc();
            context.device->GetCopyableFootprints(&d,0,1,0,&request.footprint,nullptr,nullptr,&total);Readback(total,copy,"Indirect.ImageCapture");request.readback=copy.Get();
        }
        const auto render=Clock::now();lifetime.pending=capture;
        Check(RenderD3D12Frame(presentation,text,content,capture?&request:nullptr),"indirect actual render");measured.cpuRenderMs=Ms(render);
        if(gpu){slot.fence=presentation.frames[index].fenceValue;slot.sourceFrame=sequence;lastSlotIndex=index;lastSlotValid=true;}
        rhi::Image image;
        if(capture)
        {
            Check(WaitForD3D12Gpu(presentation,text,D3D12WaitReason::Readback),"indirect image completion");lifetime.pending=false;
            image={presentation.width,presentation.height,std::vector<std::uint8_t>(SIZE_T{presentation.width}*presentation.height*4)};
            void* mapped=nullptr;D3D12_RANGE range{0,static_cast<SIZE_T>(total)},none{0,0};Hr(copy->Map(0,&range,&mapped),"indirect image map");
            for(UINT row=0;row<image.height;++row)std::memcpy(image.rgba8.data()+SIZE_T{row}*image.width*4,
                static_cast<const std::byte*>(mapped)+request.footprint.Offset+SIZE_T{row}*request.footprint.Footprint.RowPitch,SIZE_T{image.width}*4);
            copy->Unmap(0,&none);++imageReadbacks;
        }
        measured.cpuFrameMs=Ms(started);frames.push_back(measured);if(frames.size()>6000){indirectTimings.erase(frames.front().sequence);frames.pop_front();++dropped;}
        Check(profiler.EndFrame(),"indirect CPU frame end");cpuFrame.ended=true;return image;
    }
    void Drain()
    {
        Owner();if(stopped)return;Check(WaitForD3D12Gpu(presentation,text,D3D12WaitReason::Shutdown),"indirect final completion");
        for(auto& slot:slots)Collect(slot);
        for(auto& frame:frames)
        {
            const auto found=std::find_if(presentation.timings.begin(),presentation.timings.end(),[&](const auto& value){return value.sourceFrame==frame.sequence;});
            Check(found!=presentation.timings.end(),"indirect native GPU timing available");frame.drawing=*found;
            if(frame.gpu)
            {
                Check(indirectTimings.contains(frame.sequence),"indirect custom timing available");
                const auto& ticks=indirectTimings.at(frame.sequence);
                for(std::size_t i=0;i<frame.indirectTicks.size();++i)frame.indirectTicks[i]=ticks[i];
                if(frame.occlusion)for(std::size_t i=0;i<frame.hizTicks.size();++i)frame.hizTicks[i]=ticks[5+i];
            }
        }
    }
    std::uint32_t ReadVisibleCount()
    {
        Owner();Check(lastSlotValid&&lastSlotIndex<slots.size()&&slots[lastSlotIndex].output.arguments,
            "visible count needs a completed GPU frame");
        if(!visibilityAllocator)
        {
            Hr(context.device->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT,
                IID_PPV_ARGS(visibilityAllocator.GetAddressOf())),"visible count allocator");
            Hr(context.device->CreateCommandList(0,D3D12_COMMAND_LIST_TYPE_DIRECT,visibilityAllocator.Get(),nullptr,
                IID_PPV_ARGS(visibilityCommands.GetAddressOf())),"visible count commands");
            Hr(visibilityCommands->Close(),"visible count initial close");
            Readback(48,visibilityReadback,"Indirect.VisibleCount");
        }
        Check(WaitForD3D12Gpu(presentation,text,D3D12WaitReason::Readback),"visible count wait");
        Hr(visibilityAllocator->Reset(),"visible count allocator reset");
        Hr(visibilityCommands->Reset(visibilityAllocator.Get(),nullptr),"visible count list reset");
        D3D12StateRecording states(*context.states);
        auto* arguments=slots[lastSlotIndex].output.arguments.Get();
        Hr(states.Use(*visibilityCommands.Get(),*arguments,D3D12_RESOURCE_STATE_COPY_SOURCE,"Indirect.VisibleCount.Source"),"visible count source state");
        Hr(states.Use(*visibilityCommands.Get(),*visibilityReadback.Get(),D3D12_RESOURCE_STATE_COPY_DEST,"Indirect.VisibleCount.Target"),"visible count target state");
        visibilityCommands->CopyBufferRegion(visibilityReadback.Get(),0,arguments,0,48);
        Hr(states.Use(*visibilityCommands.Get(),*arguments,D3D12_RESOURCE_STATE_INDIRECT_ARGUMENT,"Indirect.VisibleCount.Restore"),"visible count restore state");
        Hr(visibilityCommands->Close(),"visible count close");
        ID3D12CommandList* lists[]{visibilityCommands.Get()};
        presentation.gpuWorkPending=true;presentation.untrackedSubmission=true;
        presentation.queue->ExecuteCommandLists(1,lists);states.Commit();
        Check(WaitForD3D12Gpu(presentation,text,D3D12WaitReason::Readback),"visible count completion");
        void* mapped=nullptr;const D3D12_RANGE range{0,48},none{0,0};
        Hr(visibilityReadback->Map(0,&range,&mapped),"visible count map");
        std::uint32_t words[12]{};std::memcpy(words,mapped,48);visibilityReadback->Unmap(0,&none);
        return words[1];
    }
    IndirectOcclusionCounters ReadOcclusionCounters()
    {
        IndirectOcclusionCounters counters;
        if(frames.empty()||!lastSlotValid)return counters;
        counters.enabled=frames.back().occlusion;
        counters.candidates=frames.back().inputCount;
        counters.visible=ReadVisibleCount();
        counters.culled=counters.candidates>=counters.visible?counters.candidates-counters.visible:0;
        counters.occlusionRate=counters.candidates>0?float(counters.culled)/float(counters.candidates):0;
        return counters;
    }
    // G06.06：把最近一帧 HZB mip0 读回 CPU；只用于可视化/证据，不进入逐帧路径。
    bool ReadHiz(std::vector<float>& depth,UINT& width,UINT& height)
    {
        Owner();depth.clear();width=0;height=0;
        if(!lastSlotValid||!hizOutput.texture)return false;
        Check(WaitForD3D12Gpu(presentation,text,D3D12WaitReason::Readback),"HZB preview wait");
        const auto description=hizOutput.texture->GetDesc();
        D3D12_PLACED_SUBRESOURCE_FOOTPRINT layout{};UINT rows=0;UINT64 rowBytes=0,total=0;
        context.device->GetCopyableFootprints(&description,0,1,0,&layout,&rows,&rowBytes,&total);
        ComPtr<ID3D12Resource> readback;Readback(total,readback,"Occlusion.HizPreview");
        if(!visibilityAllocator)
        {
            Hr(context.device->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT,
                IID_PPV_ARGS(visibilityAllocator.GetAddressOf())),"HZB preview allocator");
            Hr(context.device->CreateCommandList(0,D3D12_COMMAND_LIST_TYPE_DIRECT,visibilityAllocator.Get(),nullptr,
                IID_PPV_ARGS(visibilityCommands.GetAddressOf())),"HZB preview commands");
            Hr(visibilityCommands->Close(),"HZB preview initial close");
        }
        Hr(visibilityAllocator->Reset(),"HZB preview allocator reset");
        Hr(visibilityCommands->Reset(visibilityAllocator.Get(),nullptr),"HZB preview list reset");
        D3D12StateRecording states(*context.states);
        Hr(states.Use(*visibilityCommands.Get(),*hizOutput.texture.Get(),D3D12_RESOURCE_STATE_COPY_SOURCE,
            "Occlusion.HizPreview.Source"),"HZB preview source state");
        Hr(states.Use(*visibilityCommands.Get(),*readback.Get(),D3D12_RESOURCE_STATE_COPY_DEST,
            "Occlusion.HizPreview.Target"),"HZB preview target state");
        D3D12_TEXTURE_COPY_LOCATION destination{};destination.pResource=readback.Get();
        destination.Type=D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;destination.PlacedFootprint=layout;
        D3D12_TEXTURE_COPY_LOCATION source{};source.pResource=hizOutput.texture.Get();
        source.Type=D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;source.SubresourceIndex=0;
        visibilityCommands->CopyTextureRegion(&destination,0,0,0,&source,nullptr);
        Hr(states.Use(*visibilityCommands.Get(),*hizOutput.texture.Get(),D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE,
            "Occlusion.HizPreview.Restore"),"HZB preview restore state");
        Hr(visibilityCommands->Close(),"HZB preview close");
        ID3D12CommandList* lists[]{visibilityCommands.Get()};
        presentation.gpuWorkPending=true;presentation.untrackedSubmission=true;
        presentation.queue->ExecuteCommandLists(1,lists);states.Commit();
        Check(WaitForD3D12Gpu(presentation,text,D3D12WaitReason::Readback),"HZB preview completion");
        void* mapped=nullptr;const D3D12_RANGE range{0,static_cast<SIZE_T>(total)},none{0,0};
        Hr(readback->Map(0,&range,&mapped),"HZB preview map");
        width=static_cast<UINT>(description.Width);height=layout.Footprint.Height;
        depth.resize(std::size_t{width}*height);
        for(UINT row=0;row<height;++row)
            std::memcpy(depth.data()+std::size_t{row}*width,
                static_cast<const std::byte*>(mapped)+layout.Offset+UINT64{row}*layout.Footprint.RowPitch,
                std::size_t{width}*sizeof(float));
        readback->Unmap(0,&none);
        return width>0&&height>0&&depth.size()==std::size_t{width}*height;
    }
    nlohmann::json Statistics() const
    {
        Owner();UINT64 uploads=0,readbacks=0;const auto allocation=[&](ID3D12Resource* resource){if(!resource)return UINT64{0};const auto d=resource->GetDesc();return context.device->GetResourceAllocationInfo(0,1,&d).SizeInBytes;};
        for(const auto& frame:presentation.frames){uploads+=frame.uploads.Stats().reservedBytes+frame.baselineConstants.Stats().reservedBytes;readbacks+=allocation(frame.timestampReadback.Get());}
        for(const auto& slot:slots)readbacks+=allocation(slot.readback.Get());
        const auto memory=context.memory->Stats();const auto state=context.states->Stats();ComPtr<ID3D12InfoQueue> info;context.device.As(&info);UINT64 warnings=0,errors=0;
        if(info)for(UINT64 i=0;i<info->GetNumStoredMessages();++i){SIZE_T size=0;Hr(info->GetMessage(i,nullptr,&size),"indirect diagnostic size");std::vector<std::byte> bytes(size);auto* message=reinterpret_cast<D3D12_MESSAGE*>(bytes.data());Hr(info->GetMessage(i,message,&size),"indirect diagnostic");warnings+=message->Severity==D3D12_MESSAGE_SEVERITY_WARNING;errors+=message->Severity<D3D12_MESSAGE_SEVERITY_WARNING;}
        return {{"adapter",adapter},{"width",presentation.width},{"height",presentation.height},{"visible_window",IsWindowVisible(window.hwnd)!=FALSE},{"occluded",presentation.occluded},
            {"default_used_bytes",memory.used},{"default_reserved_bytes",memory.reserved},{"upload_reserved_bytes",uploads},{"readback_allocation_bytes",readbacks},
            {"tracked_resources",state.resources},{"uav_barriers",state.uavBarriers},{"submissions",sequence},{"result_readbacks",0},{"image_readbacks",imageReadbacks},
            {"indirect_timing_reads",timingReads},{"initialization_ms",initialization},{"gpu_initialization_ms",gpuInitialization},{"debug_info_queue",bool(info)},
            {"warnings",warnings},{"errors",errors},{"dropped_frames",dropped},{"profiler_dropped_frames",profiler.DroppedFrames()},{"gpu_dropped_frames",presentation.droppedTimingSamples},
            {"wait_counts",presentation.waitCounts},{"flush_counts",presentation.flushCounts},{"stopped",stopped},{"gpu_resources_created",gpuReady},
            {"cpu_selection_capacity_bytes",selected.capacity()*sizeof(rhi::InstanceParameters)},{"cpu_index_capacity_bytes",visible.capacity()*sizeof(UINT)}};
    }
    void Shutdown()
    {
        Owner();if(stopped)return;Drain();ReleaseD3D12Presentation(presentation);for(auto& slot:slots){slot.output={};slot.queries.Reset();slot.readback.Reset();}
        hizOutput={};hiz={};prepass={};colorPipeline={};hizHistory.Reset();
        visibilityCommands.Reset();visibilityAllocator.Reset();visibilityReadback.Reset();
        kernel={};mesh={};pipeline={};context.states->Collect();context.memory->Trim();window.postQuitOnDestroy=false;DestroyEngineWindow(window);stopped=true;
    }
};
IndirectExperiment::IndirectExperiment(const Localization& text,std::uint32_t capacity,bool visible):impl_(std::make_unique<Impl>(text,capacity)){impl_->Initialize(visible);}
IndirectExperiment::~IndirectExperiment()=default;
rhi::Image IndirectExperiment::Render(const IndirectScene& scene,bool gpu,bool capture,bool occlusion){return impl_->Render(scene,gpu,capture,occlusion);}
bool IndirectExperiment::Pump(bool& toggle)
{
    impl_->Owner();int code=0;
    for(;;)
    {
        if(!PumpWindowMessages(code)||impl_->window.closeRequested||impl_->window.callbackFailed)return false;
        if(impl_->window.drawable)break;
        MsgWaitForMultipleObjects(0,nullptr,FALSE,50,QS_ALLINPUT);
    }
    if(impl_->window.width!=impl_->presentation.width||impl_->window.height!=impl_->presentation.height)
    {
        impl_->Drain();Check(ResizeD3D12Presentation(impl_->presentation,impl_->window.width,impl_->window.height,impl_->text),"indirect client resize");
    }
    toggle=impl_->toggle;impl_->toggle=false;impl_->window.input.ClearEvents();return true;
}
void IndirectExperiment::Resize(std::uint32_t width,std::uint32_t height)
{
    impl_->Owner();impl_->Drain();Check(SetEngineWindowClientSize(impl_->window,width,height)&&ResizeD3D12Presentation(impl_->presentation,width,height,impl_->text),"indirect resize");
}
void IndirectExperiment::Drain(){impl_->Drain();}
std::uint32_t IndirectExperiment::LastVisibleCount(){return impl_->ReadVisibleCount();}
bool IndirectExperiment::ReadHiz(std::vector<float>& depth,std::uint32_t& width,std::uint32_t& height){return impl_->ReadHiz(depth,width,height);}
IndirectOcclusionCounters IndirectExperiment::LastOcclusionCounters(){return impl_->ReadOcclusionCounters();}
void IndirectExperiment::Shutdown(){impl_->Shutdown();}
std::vector<IndirectFrame> IndirectExperiment::Frames() const{impl_->Owner();return {impl_->frames.begin(),impl_->frames.end()};}
std::vector<CpuFrame> IndirectExperiment::CpuFrames() const{return impl_->profiler.Frames();}
nlohmann::json IndirectExperiment::Statistics() const{return impl_->Statistics();}
D3D12Context& IndirectExperiment::Context(){return impl_->context;}
D3D12Presentation& IndirectExperiment::Presentation(){return impl_->presentation;}
}
