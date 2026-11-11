#include "PlatformLayer/Graphics/Rhi.h"
#include "D3D12Buffer.h"
#include "D3D12StagedUpload.h"
#include "D3D12Mesh.h"
#include "D3D12Triangle.h"
#include "D3D12Presentation.h"
#include "D3D12Context.h"
#include "CoreLayer/Localization.h"
#include "CoreLayer/Utf.h"
#include "CoreLayer/Profiler.h"
#include "D3D12GpuTimer.h"
#include "D3D12Diagnostics.h"
#include "D3D12Memory.h"
#include "D3D12PipelineCache.h"
#include "D3D12Shadow.h"
#include "D3D12PostProcess.h"
#include "D3D12Gui.h"
#include "D3D12Indirect.h"
#include "D3D12HiZ.h"
#include "D3D12DepthPrepass.h"
#include "PlatformLayer/Windows/ExecutablePath.h"
#include "CoreLayer/Math/Half.h"
#include <d3d12sdklayers.h>
#include <algorithm>
#include <atomic>
#include <cstring>
#include <optional>
#include <thread>
#include <sstream>
#include <iomanip>
#include <unordered_set>
#include <limits>

namespace se::rhi
{
namespace
{
using Microsoft::WRL::ComPtr;
std::atomic<std::uint64_t> nextDevice{1};
struct Entry {virtual ~Entry()=default;std::uint64_t lastUse=0;};
struct BufferEntry : Entry
{
    ComPtr<ID3D12Resource> resource;
    BufferUsage usage=BufferUsage::Vertex;
    UINT bytes=0,stride=0,count=0,maxIndex=0;
    bool staged=false;UINT64 written=0,uploadFence=0;
};
struct TextureEntry : Entry {D3D12Texture native;std::shared_ptr<D3D12GuiTarget> target;bool staged=false;UINT writtenRows=0;UINT64 uploadFence=0;};
struct PipelineEntry : Entry {PipelineKind kind=PipelineKind::TexturedMesh;D3D12MeshPipeline mesh;D3D12Triangle triangle;};
template<class T,class H> struct Pool
{
    struct Slot {std::shared_ptr<T> data;std::uint32_t generation=1;};
    std::vector<Slot> slots;
    H Add(std::shared_ptr<T> data,std::uint64_t device)
    {
        for(std::size_t i=0;i<slots.size();++i)if(!slots[i].data&&slots[i].generation)
        {slots[i].data=std::move(data);return {device,static_cast<std::uint32_t>(i),slots[i].generation};}
        const auto index=static_cast<std::uint32_t>(slots.size());slots.push_back({std::move(data),1});return {device,index,1};
    }
    std::shared_ptr<T> Get(H handle,std::uint64_t device) const
    {
        if(!handle||handle.device!=device||handle.index>=slots.size())return {};
        const auto& slot=slots[handle.index];return slot.generation==handle.generation?slot.data:nullptr;
    }
    std::shared_ptr<T> Remove(H handle,std::uint64_t device)
    {
        auto value=Get(handle,device);if(!value)return {};
        auto& slot=slots[handle.index];slot.data.reset();++slot.generation;return value;
    }
    std::size_t Count() const {return static_cast<std::size_t>(std::count_if(slots.begin(),slots.end(),[](const auto& s){return bool(s.data);}));}
};
void Name(ID3D12Object& object,const std::string& name)
{
    std::u32string decoded;if(name.empty()||!DecodeUtf8(name,decoded))return;
    const auto utf16=EncodeUtf16(decoded);object.SetName(std::wstring(utf16.begin(),utf16.end()).c_str());
}
Result Fail(Error error){return {error};}
bool ValidateBufferContents(bool vertex,std::span<const std::byte> bytes,UINT stride,UINT& maximumIndex)
{
    for(std::size_t offset=0;offset<bytes.size();offset+=stride)
    {
        if(vertex)
        {
            Vertex v;std::memcpy(&v,bytes.data()+offset,sizeof(v));
            for(const auto value:v.position)if(!IsFinite(value))return false;
            for(const auto value:v.normal)if(!IsFinite(value))return false;
            for(const auto value:v.uv)if(!IsFinite(value))return false;
            for(const auto value:v.tangent)if(!IsFinite(value))return false;
            const float length=LengthSquared(Vec3{v.normal[0],v.normal[1],v.normal[2]});
            if(!IsFinite(length)||length<1e-12f)return false;
        }
        else{UINT index=0;std::memcpy(&index,bytes.data()+offset,sizeof(index));maximumIndex=std::max(maximumIndex,index);}
    }
    return true;
}
}
const char* Result::Key() const noexcept
{
    switch(error)
    {
    case Error::None:return "Rhi.Success";
    case Error::InvalidArgument:return "Rhi.InvalidArgument";
    case Error::InvalidHandle:return "Rhi.InvalidHandle";
    case Error::InvalidState:return "Rhi.InvalidState";
    case Error::Unsupported:return "Rhi.Unsupported";
    case Error::CapacityExceeded:return "Rhi.CapacityExceeded";
    default:return "Rhi.BackendFailure";
    }
}
struct Device::Impl
{
    const std::thread::id owner=std::this_thread::get_id();
    const std::uint64_t id=nextDevice.fetch_add(1);
    D3D12Context context;
    D3D12Presentation presentation;
    Localization text;
    Pool<BufferEntry,BufferHandle> buffers;
    Pool<TextureEntry,TextureHandle> textures;
    Pool<PipelineEntry,PipelineHandle> pipelines;
    struct Batch {std::uint64_t fence=0;std::vector<std::shared_ptr<Entry>> leases;};
    std::vector<Batch> inFlight;
    std::vector<std::shared_ptr<Entry>> retiring,pending;
    bool ready=false;
    std::uint64_t descriptorRejections=0;
    std::uint64_t stagedCopies=0,stagedPayloadBytes=0,stagedFootprintBytes=0;
    // G06.08：RHI 常驻的实例化遮挡资源。内核/PSO 只初始化一次，输出缓冲按帧槽复用，
    // HZB 输出只随 presentation.depth 或尺寸变化重建；首帧与 resize 后只做视锥剔除的间接绘制。
    struct OcclusionState
    {
        D3D12Indirect indirect;
        D3D12DepthPrepass prepass;
        const ID3D12RootSignature* prepassRoot=nullptr;
        D3D12HiZ hiz;
        bool kernelsReady=false,prepassReady=false;
        // G06.10：每个帧槽按批次数量保存输出；多批次各自一次 ExecuteIndirect。
        std::array<std::vector<D3D12IndirectOutput>,2> outputs;
        D3D12HiZOutput hizOutput;
        UINT width=0,height=0;
        bool prepassForward=false;
        // G06.09/G06.10：每个帧槽一块 READBACK，按批次 offset 存放 48B 参数；
        // counterFences 非零表示该槽的复制尚未完成，此时不覆盖，也不阻塞 CPU。
        std::array<std::vector<ComPtr<ID3D12Resource>>,2> counterReadbacks;
        std::array<std::uint64_t,2> counterFences{};
        std::array<std::uint64_t,2> counterFrames{};
        std::array<std::vector<std::uint32_t>,2> counterCandidates;
        OcclusionCounters counters;
        // G06.11：运行时遮挡时间。0/1 prepass、2..4 HZB、5 main end、
        // 批次 i 的 cull 为 6+4i..6+4i+3；每帧槽独立查询堆/READBACK。
        std::array<ComPtr<ID3D12QueryHeap>,2> telemetryQueries;
        std::array<ComPtr<ID3D12Resource>,2> telemetryReadbacks;
        std::array<std::uint64_t,2> telemetryFences{};
        std::array<std::uint64_t,2> telemetryFrames{};
        std::array<UINT,2> telemetryCounts{};
        std::uint64_t telemetryFrequency=0;
        OcclusionTiming timing;
    } occlusion;
    std::uint64_t occlusionFrames=0,occlusionInstances=0;
    bool EnsureOcclusion(const D3D12MeshPipeline& pipeline,bool forwardInstanced)
    {
        const auto experiments=GetExecutableDirectory()/"Experiments";
        if(!occlusion.kernelsReady)
        {
            if(FAILED(occlusion.indirect.Initialize(context,experiments/"Indirect.hlsl",text))
                ||FAILED(occlusion.hiz.Initialize(context,experiments/"HiZ.hlsl",text)))return false;
            occlusion.kernelsReady=true;
        }
        // prepass 的根签名必须与实例化主 PSO 同布局；Forward 实例化的实例 SRV 在参数 3。
        if(!occlusion.prepassReady||occlusion.prepassForward!=forwardInstanced||occlusion.prepassRoot!=pipeline.rootSignature.Get())
        {
            occlusion.prepass=D3D12DepthPrepass{};
            if(FAILED(occlusion.prepass.Initialize(*context.device.Get(),experiments/"DepthPrepass.hlsl",
                *pipeline.rootSignature.Get(),text,forwardInstanced?3u:2u)))return false;
            occlusion.prepassRoot=pipeline.rootSignature.Get();occlusion.prepassForward=forwardInstanced;occlusion.prepassReady=true;
        }
        return true;
    }
    // 只创建帧槽回读资源并登记初始状态；每槽至少 48B × 批次数，复用前由 counterFences 判定安全。
    bool EnsureOcclusionReadback(UINT slot,UINT batches)
    {
        if(slot>=occlusion.counterReadbacks.size()||!batches)return false;
        auto& readbacks=occlusion.counterReadbacks[slot];
        const UINT64 bytes=UINT64{batches}*D3D12Indirect::argumentBytes;
        if(!readbacks.empty()&&readbacks.front()&&readbacks.front()->GetDesc().Width>=bytes)return true;
        ComPtr<ID3D12Resource> readback;
        D3D12_HEAP_PROPERTIES heap{};
        heap.Type=D3D12_HEAP_TYPE_READBACK;heap.CreationNodeMask=heap.VisibleNodeMask=1;
        D3D12_RESOURCE_DESC description{};
        description.Dimension=D3D12_RESOURCE_DIMENSION_BUFFER;description.Width=bytes;
        description.Height=description.DepthOrArraySize=description.MipLevels=1;description.SampleDesc={1,0};
        description.Layout=D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
        if(FAILED(context.device->CreateCommittedResource(&heap,D3D12_HEAP_FLAG_NONE,&description,
            D3D12_RESOURCE_STATE_COPY_DEST,nullptr,IID_PPV_ARGS(readback.GetAddressOf()))))return false;
        if(!context.states||FAILED(context.states->Register(*readback.Get(),D3D12_RESOURCE_STATE_COPY_DEST,
            D3D12TrackedHeap::Readback,"Occlusion.CounterReadback")))return false;
        readbacks.clear();readbacks.push_back(std::move(readback));return true;
    }
    // 非阻塞收集：只映射 Fence 已完成且 READY 的槽；不等待 GPU，也不改变绘制路径。
    void PollOcclusionCounters()
    {
        const auto completed=Completed();
        if(completed==UINT64_MAX)return;
        for(std::size_t slot=0;slot<occlusion.counterFences.size();++slot)
        {
            const auto fence=occlusion.counterFences[slot];
            if(!fence||completed<fence)continue;
            auto& readbacks=occlusion.counterReadbacks[slot];
            occlusion.counterFences[slot]=0;
            if(readbacks.empty()||!readbacks.front())continue;
            auto& readback=readbacks.front();
            void* mapped=nullptr;const auto& candidates=occlusion.counterCandidates[slot];
            const auto bytes=UINT64{candidates.size()}*D3D12Indirect::argumentBytes;
            const D3D12_RANGE range{0,static_cast<SIZE_T>(bytes)};
            if(FAILED(readback->Map(0,&range,&mapped))||!mapped)continue;
            const auto* words=static_cast<const std::uint32_t*>(mapped);
            OcclusionCounters next;next.valid=true;next.frame=occlusion.counterFrames[slot];
            for(std::size_t batch=0;batch<candidates.size();++batch)
            {
                const auto* arguments=words+batch*(D3D12Indirect::argumentBytes/sizeof(std::uint32_t));
                // Indirect.hlsl 的 48B 参数：words[1] 为可见实例数，words[8] 为溢出标志。
                next.visible+=arguments[1];next.overflow|=arguments[8]!=0;next.candidates+=candidates[batch];
            }
            const D3D12_RANGE noWrites{0,0};readback->Unmap(0,&noWrites);
            next.culled=next.candidates>=next.visible?next.candidates-next.visible:0;
            next.occlusionRate=next.candidates>0?float(next.culled)/float(next.candidates):0;
            if(!occlusion.counters.valid||next.frame>=occlusion.counters.frame)occlusion.counters=next;
        }
    }
    bool EnsureOcclusionTelemetry(UINT slot,UINT queries)
    {
        if(slot>=occlusion.telemetryQueries.size()||queries<6)return false;
        if(!occlusion.telemetryFrequency)
        {
            UINT64 frequency=0;
            if(!presentation.queue||FAILED(presentation.queue->GetTimestampFrequency(&frequency))||!frequency)return false;
            occlusion.telemetryFrequency=frequency;
        }
        if(occlusion.telemetryQueries[slot]&&occlusion.telemetryCounts[slot]==queries)return true;
        D3D12_QUERY_HEAP_DESC heapDescription{};heapDescription.Type=D3D12_QUERY_HEAP_TYPE_TIMESTAMP;heapDescription.Count=queries;
        ComPtr<ID3D12QueryHeap> heap;
        if(FAILED(context.device->CreateQueryHeap(&heapDescription,IID_PPV_ARGS(heap.GetAddressOf()))))return false;
        D3D12_HEAP_PROPERTIES heapProperties{};
        heapProperties.Type=D3D12_HEAP_TYPE_READBACK;heapProperties.CreationNodeMask=heapProperties.VisibleNodeMask=1;
        D3D12_RESOURCE_DESC buffer{};
        buffer.Dimension=D3D12_RESOURCE_DIMENSION_BUFFER;buffer.Width=UINT64{queries}*sizeof(UINT64);
        buffer.Height=buffer.DepthOrArraySize=buffer.MipLevels=1;buffer.SampleDesc={1,0};buffer.Layout=D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
        ComPtr<ID3D12Resource> readback;
        if(FAILED(context.device->CreateCommittedResource(&heapProperties,D3D12_HEAP_FLAG_NONE,&buffer,
            D3D12_RESOURCE_STATE_COPY_DEST,nullptr,IID_PPV_ARGS(readback.GetAddressOf()))))return false;
        if(!context.states||FAILED(context.states->Register(*readback.Get(),D3D12_RESOURCE_STATE_COPY_DEST,
            D3D12TrackedHeap::Readback,"Occlusion.Telemetry")))return false;
        occlusion.telemetryQueries[slot]=std::move(heap);occlusion.telemetryReadbacks[slot]=std::move(readback);
        occlusion.telemetryCounts[slot]=queries;return true;
    }
    // 非阻塞收集：只映射 Fence 已完成的槽；时间戳顺序异常时保留旧样本，不猜测。
    void PollOcclusionTelemetry()
    {
        const auto completed=Completed();
        if(completed==UINT64_MAX)return;
        for(std::size_t slot=0;slot<occlusion.telemetryFences.size();++slot)
        {
            const auto fence=occlusion.telemetryFences[slot];
            if(!fence||completed<fence)continue;
            occlusion.telemetryFences[slot]=0;
            const auto count=occlusion.telemetryCounts[slot];
            auto& readback=occlusion.telemetryReadbacks[slot];
            if(!readback||count<6||(count-6)%4)continue;
            std::vector<UINT64> raw(count);
            void* mapped=nullptr;const D3D12_RANGE range{0,static_cast<SIZE_T>(count*sizeof(UINT64))};
            if(FAILED(readback->Map(0,&range,&mapped))||!mapped)continue;
            std::memcpy(raw.data(),mapped,range.End);
            const D3D12_RANGE noWrites{0,0};readback->Unmap(0,&noWrites);
            const auto batches=(count-6)/4;
            if(raw[1]<raw[0]||raw[2]<raw[1]||raw[3]<raw[2]||raw[4]<raw[3]||raw[6]<raw[4]
                ||raw[5]<raw[6+4*batches-1])continue;
            double cullTicks=0;UINT64 previous=raw[4];bool ordered=true;
            for(UINT batch=0;batch<batches;++batch)
            {
                const auto begin=raw[6+4*batch],end=raw[6+4*batch+3];
                if(begin<previous||end<begin){ordered=false;break;}
                cullTicks+=double(end-begin);previous=end;
            }
            if(!ordered)continue;
            auto& timing=occlusion.timing;
            timing.valid=true;timing.frequency=occlusion.telemetryFrequency;
            timing.frame=occlusion.telemetryFrames[slot];
            const auto milliseconds=[](UINT64 ticks,UINT64 frequency){return frequency?double(ticks)*1000.0/double(frequency):0.0;};
            timing.prepassMs=milliseconds(raw[1]-raw[0],timing.frequency);
            timing.hzbMs=milliseconds(raw[4]-raw[2],timing.frequency);
            timing.cullMs=timing.frequency?cullTicks*1000.0/double(timing.frequency):0.0;
            timing.mainMs=milliseconds(raw[5]-raw[6+4*batches-1],timing.frequency);
            timing.totalMs=milliseconds(raw[5]-raw[0],timing.frequency);
        }
    }
    bool Usable() const{return ready&&owner==std::this_thread::get_id()&&!presentation.failed;}
    std::uint64_t Completed() const{return presentation.fence?presentation.fence->GetCompletedValue():0;}
    bool Ready(const BufferEntry& entry) const
    {if(!entry.staged)return true;const auto completed=Completed();return entry.written==entry.bytes&&entry.uploadFence&&completed!=UINT64_MAX&&completed>=entry.uploadFence;}
    bool Ready(const TextureEntry& entry) const
    {if(!entry.staged)return true;const auto completed=Completed();return entry.writtenRows==entry.native.height&&entry.uploadFence&&completed!=UINT64_MAX&&completed>=entry.uploadFence;}
    void Collect()
    {
        const auto completed=Completed();if(completed==UINT64_MAX)return;
        PollOcclusionCounters();
        PollOcclusionTelemetry();
        CollectD3D12UploadReferences(presentation);
        std::erase_if(inFlight,[&](const auto& b){return b.fence<=completed;});
        std::erase_if(retiring,[&](const auto& value){return value.use_count()==1&&value->lastUse<=completed;});
        if(context.states)context.states->Collect();
    }
    ~Impl(){ReleaseD3D12Presentation(presentation);}
};
struct CommandContext::Impl
{
    bool cacheOpaqueBindings=false;
    std::weak_ptr<Device::Impl> device;
    int phase=0;PassKind kind=PassKind::Clear;
    std::vector<D3D12Mesh> geometry;
    std::vector<DrawIndexed> draws;
    std::vector<std::shared_ptr<PipelineEntry>> drawPipelines; // 与 draws 一一对应的实际 Pipeline。
    std::vector<std::vector<InstanceParameters>> instances;
    size_t instanceCount=0;
    std::vector<std::shared_ptr<Entry>> leases;
    std::shared_ptr<PipelineEntry> pipeline;
    PipelineHandle pipelineHandle;
    // G06.08/G06.10：本帧的实例化遮挡批次声明；Submit 时转成 D3D12IndirectBatch 数组。
    struct OcclusionFrame
    {
        bool active=false;
        std::vector<std::vector<se::VisibilityItem>> bounds;
        se::VisibilityFrustum frustum{};
        OcclusionSettings settings;
    } occlusion;
    Mat4 viewProjection;
    std::uint64_t sourceFrame=0;
    std::unordered_set<std::string> descriptors;
    std::optional<ForwardLighting> forward;
    std::shared_ptr<PipelineEntry> outputPipeline;
    std::shared_ptr<PipelineEntry> skyPipeline;
    std::optional<ShadowSettings> shadow;
    std::shared_ptr<PipelineEntry> shadowPipeline;
    std::vector<D3D12Mesh> shadowGeometry;
    std::vector<Mat4> shadowWorlds;
    std::optional<EnvironmentBindings> environment;
    std::array<std::shared_ptr<TextureEntry>,3> environmentTextures;
    std::optional<PostProcessSettings> post;
    std::array<std::shared_ptr<PipelineEntry>,5> postPipelines;
    std::vector<OverlayQuad> overlays;
    std::shared_ptr<TextureEntry> guiScene,guiComposite;
    std::shared_ptr<PipelineEntry> guiPipeline;
    GuiGeometry guiGeometry;
    std::vector<std::shared_ptr<TextureEntry>> guiTextures;
    std::optional<FrameComposition> composition;
    std::shared_ptr<TextureEntry> compositionHdr,compositionLdr;
    std::shared_ptr<PipelineEntry> gameUiPipeline;
    GuiGeometry gameUiGeometry;
    std::vector<std::shared_ptr<TextureEntry>> gameUiTextures;
};
Device::Device():impl_(std::make_shared<Impl>()){}
Device::~Device(){Shutdown();}
std::shared_ptr<Device> Device::CreateD3D12(Surface surface,const Localization& text,Result& result,MemorySettings memory)
{
    result=Fail(Error::InvalidArgument);
    if(!surface.window||!surface.width||!surface.height)return {};
    auto device=std::shared_ptr<Device>(new Device);auto& p=*device->impl_;p.text=text;
    if(!InitializeD3D12Context(p.context,text)){result=Fail(Error::BackendFailure);return {};}
    if(!p.context.memory->Configure(memory)){result=Fail(Error::InvalidArgument);return {};}
    if(!InitializeD3D12Presentation(p.presentation,p.context,static_cast<HWND>(surface.window),text))
    {result=Fail(p.context.memory->Stats().budgetFailures?Error::CapacityExceeded:Error::BackendFailure);return {};}
    p.ready=true;result={};return device;
}
Result Device::CreateBuffer(const BufferDescription& description,std::span<const std::byte> bytes,Buffer& output)
{
    if(!impl_||!impl_->Usable())return Fail(Error::InvalidState);
    if(output||bytes.empty()||bytes.size()>256ull*1024*1024)return Fail(Error::InvalidArgument);
    const bool vertex=description.usage==BufferUsage::Vertex;
    const bool packedVertex=vertex&&description.stride==16;
    const bool narrowIndex=!vertex&&description.stride==2;
    const UINT stride=vertex?description.stride:(narrowIndex?2u:4u);
    if((description.usage!=BufferUsage::Vertex&&description.usage!=BufferUsage::Index)
        ||(vertex&&!packedVertex&&description.stride!=sizeof(Vertex))
        ||(!vertex&&description.stride!=stride)||bytes.size()%stride)
        return Fail(Error::Unsupported);
    auto entry=std::make_shared<BufferEntry>();entry->usage=description.usage;entry->stride=stride;
    entry->bytes=static_cast<UINT>(bytes.size());entry->count=entry->bytes/stride;
    if(!packedVertex&&!narrowIndex&&!ValidateBufferContents(vertex,bytes,stride,entry->maxIndex))return Fail(Error::InvalidArgument);
    if(narrowIndex)
    {
        for(std::size_t offset=0;offset<bytes.size();offset+=stride)
        {
            std::uint16_t value=0;std::memcpy(&value,bytes.data()+offset,sizeof(value));
            entry->maxIndex=(std::max)(entry->maxIndex,static_cast<UINT>(value));
        }
    }
    if(!vertex&&entry->count%3)return Fail(Error::InvalidArgument);
    const auto budgetFailures=impl_->context.memory->Stats().budgetFailures;
    if(!UploadD3D12Buffer(entry->resource,impl_->context,impl_->presentation,bytes,
        vertex?D3D12_RESOURCE_STATE_VERTEX_AND_CONSTANT_BUFFER:D3D12_RESOURCE_STATE_INDEX_BUFFER,impl_->text))
        return Fail(impl_->context.memory->Stats().budgetFailures>budgetFailures?Error::CapacityExceeded:Error::BackendFailure);
    Name(*entry->resource.Get(),description.name);
    impl_->context.states->SetName(*entry->resource.Get(),description.name);
    impl_->context.memory->SetName(*entry->resource.Get(),description.name);
    const auto handle=impl_->buffers.Add(std::move(entry),impl_->id);const auto weak=weak_from_this();
    output=Buffer(new BufferHandle(handle),[weak](const BufferHandle* value){if(auto device=weak.lock())device->Destroy(*value);delete value;});
    return {};
}
Result Device::CreateTexture(const TextureDescription& description,std::span<const std::uint8_t> pixels,Texture& output)
{
    if(!impl_||!impl_->Usable())return Fail(Error::InvalidState);
    if(output)return Fail(Error::InvalidArgument);
    if(description.mipLevels!=1||description.samples!=1||description.arrayLayers!=1||description.cube
        ||description.format!=TextureFormat::Rgba8)return Fail(Error::Unsupported);
    const auto bpp=4;
    if(pixels.size()!=UINT64(description.width)*description.height*bpp)return Fail(Error::InvalidArgument);
    const TextureSubresource subresource{pixels,0};return CreateTextureSubresources(description,std::span(&subresource,1),output);
}
Result Device::CreateRenderTarget(const RenderTargetDescription& d,Texture& output)
{
    if(!impl_||!impl_->Usable()||impl_->presentation.uploadStates)return Fail(Error::InvalidState);
    if(output||!d.width||!d.height||d.width>8192||d.height>8192)return Fail(Error::InvalidArgument);
    auto entry=std::make_shared<TextureEntry>();entry->target=std::make_shared<D3D12GuiTarget>();
    if(!CreateD3D12GuiTarget(impl_->presentation,d,*entry->target,impl_->text))return Fail(Error::BackendFailure);
    entry->native=entry->target->color;
    const auto handle=impl_->textures.Add(std::move(entry),impl_->id);const auto weak=weak_from_this();
    output=Texture(new TextureHandle(handle),[weak](const TextureHandle* value){if(auto device=weak.lock())device->Destroy(*value);delete value;});return {};
}
Result Device::CreateTextureSubresources(const TextureDescription& description,std::span<const TextureSubresource> subresources,Texture& output)
{
    if(!impl_||!impl_->Usable())return Fail(Error::InvalidState);
    if(output)return Fail(Error::InvalidArgument);
    if(!ValidD3D12TextureData(description,subresources))return Fail(Error::InvalidArgument);
    if(!impl_->context.srvDescriptors->CanAllocate(2)){++impl_->descriptorRejections;return Fail(Error::CapacityExceeded);}
    auto entry=std::make_shared<TextureEntry>();
    const auto budgetFailures=impl_->context.memory->Stats().budgetFailures;
    if(!InitializeD3D12Texture(entry->native,impl_->context,impl_->presentation,description,subresources,impl_->text))
        return Fail(impl_->context.memory->Stats().budgetFailures>budgetFailures?Error::CapacityExceeded:Error::BackendFailure);
    Name(*entry->native.resource.Get(),description.name);
    impl_->context.states->SetName(*entry->native.resource.Get(),description.name);
    impl_->context.memory->SetName(*entry->native.resource.Get(),description.name);
    const auto handle=impl_->textures.Add(std::move(entry),impl_->id);const auto weak=weak_from_this();
    output=Texture(new TextureHandle(handle),[weak](const TextureHandle* value){if(auto device=weak.lock())device->Destroy(*value);delete value;});return {};
}
Result Device::SetTextureMipRange(const Texture& texture,std::uint32_t mostDetailedMip)
{
    if(!impl_||!impl_->Usable())return Fail(Error::InvalidState);
    if(!texture)return Fail(Error::InvalidHandle);
    auto entry=impl_->textures.Get(*texture,impl_->id);
    if(!entry||entry->target)return Fail(Error::Unsupported);
    auto& native=entry->native;
    if(native.format!=TextureFormat::Bc1Unorm&&native.format!=TextureFormat::Bc1UnormSrgb
        &&native.format!=TextureFormat::Bc5Unorm&&native.format!=TextureFormat::Bc4Unorm)return Fail(Error::Unsupported);
    if(mostDetailedMip>=native.mipLevels)return Fail(Error::InvalidArgument);
    if(native.mostDetailedMip==mostDetailedMip)return {};
    if(!impl_->context.srvDescriptors->CanAllocate(2)){++impl_->descriptorRejections;return Fail(Error::CapacityExceeded);}
    if(!AllocateD3D12TextureViews(native,impl_->context,mostDetailedMip,impl_->text))return Fail(Error::BackendFailure);
    return {};
}
Result Device::CreateTextureSubresourcesRange(const TextureDescription& description,std::uint32_t firstMip,
    std::span<const TextureSubresource> subresources,Texture& output)
{
    if(!impl_||!impl_->Usable())return Fail(Error::InvalidState);
    if(output)return Fail(Error::InvalidArgument);
    if(!ValidD3D12TextureSubrange(description,firstMip,subresources))return Fail(Error::InvalidArgument);
    if(!impl_->context.srvDescriptors->CanAllocate(2)){++impl_->descriptorRejections;return Fail(Error::CapacityExceeded);}
    auto entry=std::make_shared<TextureEntry>();
    const auto budgetFailures=impl_->context.memory->Stats().budgetFailures;
    if(!AllocateD3D12Texture(entry->native,impl_->context,description,impl_->text))
        return Fail(impl_->context.memory->Stats().budgetFailures>budgetFailures?Error::CapacityExceeded:Error::BackendFailure);
    if(!UploadD3D12TextureMips(entry->native,impl_->context,impl_->presentation,firstMip,subresources,impl_->text))
        return Fail(Error::BackendFailure);
    Name(*entry->native.resource.Get(),description.name);
    impl_->context.states->SetName(*entry->native.resource.Get(),description.name);
    impl_->context.memory->SetName(*entry->native.resource.Get(),description.name);
    const auto handle=impl_->textures.Add(std::move(entry),impl_->id);const auto weak=weak_from_this();
    output=Texture(new TextureHandle(handle),[weak](const TextureHandle* value){if(auto device=weak.lock())device->Destroy(*value);delete value;});return {};
}
Result Device::UploadTextureSubresourcesRange(const Texture& texture,std::uint32_t firstMip,
    std::span<const TextureSubresource> subresources)
{
    if(!impl_||!impl_->Usable())return Fail(Error::InvalidState);
    if(!texture)return Fail(Error::InvalidHandle);
    auto entry=impl_->textures.Get(*texture,impl_->id);
    if(!entry||entry->target)return Fail(Error::Unsupported);
    if(!UploadD3D12TextureMips(entry->native,impl_->context,impl_->presentation,firstMip,subresources,impl_->text))
        return Fail(Error::BackendFailure);
    return {};
}
Result Device::CreateStagedBuffer(const BufferDescription& description,std::uint64_t bytes,Buffer& output)
{
    if(!impl_||!impl_->Usable())return Fail(Error::InvalidState);
    if(output||!bytes||bytes>256ull*1024*1024)return Fail(Error::InvalidArgument);
    const bool vertex=description.usage==BufferUsage::Vertex;const bool packedVertex=vertex&&description.stride==16;
    const bool narrowIndex=!vertex&&description.stride==2;
    const UINT stride=vertex?description.stride:(narrowIndex?2u:4u);
    if((description.usage!=BufferUsage::Vertex&&description.usage!=BufferUsage::Index)
        ||(vertex&&!packedVertex&&description.stride!=sizeof(Vertex))
        ||(!vertex&&description.stride!=stride)||bytes%stride)return Fail(Error::Unsupported);
    if(!vertex&&(bytes/stride)%3)return Fail(Error::InvalidArgument);
    auto entry=std::make_shared<BufferEntry>();entry->usage=description.usage;entry->stride=stride;entry->bytes=static_cast<UINT>(bytes);entry->count=entry->bytes/stride;entry->staged=true;
    const auto failures=impl_->context.memory->Stats().budgetFailures;
    if(!AllocateD3D12Buffer(entry->resource,impl_->context,bytes,impl_->text))return Fail(impl_->context.memory->Stats().budgetFailures>failures?Error::CapacityExceeded:Error::BackendFailure);
    Name(*entry->resource.Get(),description.name);impl_->context.states->SetName(*entry->resource.Get(),description.name);impl_->context.memory->SetName(*entry->resource.Get(),description.name);
    const auto handle=impl_->buffers.Add(std::move(entry),impl_->id);const auto weak=weak_from_this();
    output=Buffer(new BufferHandle(handle),[weak](const BufferHandle* value){if(auto device=weak.lock())device->Destroy(*value);delete value;});return {};
}
Result Device::CreateStagedTexture(const TextureDescription& description,Texture& output)
{
    if(!impl_||!impl_->Usable())return Fail(Error::InvalidState);
    if(output)return Fail(Error::InvalidArgument);
    if(description.format!=TextureFormat::Rgba8||description.mipLevels!=1||description.arrayLayers!=1||description.cube)return Fail(Error::Unsupported);
    UploadFootprint one;if(!D3D12TextureUploadFootprint(impl_->context,description,1,one))return Fail(Error::InvalidArgument);
    if(!impl_->context.srvDescriptors->CanAllocate(2)){++impl_->descriptorRejections;return Fail(Error::CapacityExceeded);}
    auto entry=std::make_shared<TextureEntry>();entry->staged=true;const auto failures=impl_->context.memory->Stats().budgetFailures;
    if(!AllocateD3D12Texture(entry->native,impl_->context,description,impl_->text))return Fail(impl_->context.memory->Stats().budgetFailures>failures?Error::CapacityExceeded:Error::BackendFailure);
    Name(*entry->native.resource.Get(),description.name);impl_->context.states->SetName(*entry->native.resource.Get(),description.name);impl_->context.memory->SetName(*entry->native.resource.Get(),description.name);
    const auto handle=impl_->textures.Add(std::move(entry),impl_->id);const auto weak=weak_from_this();
    output=Texture(new TextureHandle(handle),[weak](const TextureHandle* value){if(auto device=weak.lock())device->Destroy(*value);delete value;});return {};
}
Result Device::QueryBufferUploadFootprint(std::uint64_t payloadBytes,UploadFootprint& output) const
{
    if(!impl_||!impl_->Usable())return Fail(Error::InvalidState);
    return D3D12BufferUploadFootprint(payloadBytes,output)?Result{}:Fail(Error::InvalidArgument);
}
Result Device::QueryTextureUploadFootprint(const TextureDescription& description,std::uint32_t rows,UploadFootprint& output) const
{
    if(!impl_||!impl_->Usable())return Fail(Error::InvalidState);
    return D3D12TextureUploadFootprint(impl_->context,description,rows,output)?Result{}:Fail(Error::InvalidArgument);
}
Result Device::UploadBufferRange(BufferHandle buffer,std::uint64_t offset,std::span<const std::byte> bytes)
{
    if(!impl_||!impl_->Usable()||!impl_->presentation.uploadStates)return Fail(Error::InvalidState);
    const auto entry=impl_->buffers.Get(buffer,impl_->id);if(!entry)return Fail(Error::InvalidHandle);
    if(!entry->staged||bytes.empty()||offset!=entry->written||bytes.size()>entry->bytes-entry->written||offset%entry->stride||bytes.size()%entry->stride)return Fail(Error::InvalidArgument);
    const bool vertex=entry->usage==BufferUsage::Vertex;UINT maximumIndex=entry->maxIndex;
    if(!ValidateBufferContents(vertex,bytes,entry->stride,maximumIndex))return Fail(Error::InvalidArgument);
    UploadFootprint footprint;if(!D3D12BufferUploadFootprint(bytes.size(),footprint))return Fail(Error::InvalidArgument);
    if(!RecordD3D12BufferRange(*entry->resource.Get(),impl_->context,impl_->presentation,offset,bytes,
        vertex?D3D12_RESOURCE_STATE_VERTEX_AND_CONSTANT_BUFFER:D3D12_RESOURCE_STATE_INDEX_BUFFER,offset+bytes.size()==entry->bytes,footprint,impl_->text))return Fail(Error::BackendFailure);
    entry->written+=bytes.size();entry->maxIndex=maximumIndex;entry->uploadFence=impl_->presentation.nextFenceValue;entry->lastUse=std::max(entry->lastUse,entry->uploadFence);
    ++impl_->stagedCopies;impl_->stagedPayloadBytes+=footprint.payloadBytes;impl_->stagedFootprintBytes+=footprint.stagingBytes;return {};
}
Result Device::UploadTextureRows(TextureHandle texture,std::uint32_t firstRow,std::uint32_t rows,std::span<const std::uint8_t> rgba8)
{
    if(!impl_||!impl_->Usable()||!impl_->presentation.uploadStates)return Fail(Error::InvalidState);
    const auto entry=impl_->textures.Get(texture,impl_->id);if(!entry)return Fail(Error::InvalidHandle);
    if(!entry->staged||!rows||firstRow!=entry->writtenRows||rows>entry->native.height-entry->writtenRows||rgba8.size()!=UINT64(entry->native.width)*rows*4)return Fail(Error::InvalidArgument);
    TextureDescription description{entry->native.width,entry->native.height};UploadFootprint footprint;
    if(!D3D12TextureUploadFootprint(impl_->context,description,rows,footprint))return Fail(Error::InvalidArgument);
    if(!RecordD3D12TextureRows(entry->native,impl_->context,impl_->presentation,firstRow,rows,rgba8,firstRow+rows==entry->native.height,footprint,impl_->text))return Fail(Error::BackendFailure);
    entry->writtenRows+=rows;entry->uploadFence=impl_->presentation.nextFenceValue;entry->lastUse=std::max(entry->lastUse,entry->uploadFence);
    ++impl_->stagedCopies;impl_->stagedPayloadBytes+=footprint.payloadBytes;impl_->stagedFootprintBytes+=footprint.stagingBytes;return {};
}
Result Device::CreatePipeline(const PipelineDescription& description,Pipeline& output)
{
    if(!impl_||!impl_->Usable())return Fail(Error::InvalidState);
    if(output||description.shader.empty())return Fail(Error::InvalidArgument);
    if(description.kind==PipelineKind::Gui&&(description.colorFormat!=ColorFormat::Rgba8Srgb||description.blend!=BlendMode::Alpha
        ||(description.depth!=DepthMode::Default&&description.depth!=DepthMode::Disabled)))return Fail(Error::Unsupported);
    // G15.09：输出 LUT 布局同时服务编辑器 BasicLit（ShadowOutput）与 Lighting 演示（FinalOutput）。
    if(!ValidPipelineKind(description.kind)||(description.bindingLayoutVersion!=BindingLayout(description.kind)
        &&!(IsOutput(description.kind)&&description.bindingLayoutVersion==OutputLutLayoutVersion))
        ||(description.depth!=DepthMode::Default&&description.depth!=DepthMode::Disabled&&description.depth!=DepthMode::ReadWriteLess&&description.depth!=DepthMode::ReadOnlyLessEqual)
        ||(description.blend!=BlendMode::Opaque&&description.blend!=BlendMode::Alpha)
        ||(description.colorFormat!=ColorFormat::Rgba8Unorm&&description.colorFormat!=ColorFormat::Rgba16Float&&description.colorFormat!=ColorFormat::Rgba8Srgb))return Fail(Error::Unsupported);
    if(IsPostProcess(description.kind))
    {
        const auto format=description.kind==PipelineKind::BloomExtract||description.kind==PipelineKind::BloomBlur?ColorFormat::Rgba16Float:description.kind==PipelineKind::ToneMap?ColorFormat::Rgba8Unorm:ColorFormat::Rgba8Srgb;
        if(description.colorFormat!=format||(description.depth!=DepthMode::Default&&description.depth!=DepthMode::Disabled)
            ||description.blend!=(description.kind==PipelineKind::UiOverlay?BlendMode::Alpha:BlendMode::Opaque))return Fail(Error::Unsupported);
    }
    if((UsesLitLayout(description.kind)||IsOutput(description.kind)||IsSky(description.kind)||description.kind==PipelineKind::ShadowDepth)&&description.blend!=BlendMode::Opaque)return Fail(Error::Unsupported);
    if(IsForward(description.kind)&&description.colorFormat!=ColorFormat::Rgba16Float)return Fail(Error::Unsupported);
    if(IsOutput(description.kind)&&(description.colorFormat!=ColorFormat::Rgba8Unorm
        ||(description.depth!=DepthMode::Default&&description.depth!=DepthMode::Disabled)))return Fail(Error::Unsupported);
    if(IsSky(description.kind)&&(description.colorFormat!=ColorFormat::Rgba16Float
        ||(description.depth!=DepthMode::Default&&description.depth!=DepthMode::Disabled)))return Fail(Error::Unsupported);
    if(description.kind==PipelineKind::ShadowDepth&&(description.colorFormat!=ColorFormat::Rgba8Unorm
        ||(description.depth!=DepthMode::Default&&description.depth!=DepthMode::ReadWriteLess)))return Fail(Error::Unsupported);
    auto entry=std::make_shared<PipelineEntry>();entry->kind=description.kind;
    bool success=false;
    if(IsOutput(description.kind)||IsSky(description.kind)||IsPostProcess(description.kind)||description.kind==PipelineKind::ShadowDepth||description.kind==PipelineKind::Gui)
    {
        entry->mesh.prepared=impl_->context.pipelines->Prepare(description,impl_->text);success=bool(entry->mesh.prepared);
        if(success){entry->mesh.rootSignature=entry->mesh.prepared->rootSignature;entry->mesh.pipeline=entry->mesh.prepared->pipeline;}
    }
    else if(description.kind==PipelineKind::TexturedMesh||UsesLitLayout(description.kind))success=InitializeD3D12MeshPipeline(entry->mesh,impl_->context,description,impl_->text);
    else if(description.kind==PipelineKind::Triangle)success=InitializeD3D12Triangle(entry->triangle,impl_->context,description,impl_->text);
    else return Fail(Error::Unsupported);
    if(!success)return Fail(Error::BackendFailure);
    const auto handle=impl_->pipelines.Add(std::move(entry),impl_->id);const auto weak=weak_from_this();
    output=Pipeline(new PipelineHandle(handle),[weak](const PipelineHandle* value){if(auto device=weak.lock())device->Destroy(*value);delete value;});return {};
}
Result Device::Destroy(BufferHandle handle)
{
    if(!impl_||!impl_->ready||impl_->owner!=std::this_thread::get_id())return Fail(Error::InvalidState);
    auto entry=impl_->buffers.Remove(handle,impl_->id);if(!entry)return Fail(Error::InvalidHandle);
    impl_->retiring.push_back(std::move(entry));return {};
}
Result Device::BeginUploadBatch()
{
    if(!impl_||!impl_->Usable()||impl_->presentation.uploadStates)return Fail(Error::InvalidState);
    return BeginD3D12UploadBatch(impl_->presentation,impl_->text)?Result{}:Fail(Error::BackendFailure);
}
Result Device::EndUploadBatch(CompletionToken& token)
{
    if(!impl_||!impl_->Usable()||!impl_->presentation.uploadStates)return Fail(Error::InvalidState);
    UINT64 fence=0;
    if(!EndD3D12UploadBatch(impl_->presentation,fence,impl_->text))return Fail(Error::BackendFailure);
    token={impl_->id,1,fence};impl_->Collect();return {};
}
Result Device::SetUploadMode(UploadMode mode)
{
    if(!impl_||!impl_->Usable()||impl_->presentation.uploadStates)return Fail(Error::InvalidState);
    if(mode!=UploadMode::Pooled&&mode!=UploadMode::DedicatedBaseline)return Fail(Error::InvalidArgument);
    if(!WaitIdle())return Fail(Error::BackendFailure);
    impl_->presentation.dedicatedUploads=mode==UploadMode::DedicatedBaseline;
    for(auto& frame:impl_->presentation.frames)
    {
        frame.drawConstants.clear();frame.instanceSlices.clear();frame.uploads.SetDedicated(mode==UploadMode::DedicatedBaseline);frame.baselineConstants.Release();
        if(mode==UploadMode::DedicatedBaseline)
        {
            frame.baselineConstants.Initialize(*impl_->context.device.Get(),impl_->context.states,4096);
            D3D12UploadSlice warm;const auto created=frame.baselineConstants.Allocate(4096,256,warm);frame.baselineConstants.Reset();
            if(FAILED(created)){impl_->presentation.failed=true;return Fail(Error::BackendFailure);}
        }
    }
    impl_->Collect();return {};
}
Result Device::Destroy(TextureHandle handle)
{
    if(!impl_||!impl_->ready||impl_->owner!=std::this_thread::get_id())return Fail(Error::InvalidState);
    auto entry=impl_->textures.Remove(handle,impl_->id);if(!entry)return Fail(Error::InvalidHandle);
    impl_->retiring.push_back(std::move(entry));return {};
}
Result Device::ConfigureDescriptors(DescriptorLimits limits)
{
    if(!impl_||!impl_->Usable()||impl_->presentation.uploadStates||impl_->textures.Count())return Fail(Error::InvalidState);
    if(!limits.persistentPage||limits.persistentPage>limits.persistentMaximum||limits.persistentMaximum>65536
        ||!limits.frameInitial||limits.frameInitial>limits.frameMaximum||limits.frameMaximum>65536)return Fail(Error::InvalidArgument);
    if(!WaitIdle())return Fail(Error::BackendFailure);
    if(impl_->context.srvDescriptors->Stats().used)return Fail(Error::InvalidState);
    if(!impl_->context.srvDescriptors->Configure(limits.persistentPage,limits.persistentMaximum))return Fail(Error::InvalidState);
    for(auto& frame:impl_->presentation.frames){frame.descriptors.Configure(limits.frameInitial,limits.frameMaximum);frame.guiDescriptors.Configure(limits.frameInitial,limits.frameMaximum);frame.gameUiDescriptors.Configure(limits.frameInitial,limits.frameMaximum);}
    return {};
}
Result Device::TrimMemory()
{
    if(!impl_||!impl_->Usable()||impl_->presentation.uploadStates)return Fail(Error::InvalidState);
    if(!WaitIdle())return Fail(Error::BackendFailure);impl_->context.memory->Trim();return {};
}
Result Device::Destroy(PipelineHandle handle)
{
    if(!impl_||!impl_->ready||impl_->owner!=std::this_thread::get_id())return Fail(Error::InvalidState);
    auto entry=impl_->pipelines.Remove(handle,impl_->id);if(!entry)return Fail(Error::InvalidHandle);
    impl_->retiring.push_back(std::move(entry));return {};
}
CommandContext::CommandContext(std::unique_ptr<Impl> impl):impl_(std::move(impl)){}
CommandContext::~CommandContext()=default;
CommandContext::CommandContext(CommandContext&&) noexcept=default;
CommandContext& CommandContext::operator=(CommandContext&&) noexcept=default;
std::unique_ptr<CommandContext> Device::BeginCommands(std::uint64_t sourceFrame)
{
    if(!impl_||!impl_->Usable()||impl_->presentation.uploadStates)return {};
    impl_->Collect();auto context=std::make_unique<CommandContext::Impl>();context->device=impl_;
    context->sourceFrame=sourceFrame;
    return std::unique_ptr<CommandContext>(new CommandContext(std::move(context)));
}
namespace
{
Result ResolveGuiGeometry(auto& d,const GuiGeometry& g,std::span<const TextureHandle> textures,UINT maximumCommands,
    const TextureEntry* forbidden,bool allowTargets,std::vector<std::shared_ptr<TextureEntry>>& resolved)
{
    if(!IsFinite(g.displayPosition.x)||!IsFinite(g.displayPosition.y)||!IsFinite(g.displaySize.x)||!IsFinite(g.displaySize.y)
        ||!IsFinite(g.framebufferScale.x)||!IsFinite(g.framebufferScale.y)||g.displaySize.x<=0||g.displaySize.y<=0||g.framebufferScale.x<=0||g.framebufferScale.y<=0
        ||g.displaySize.x*g.framebufferScale.x>8192||g.displaySize.y*g.framebufferScale.y>8192||textures.size()!=g.commands.size())return Fail(Error::InvalidArgument);
    if(g.vertices.size()>2*1024*1024||g.indices.size()>4*1024*1024||g.commands.size()>maximumCommands)return Fail(Error::CapacityExceeded);
    const Vec4 transform{2/g.displaySize.x,-2/g.displaySize.y,-1-g.displayPosition.x*2/g.displaySize.x,1+g.displayPosition.y*2/g.displaySize.y};
    if(!IsFinite(transform))return Fail(Error::InvalidArgument);
    for(const auto& v:g.vertices)if(!IsFinite(v.position.x)||!IsFinite(v.position.y)||!IsFinite(v.uv.x)||!IsFinite(v.uv.y)
        ||!IsFinite(v.position.x*transform.x+transform.z)||!IsFinite(v.position.y*transform.y+transform.w))return Fail(Error::InvalidArgument);
    resolved.reserve(textures.size());
    for(size_t i=0;i<g.commands.size();++i)
    {
        const auto& command=g.commands[i];auto texture=d.textures.Get(textures[i],d.id);
        if(!texture)return Fail(Error::InvalidHandle);
        if(!d.Ready(*texture))return Fail(Error::InvalidState);
        if(texture.get()==forbidden||(!allowTargets&&texture->target)||texture->native.cube||texture->native.arrayLayers!=1||texture->native.format!=TextureFormat::Rgba8)return Fail(Error::Unsupported);
        if(!IsFinite(command.clip)||command.clip.z<command.clip.x||command.clip.w<command.clip.y||command.indexCount%3
            ||command.indexOffset>g.indices.size()||command.indexCount>g.indices.size()-command.indexOffset||command.vertexOffset>INT_MAX)return Fail(Error::InvalidArgument);
        for(size_t j=command.indexOffset;j<size_t(command.indexOffset)+command.indexCount;++j)
            if(std::uint64_t(g.indices[j])+command.vertexOffset>=g.vertices.size())return Fail(Error::InvalidArgument);
        resolved.push_back(std::move(texture));
    }
    return {};
}
}
Result CommandContext::ConfigureGui(TextureHandle sceneTarget,TextureHandle composite,PipelineHandle pipeline,
    const GuiGeometry& g,std::span<const TextureHandle> textures)
{
    auto d=impl_?impl_->device.lock():nullptr;
    if(!d||!d->Usable()||impl_->phase||impl_->forward||impl_->guiScene)return Fail(Error::InvalidState);
    auto scene=d->textures.Get(sceneTarget,d->id),output=d->textures.Get(composite,d->id);auto program=d->pipelines.Get(pipeline,d->id);
    if(!scene||!output||!program)return Fail(Error::InvalidHandle);
    if(scene==output||!scene->target||!scene->target->depth||scene->target->srgb||!output->target||!output->target->srgb||output->target->depth||program->kind!=PipelineKind::Gui)return Fail(Error::Unsupported);
    std::vector<std::shared_ptr<TextureEntry>> resolved;
    if(auto result=ResolveGuiGeometry(*d,g,textures,d->presentation.frames[0].guiDescriptors.Maximum(),output.get(),true,resolved);!result)return result;
    // 150%/200% 等分数缩放会产生亚像素差异；按 1px 容差比较，避免合法 DPI 被误判。
    if(std::abs(g.displaySize.x*g.framebufferScale.x-output->native.width)>1.0f||std::abs(g.displaySize.y*g.framebufferScale.y-output->native.height)>1.0f)return Fail(Error::InvalidArgument);
    impl_->guiGeometry=g;impl_->guiScene=scene;impl_->guiComposite=output;impl_->guiPipeline=program;impl_->guiTextures=std::move(resolved);
    impl_->leases.insert(impl_->leases.end(),{scene,output,program});
    for(const auto& texture:impl_->guiTextures)impl_->leases.push_back(texture);
    return {};
}
Result CommandContext::ConfigureGameUi(PipelineHandle pipeline,const GuiGeometry& g,std::span<const TextureHandle> textures)
{
    auto d=impl_?impl_->device.lock():nullptr;
    if(!d||!d->Usable()||impl_->phase||impl_->gameUiPipeline)return Fail(Error::InvalidState);
    auto program=d->pipelines.Get(pipeline,d->id);if(!program)return Fail(Error::InvalidHandle);
    if(program->kind!=PipelineKind::Gui)return Fail(Error::Unsupported);
    if(g.vertices.size()>65536*4||g.indices.size()>65536*6)return Fail(Error::CapacityExceeded);
    std::vector<std::shared_ptr<TextureEntry>> resolved;
    if(auto result=ResolveGuiGeometry(*d,g,textures,d->presentation.frames[0].gameUiDescriptors.Maximum(),nullptr,false,resolved);!result)return result;
    impl_->gameUiGeometry=g;impl_->gameUiPipeline=program;impl_->gameUiTextures=std::move(resolved);
    impl_->leases.push_back(program);for(const auto& texture:impl_->gameUiTextures)impl_->leases.push_back(texture);
    return {};
}
Result CommandContext::ConfigureComposition(const FrameComposition& composition)
{
    auto d=impl_?impl_->device.lock():nullptr;
    if(!d||!d->Usable()||impl_->phase||impl_->forward||impl_->guiScene||impl_->composition)return Fail(Error::InvalidState);
    if(!composition.scene.width||!composition.scene.height||composition.scene.width>8192||composition.scene.height>8192
        ||composition.scene.hdrColor==composition.scene.ldrColor)return Fail(Error::InvalidArgument);
    const auto hdr=d->textures.Get(composition.scene.hdrColor,d->id);
    const auto ldr=d->textures.Get(composition.scene.ldrColor,d->id);
    if(!hdr||!ldr)return Fail(Error::InvalidHandle);
    if(!hdr->target||!ldr->target||hdr->native.format!=TextureFormat::Rgba16Float||ldr->native.format!=TextureFormat::Rgba8
        ||!ldr->target->depth||hdr->native.width!=composition.scene.width||hdr->native.height!=composition.scene.height
        ||ldr->native.width!=composition.scene.width||ldr->native.height!=composition.scene.height)
        return Fail(Error::Unsupported);
    impl_->composition=composition;impl_->leases.insert(impl_->leases.end(),{hdr,ldr});return {};
}
Result CommandContext::ConfigureForward(const ForwardLighting& lighting,PipelineHandle outputPipeline,PipelineHandle skyPipeline)
{
    auto device=impl_?impl_->device.lock():nullptr;if(!device||!device->Usable()||impl_->phase||impl_->forward||(impl_->guiScene&&!impl_->composition))return Fail(Error::InvalidState);
    const auto pipeline=device->pipelines.Get(outputPipeline,device->id);if(!pipeline)return Fail(Error::InvalidHandle);
    if(!IsOutput(pipeline->kind))return Fail(Error::Unsupported);
    const auto sky=device->pipelines.Get(skyPipeline,device->id);if(!sky)return Fail(Error::InvalidHandle);
    if(!IsSky(sky->kind))return Fail(Error::Unsupported);
    const auto radiance=[](Vec3 value){return IsFinite(value)&&value.x>=0&&value.y>=0&&value.z>=0&&value.x<=10000&&value.y<=10000&&value.z<=10000;};
    const auto length=LengthSquared(lighting.directionToLight);
    const auto& skySettings=lighting.skySettings;
    if(!radiance(skySettings.zenithColor)||!radiance(skySettings.horizonColor)||!radiance(skySettings.groundColor)||!radiance(skySettings.sunColor)
        ||!IsFinite(skySettings.sunIntensity)||skySettings.sunIntensity<0||skySettings.sunIntensity>1000
        ||!IsFinite(skySettings.sunAngularRadius)||skySettings.sunAngularRadius<0.001f||skySettings.sunAngularRadius>0.1f)
        return Fail(Error::InvalidArgument);
    if(lighting.pointCount>MaximumPointLights||!IsFinite(lighting.exposure)||lighting.exposure<1.0f/1024||lighting.exposure>64
        ||!radiance(lighting.sky)||!radiance(lighting.directionalRadiance)||!IsFinite(lighting.directionToLight)||!IsFinite(length)||length<1e-12f)return Fail(Error::InvalidArgument);
    for(UINT index=0;index<lighting.pointCount;++index)
    {
        const auto& point=lighting.points[index];
        if(!IsFinite(point.position)||std::abs(point.position.x)>1e6||std::abs(point.position.y)>1e6||std::abs(point.position.z)>1e6
            ||!radiance(point.radiance)||!IsFinite(point.range)||point.range<0.05f||point.range>10000)return Fail(Error::InvalidArgument);
    }
    impl_->forward=lighting;impl_->outputPipeline=pipeline;impl_->skyPipeline=sky;impl_->leases.insert(impl_->leases.end(),{pipeline,sky});return {};
}
Result CommandContext::ConfigureOcclusion(std::span<const se::VisibilityItem> bounds,const se::VisibilityFrustum& frustum,
    const OcclusionSettings& settings)
{
    const OcclusionBatch batch{bounds};
    return ConfigureOcclusion(std::span<const OcclusionBatch>(&batch,1),frustum,settings);
}
Result CommandContext::ConfigureOcclusion(std::span<const OcclusionBatch> batches,const se::VisibilityFrustum& frustum,
    const OcclusionSettings& settings)
{
    auto device=impl_?impl_->device.lock():nullptr;
    if(!device||!device->Usable()||!impl_||impl_->occlusion.active)return Fail(Error::InvalidState);
    // 该批次接管整帧的实例化绘制；阴影/后处理/编辑器 GUI/合成不复用同一帧。
    // G06.10：Forward 实例化允许同帧；GameUI 是场景后的覆盖层，也允许共存（运行时 HUD）。
    if(impl_->shadow||impl_->post||impl_->guiScene||impl_->guiPipeline
        ||impl_->composition||!impl_->draws.empty()||impl_->pipeline)return Fail(Error::Unsupported);
    if(impl_->phase!=0&&impl_->phase!=2)return Fail(Error::InvalidState);
    if(batches.empty()||batches.size()>D3D12MeshMaxDraws||!se::ValidVisibilityFrustum(frustum)
        ||!IsFinite(settings.depthBias)||settings.depthBias<0||settings.depthBias>0.1f)return Fail(Error::InvalidArgument);
    impl_->occlusion.bounds.clear();impl_->occlusion.bounds.reserve(batches.size());
    for(const auto& batch:batches)
    {
        if(batch.bounds.empty()||batch.bounds.size()>se::MaximumVisibilityItems)return Fail(Error::InvalidArgument);
        for(std::size_t index=0;index<batch.bounds.size();++index)
        {
            const auto& item=batch.bounds[index];
            if(item.id!=index||!IsFinite(item.minimum)||!IsFinite(item.maximum)
                ||item.minimum.x>item.maximum.x||item.minimum.y>item.maximum.y||item.minimum.z>item.maximum.z)
                return Fail(Error::InvalidArgument);
        }
        impl_->occlusion.bounds.emplace_back(batch.bounds.begin(),batch.bounds.end());
    }
    impl_->occlusion.active=true;
    impl_->occlusion.frustum=frustum;impl_->occlusion.settings=settings;return {};
}
Result CommandContext::ConfigureShadows(const ShadowSettings& shadows,PipelineHandle depthPipeline)
{
    auto device=impl_?impl_->device.lock():nullptr;
    if(!device||!device->Usable()||impl_->phase||!impl_->forward||impl_->shadow)return Fail(Error::InvalidState);
    if(!impl_->outputPipeline||impl_->outputPipeline->kind!=PipelineKind::ShadowOutput)return Fail(Error::Unsupported);
    const auto pipeline=device->pipelines.Get(depthPipeline,device->id);if(!pipeline)return Fail(Error::InvalidHandle);
    if(pipeline->kind!=PipelineKind::ShadowDepth)return Fail(Error::Unsupported);
    if(!IsFinite(shadows.clipFromWorld)||!Inverse(shadows.clipFromWorld)
        ||(shadows.resolution!=256&&shadows.resolution!=512&&shadows.resolution!=1024&&shadows.resolution!=2048&&shadows.resolution!=4096)
        ||!IsFinite(shadows.depthBias)||shadows.depthBias<0||shadows.depthBias>0.02f||!IsFinite(shadows.slopeBias)||shadows.slopeBias<0||shadows.slopeBias>0.05f
        ||!IsFinite(shadows.normalBias)||shadows.normalBias<0||shadows.normalBias>0.25f)return Fail(Error::InvalidArgument);
    impl_->shadow=shadows;impl_->shadowPipeline=pipeline;impl_->leases.push_back(pipeline);return {};
}
Result CommandContext::ConfigureEnvironment(const EnvironmentBindings& environment)
{
    auto device=impl_?impl_->device.lock():nullptr;
    if(!device||!device->Usable()||impl_->phase||!impl_->shadow||impl_->environment)return Fail(Error::InvalidState);
    if(!impl_->skyPipeline||impl_->skyPipeline->kind!=PipelineKind::EnvironmentSky)return Fail(Error::Unsupported);
    if(!IsFinite(environment.intensity)||environment.intensity<0||environment.intensity>64||!IsFinite(environment.yaw)||!IsFinite(environment.skyFromClip)||!Inverse(environment.skyFromClip))return Fail(Error::InvalidArgument);
    for(float value:environment.skyFromClip.values)if(std::abs(value)>1e6f)return Fail(Error::InvalidArgument);
    const TextureHandle handles[]{environment.irradiance,environment.specular,environment.brdfLut};
    std::array<std::shared_ptr<TextureEntry>,3> textures;
    for(size_t i=0;i<textures.size();++i)
    {
        textures[i]=device->textures.Get(handles[i],device->id);if(!textures[i])return Fail(Error::InvalidHandle);
        const auto& texture=textures[i]->native;
        if(texture.format!=TextureFormat::Rgba16Float||texture.cube!=(i<2)||texture.arrayLayers!=(i<2?6u:1u))return Fail(Error::Unsupported);
    }
    impl_->environment=environment;impl_->environmentTextures=textures;
    for(const auto& texture:textures)impl_->leases.push_back(texture);return {};
}
Result CommandContext::ConfigurePostProcess(const PostProcessSettings& settings,const PostProcessPipelines& pipelines)
{
    auto device=impl_?impl_->device.lock():nullptr;
    if(!device||!device->Usable()||impl_->phase||!impl_->forward||impl_->post)return Fail(Error::InvalidState);
    if(!IsFinite(settings.bloomThreshold)||settings.bloomThreshold<0||settings.bloomThreshold>65504
        ||!IsFinite(settings.bloomStrength)||settings.bloomStrength<0||settings.bloomStrength>4
        ||!IsFinite(settings.exposureDeltaSeconds)||settings.exposureDeltaSeconds<0||settings.exposureDeltaSeconds>0.5
        ||!IsFinite(settings.exposureLowPercentile)||settings.exposureLowPercentile<0||settings.exposureLowPercentile>1
        ||!IsFinite(settings.exposureHighPercentile)||settings.exposureHighPercentile<0||settings.exposureHighPercentile>1
        ||!IsFinite(settings.exposureBrightAdaptation)||settings.exposureBrightAdaptation<0||settings.exposureBrightAdaptation>256
        ||!IsFinite(settings.exposureDarkAdaptation)||settings.exposureDarkAdaptation<0||settings.exposureDarkAdaptation>256
        ||!IsFinite(settings.exposureMinimum)||settings.exposureMinimum<=0||settings.exposureMinimum>1e6
        ||!IsFinite(settings.exposureMaximum)||settings.exposureMaximum<settings.exposureMinimum||settings.exposureMaximum>1e6
        ||!IsFinite(settings.exposureKey)||settings.exposureKey<=0||settings.exposureKey>1e6)return Fail(Error::InvalidArgument);
    for(size_t index=0;index<3;++index)
        if(!IsFinite(settings.lutLift[index])||std::abs(settings.lutLift[index])>1e4
            ||!IsFinite(settings.lutGamma[index])||settings.lutGamma[index]<1e-4f||settings.lutGamma[index]>1e4f
            ||!IsFinite(settings.lutGain[index])||std::abs(settings.lutGain[index])>1e4)return Fail(Error::InvalidArgument);
    // G15：`.selut` 资产必须恰好是 33³ 显示线性晶格，值域与 16F 一致；不静默截断。
    if(settings.lutAsset)
    {
        if(settings.lutAsset->size()!=std::size_t{D3D12Exposure::lutSize}*D3D12Exposure::lutSize*D3D12Exposure::lutSize)
            return Fail(Error::InvalidArgument);
        for(const auto& value:*settings.lutAsset)
            if(!IsFinite(value.x)||!IsFinite(value.y)||!IsFinite(value.z)
                ||value.x<0||value.x>65504||value.y<0||value.y>65504||value.z<0||value.z>65504)
                return Fail(Error::InvalidArgument);
    }
    if(settings.taa&&(!IsFinite(settings.taaCurrentClipFromWorld)||!IsFinite(settings.taaPreviousClipFromWorld)
        ||!IsFinite(settings.taaStaticBlend)||settings.taaStaticBlend<0||settings.taaStaticBlend>1
        ||!IsFinite(settings.taaDynamicBlend)||settings.taaDynamicBlend<0||settings.taaDynamicBlend>1
        ||!IsFinite(settings.taaDepthThreshold)||settings.taaDepthThreshold<=0||settings.taaDepthThreshold>1
        ||!IsFinite(settings.taaVelocityThreshold)||settings.taaVelocityThreshold<=0||settings.taaVelocityThreshold>1
        ||!IsFinite(settings.taaSharpen)||settings.taaSharpen<0||settings.taaSharpen>2))return Fail(Error::InvalidArgument);
    const PipelineHandle handles[]{pipelines.extract,pipelines.blur,pipelines.toneMap,pipelines.fxaa,pipelines.overlay};
    const PipelineKind kinds[]{PipelineKind::BloomExtract,PipelineKind::BloomBlur,PipelineKind::ToneMap,PipelineKind::Fxaa,PipelineKind::UiOverlay};
    std::array<std::shared_ptr<PipelineEntry>,5> entries;
    for(size_t index=0;index<entries.size();++index)
    {entries[index]=device->pipelines.Get(handles[index],device->id);if(!entries[index])return Fail(Error::InvalidHandle);if(entries[index]->kind!=kinds[index])return Fail(Error::Unsupported);}
    impl_->post=settings;impl_->postPipelines=entries;for(const auto& entry:entries)impl_->leases.push_back(entry);return {};
}
Result CommandContext::DrawOverlay(const OverlayQuad& quad)
{
    auto device=impl_?impl_->device.lock():nullptr;
    if(!device||!device->Usable()||!impl_->post||impl_->phase!=23||impl_->kind!=PassKind::Ui)return Fail(Error::InvalidState);
    if(!IsFinite(quad.rectangle)||!IsFinite(quad.color)||!IsFinite(quad.clip)||quad.rectangle.z<0||quad.rectangle.w<0||quad.clip.z<0||quad.clip.w<0)return Fail(Error::InvalidArgument);
    for(const auto value:{quad.rectangle.x,quad.rectangle.y,quad.rectangle.z,quad.rectangle.w,quad.clip.x,quad.clip.y,quad.clip.z,quad.clip.w})if(std::abs(value)>1e6)return Fail(Error::InvalidArgument);
    for(const auto value:{quad.color.x,quad.color.y,quad.color.z,quad.color.w})if(value<0||value>1)return Fail(Error::InvalidArgument);
    if(impl_->overlays.size()>=65536)return Fail(Error::CapacityExceeded);impl_->overlays.push_back(quad);return {};
}
Result CommandContext::DrawShadow(BufferHandle vertices,BufferHandle indices,const Mat4& world)
{
    auto device=impl_?impl_->device.lock():nullptr;
    if(!device||!device->Usable()||impl_->phase!=11||!impl_->shadow||!impl_->shadowPipeline)return Fail(Error::InvalidState);
    const auto vertex=device->buffers.Get(vertices,device->id),index=device->buffers.Get(indices,device->id);
    if(!vertex||!index)return Fail(Error::InvalidHandle);
    if(!device->Ready(*vertex)||!device->Ready(*index))return Fail(Error::InvalidState);
    if(vertex->usage!=BufferUsage::Vertex||index->usage!=BufferUsage::Index||index->maxIndex>=vertex->count||impl_->shadowGeometry.size()>=D3D12MeshMaxDraws
        ||!IsFinite(world)||!Inverse(world)||!IsFinite(impl_->shadow->clipFromWorld*world)
        ||std::abs(world(3,0))>1e-5f||std::abs(world(3,1))>1e-5f||std::abs(world(3,2))>1e-5f||std::abs(world(3,3)-1)>1e-5f)return Fail(Error::InvalidArgument);
    D3D12Mesh mesh;mesh.vertices=vertex->resource;mesh.indices=index->resource;
    mesh.vertexView={vertex->resource->GetGPUVirtualAddress(),vertex->bytes,vertex->stride};
    mesh.indexView={index->resource->GetGPUVirtualAddress(),index->bytes,index->stride==2?DXGI_FORMAT_R16_UINT:DXGI_FORMAT_R32_UINT};mesh.indexCount=index->count;
    impl_->shadowGeometry.push_back(std::move(mesh));impl_->shadowWorlds.push_back(world);impl_->leases.insert(impl_->leases.end(),{vertex,index});return {};
}
Result CommandContext::BeginPass(const PassDescription& pass)
{
    auto device=impl_?impl_->device.lock():nullptr;if(!device||!device->Usable())return Fail(Error::InvalidState);
    if(pass.cacheBindings&&pass.kind!=PassKind::Opaque)return Fail(Error::Unsupported);
    if(pass.kind>=PassKind::BloomExtract&&pass.kind<=PassKind::Ui)
    {
        if(!impl_->post)return Fail(Error::InvalidState);
        if(pass.color!=ResourceUse::ColorWrite||pass.depth!=ResourceUse::None||pass.input!=(pass.kind==PassKind::Ui?ResourceUse::None:ResourceUse::ColorRead))return Fail(Error::Unsupported);
        const int expected=pass.kind==PassKind::BloomExtract?4:pass.kind==PassKind::BloomHorizontal?14:pass.kind==PassKind::BloomVertical?16:
            pass.kind==PassKind::ToneMap?(impl_->post->bloom?18:4):pass.kind==PassKind::Fxaa?20:22;
        if(impl_->phase!=expected||(pass.kind<=PassKind::BloomVertical&&!impl_->post->bloom))return Fail(Error::InvalidState);
        impl_->phase=13+2*(int(pass.kind)-int(PassKind::BloomExtract));impl_->kind=pass.kind;return {};
    }
    if(pass.kind==PassKind::Shadow)
    {
        if(!impl_->shadow||impl_->phase!=2)return Fail(Error::InvalidState);
        if(pass.color!=ResourceUse::None||pass.depth!=ResourceUse::DepthWrite||pass.input!=ResourceUse::None)return Fail(Error::Unsupported);
        impl_->phase=11;impl_->kind=pass.kind;return {};
    }
    if(pass.color!=ResourceUse::ColorWrite)return Fail(Error::Unsupported);
    if(impl_->forward)
    {
        const bool overlay=pass.kind==PassKind::Sky||pass.kind==PassKind::Output;
        const auto input=pass.kind==PassKind::Output?ResourceUse::ColorRead:pass.kind==PassKind::Opaque&&(impl_->shadow||impl_->occlusion.active)?ResourceUse::DepthRead:ResourceUse::None;
        if(pass.depth!=(overlay?ResourceUse::None:ResourceUse::DepthWrite)||pass.input!=input)return Fail(Error::Unsupported);
        if(pass.kind==PassKind::Clear&&impl_->phase==0)impl_->phase=1;
        else if(pass.kind==PassKind::Sky&&impl_->phase==(impl_->shadow?12:2))impl_->phase=7;
        else if(pass.kind==PassKind::Opaque&&impl_->phase==8)impl_->phase=3;
        else if(pass.kind==PassKind::Output&&impl_->phase==(impl_->post?24:4))impl_->phase=9;
        else return Fail(Error::InvalidState);
        if(pass.kind==PassKind::Opaque)impl_->cacheOpaqueBindings=pass.cacheBindings;
        impl_->kind=pass.kind;return {};
    }
    if(pass.depth!=ResourceUse::DepthWrite||pass.input!=ResourceUse::None)return Fail(Error::Unsupported);
    if(pass.kind==PassKind::Clear&&impl_->phase==0)impl_->phase=1;
    else if((pass.kind==PassKind::Opaque||pass.kind==PassKind::Triangle)&&impl_->phase==2)impl_->phase=3;
    else return Fail(Error::InvalidState);
    if(pass.kind==PassKind::Opaque)impl_->cacheOpaqueBindings=pass.cacheBindings;
    impl_->kind=pass.kind;return {};
}
Result CommandContext::Draw(const DrawIndexed& draw)
{return DrawInstanced(draw,{});}
Result CommandContext::DrawInstanced(const DrawIndexed& draw,std::span<const InstanceParameters> instances)
{
    auto device=impl_?impl_->device.lock():nullptr;
    if(!device||!device->Usable()||impl_->phase!=3||impl_->kind!=PassKind::Opaque)return Fail(Error::InvalidState);
    const auto vertex=device->buffers.Get(draw.vertices,device->id),index=device->buffers.Get(draw.indices,device->id);
    const auto texture=device->textures.Get(draw.texture,device->id);const auto pipeline=device->pipelines.Get(draw.pipeline,device->id);
    if(!vertex||!index||!texture||!pipeline)return Fail(Error::InvalidHandle);
    if(!device->Ready(*vertex)||!device->Ready(*index)||!device->Ready(*texture))return Fail(Error::InvalidState);
    if(IsInstanced(pipeline->kind)!=!instances.empty())return Fail(Error::Unsupported);
    if(instances.size()>MaximumFrameInstances||impl_->instanceCount>MaximumFrameInstances-instances.size())return Fail(Error::CapacityExceeded);
    const auto colorTexture=[](const D3D12Texture& value){return !value.cube&&value.arrayLayers==1
        &&(value.format==TextureFormat::Rgba8||value.format==TextureFormat::Bc1Unorm||value.format==TextureFormat::Bc1UnormSrgb);};
    // BC5 只有两个通道，只用于法线/单通道数据，不能作为 baseColor。
    const auto dataTexture=[](const D3D12Texture& value){return !value.cube&&value.arrayLayers==1
        &&(value.format==TextureFormat::Rgba8||value.format==TextureFormat::Bc1Unorm||value.format==TextureFormat::Bc1UnormSrgb
            ||value.format==TextureFormat::Bc5Unorm||value.format==TextureFormat::Bc4Unorm);};
    if(!colorTexture(texture->native)||texture->target)return Fail(Error::Unsupported);
    const bool forward=IsForward(pipeline->kind),shadow=IsShadowed(pipeline->kind),environment=pipeline->kind==PipelineKind::EnvironmentMesh;
    if(forward!=impl_->forward.has_value()||shadow!=impl_->shadow.has_value()||environment!=impl_->environment.has_value()||IsOutput(pipeline->kind)||pipeline->kind==PipelineKind::Triangle||pipeline->kind==PipelineKind::ShadowDepth||IsSky(pipeline->kind))return Fail(Error::Unsupported);
    if(pipeline->mesh.prepared->colorFormat!=(forward?ColorFormat::Rgba16Float:ColorFormat::Rgba8Unorm))return Fail(Error::Unsupported);
    const bool pbr=UsesLitLayout(pipeline->kind);
    if(vertex->usage!=BufferUsage::Vertex||index->usage!=BufferUsage::Index||(pipeline->kind!=PipelineKind::TexturedMesh&&!pbr)
        ||index->maxIndex>=vertex->count||impl_->draws.size()>=D3D12MeshMaxDraws)return Fail(Error::InvalidArgument);
    if(impl_->pipeline&&(impl_->viewProjection.values!=draw.viewProjection.values
        ||se::rhi::IsForward(impl_->pipeline->kind)!=forward||se::rhi::IsShadowed(impl_->pipeline->kind)!=shadow
        ||(impl_->pipeline->kind==PipelineKind::EnvironmentMesh)!=environment||se::rhi::IsInstanced(impl_->pipeline->kind)!=IsInstanced(pipeline->kind)))
        return Fail(Error::Unsupported);
    D3D12Mesh mesh;mesh.vertices=vertex->resource;mesh.indices=index->resource;
    mesh.vertexView={vertex->resource->GetGPUVirtualAddress(),vertex->bytes,vertex->stride};
    mesh.indexView={index->resource->GetGPUVirtualAddress(),index->bytes,index->stride==2?DXGI_FORMAT_R16_UINT:DXGI_FORMAT_R32_UINT};mesh.indexCount=index->count;
    mesh.texture=texture->native.resource;mesh.textureViews=texture->native.views;
    mesh.textureWidth=texture->native.width;mesh.textureHeight=texture->native.height;
    std::shared_ptr<TextureEntry> normal,mr;
    if(pbr)
    {
        normal=device->textures.Get(draw.normalTexture,device->id);mr=device->textures.Get(draw.metallicRoughnessTexture,device->id);
        if(!normal||!mr)return Fail(Error::InvalidHandle);
        if(!device->Ready(*normal)||!device->Ready(*mr))return Fail(Error::InvalidState);
        if(!dataTexture(normal->native)||!dataTexture(mr->native))return Fail(Error::Unsupported);
        mesh.pbrTextures={normal->native,mr->native};
    }
    const D3D12MeshDraw native{&mesh,draw.world,draw.tint,draw.textureIsData,draw.unlit,draw.pbr,instances,nullptr,draw.impostorView,draw.impostorNext,draw.impostorBlend,
        draw.hasPreviousWorld,draw.previousWorld,pipeline->mesh.prepared->program->source.compressedVertex};
    if(!ValidateD3D12MeshDraws(pipeline->mesh,std::span(&native,1),draw.viewProjection,device->text))return Fail(Error::InvalidArgument);
    auto source=std::to_string(texture->native.views->Cpu(draw.textureIsData?1:0).ptr)+":";
    if(pbr)source+=std::to_string(normal->native.views->Cpu(1).ptr)+":"+std::to_string(mr->native.views->Cpu(1).ptr)+":";
    if(shadow)source+="Shadow:";
    if(environment)source+="Environment:";
    if(!impl_->descriptors.contains(source)&&(impl_->descriptors.size()+1)*(environment?7:shadow?4:pbr?3:1)>device->presentation.frames[0].descriptors.Maximum())
    {++device->descriptorRejections;return Fail(Error::CapacityExceeded);}
    impl_->descriptors.insert(source);
    impl_->geometry.push_back(std::move(mesh));impl_->draws.push_back(draw);
    impl_->instances.emplace_back(instances.begin(),instances.end());impl_->instanceCount+=instances.size();
    impl_->leases.insert(impl_->leases.end(),{vertex,index,texture,pipeline});
    if(pbr)impl_->leases.insert(impl_->leases.end(),{normal,mr});
    impl_->drawPipelines.push_back(pipeline);
    if(!impl_->pipeline){impl_->pipeline=pipeline;impl_->pipelineHandle=draw.pipeline;impl_->viewProjection=draw.viewProjection;}
    return {};
}
Result CommandContext::Triangle(PipelineHandle handle,const Mat4& viewProjection)
{
    auto device=impl_?impl_->device.lock():nullptr;
    if(!device||!device->Usable()||impl_->phase!=3||impl_->kind!=PassKind::Triangle||impl_->pipeline)return Fail(Error::InvalidState);
    const auto pipeline=device->pipelines.Get(handle,device->id);if(!pipeline)return Fail(Error::InvalidHandle);
    if(pipeline->kind!=PipelineKind::Triangle||!IsFinite(viewProjection))return Fail(Error::InvalidArgument);
    if(pipeline->triangle.prepared->colorFormat!=ColorFormat::Rgba8Unorm||pipeline->triangle.prepared->depth!=DepthMode::Disabled)return Fail(Error::Unsupported);
    impl_->pipeline=pipeline;impl_->pipelineHandle=handle;impl_->viewProjection=viewProjection;impl_->leases.push_back(pipeline);return {};
}
Result CommandContext::EndPass()
{
    auto device=impl_?impl_->device.lock():nullptr;if(!device||!device->Usable())return Fail(Error::InvalidState);
    if(impl_->phase==1)impl_->phase=2;else if(impl_->phase==3)impl_->phase=4;
    else if(impl_->forward&&impl_->phase==7)impl_->phase=8;else if(impl_->forward&&impl_->phase==9)impl_->phase=10;
    else if(impl_->shadow&&impl_->phase==11)impl_->phase=12;
    else if(impl_->post&&impl_->phase>=13&&impl_->phase<=23&&impl_->phase%2)++impl_->phase;
    else return Fail(Error::InvalidState);return {};
}
Result CommandContext::Close()
{
    auto device=impl_?impl_->device.lock():nullptr;
    if(!device||!device->Usable()||(impl_->forward?impl_->phase!=10:impl_->phase!=2&&impl_->phase!=4)
        ||(impl_->skyPipeline&&impl_->environment.has_value()!=(impl_->skyPipeline->kind==PipelineKind::EnvironmentSky))
        ||(impl_->outputPipeline&&impl_->shadow.has_value()!=(impl_->outputPipeline->kind==PipelineKind::ShadowOutput)))return Fail(Error::InvalidState);
    impl_->phase=5;return {};
}
Result Device::Resize(std::uint32_t width,std::uint32_t height)
{
    if(!impl_||!impl_->Usable()||impl_->presentation.uploadStates)return Fail(Error::InvalidState);
    if(!ResizeD3D12Presentation(impl_->presentation,width,height,impl_->text))return Fail(Error::BackendFailure);
    // 呈现 Fence 随 resize 重建；旧槽的等待值不再可比较，丢弃未完成的计数。
    impl_->occlusion.counterFences.fill(0);
    impl_->occlusion.telemetryFences.fill(0);
    impl_->Collect();return {};
}
Result Device::Submit(CommandContext& commands,CompletionToken& token,Image* capture,HdrImage* hdrCapture,DepthImage* shadowCapture)
{
    if(!impl_||!impl_->Usable()||impl_->presentation.uploadStates||!commands.impl_||commands.impl_->phase!=5||commands.impl_->device.lock()!=impl_)return Fail(Error::InvalidState);
    auto& c=*commands.impl_;auto& p=impl_->presentation;
    // G06.09：复用帧槽回读前先消费已完成的计数；不等待 GPU。
    impl_->PollOcclusionCounters();
    impl_->PollOcclusionTelemetry();
    const UINT frame=p.frameCursor;
    if(c.guiComposite&&(c.guiComposite->native.width!=p.width||c.guiComposite->native.height!=p.height))return Fail(Error::InvalidArgument);
    if(c.gameUiPipeline)
    {
        const auto width=c.guiScene?c.guiScene->native.width:p.width,height=c.guiScene?c.guiScene->native.height:p.height;
        if(std::abs(c.gameUiGeometry.displaySize.x*c.gameUiGeometry.framebufferScale.x-width)>0.01f
            ||std::abs(c.gameUiGeometry.displaySize.y*c.gameUiGeometry.framebufferScale.y-height)>0.01f)return Fail(Error::InvalidArgument);
    }
    if(hdrCapture&&!c.forward)return Fail(Error::InvalidArgument);
    if(shadowCapture&&!c.shadow)return Fail(Error::InvalidArgument);
    std::vector<D3D12MeshDraw> draws;draws.reserve(c.draws.size());
    for(size_t i=0;i<c.draws.size();++i)
    {const auto& draw=c.draws[i];draws.push_back({&c.geometry[i],draw.world,draw.tint,draw.textureIsData,draw.unlit,draw.pbr,c.instances[i],&c.drawPipelines[i]->mesh,draw.impostorView,draw.impostorNext,draw.impostorBlend,
        draw.hasPreviousWorld,draw.previousWorld,c.drawPipelines[i]->mesh.prepared->program->source.compressedVertex});}
    // G06.08：批处理对象必须活到 RenderD3D12Frame 返回；这里只做本地借用。
    D3D12FrameContent content;content.viewProjection=c.viewProjection;content.meshes=draws;content.sourceFrame=c.sourceFrame;content.cacheOpaqueBindings=c.cacheOpaqueBindings;    std::vector<const D3D12Texture*> guiTextures;for(const auto& texture:c.guiTextures)guiTextures.push_back(&texture->native);
    D3D12GuiContent gui{c.guiScene?c.guiScene->target.get():nullptr,c.guiComposite?c.guiComposite->target.get():nullptr,
        {c.guiPipeline?&c.guiPipeline->mesh:nullptr,&c.guiGeometry,guiTextures}};
    if(c.composition)
    {
        auto hdr=impl_->textures.Get(c.composition->scene.hdrColor,impl_->id);
        auto ldr=impl_->textures.Get(c.composition->scene.ldrColor,impl_->id);
        if(!hdr||!ldr||!hdr->target||!ldr->target||!ldr->target->depth)return Fail(Error::InvalidHandle);
        content.sceneHdr=hdr->target.get();content.sceneLdr=ldr->target.get();
    }
    if(c.guiScene)content.gui=&gui;
    std::vector<const D3D12Texture*> gameUiTextures;for(const auto& texture:c.gameUiTextures)gameUiTextures.push_back(&texture->native);
    const D3D12GuiLayer gameUi{c.gameUiPipeline?&c.gameUiPipeline->mesh:nullptr,&c.gameUiGeometry,gameUiTextures};
    if(c.gameUiPipeline)content.gameUi=&gameUi;
    content.forward=c.forward?&*c.forward:nullptr;content.outputPipeline=c.outputPipeline?&c.outputPipeline->mesh:nullptr;
    // G06.08/G06.10：实例化遮挡。批次数必须与 DrawInstanced 次数、实例缓冲数量一致；
    // Forward 实例化允许同帧（HDR 目标由呈现层按 forward 选择）。阴影/后处理/GUI/合成仍互斥。
    std::vector<D3D12IndirectBatch> occlusionBatches;
    D3D12OcclusionTelemetry telemetry{};
    bool occlusionCountReadback=false;
    bool occlusionTelemetry=false;
    if(c.occlusion.active)
    {
        if(c.draws.size()!=c.occlusion.bounds.size()||c.instances.size()!=c.occlusion.bounds.size()
            ||c.shadow||c.post||c.guiScene||c.guiPipeline||c.composition)
            return Fail(Error::InvalidState);
        const bool forward=c.forward.has_value();
        const bool hizEnabled=c.occlusion.settings.enabled;
        for(size_t index=0;index<c.draws.size();++index)
        {
            const auto& entry=*c.drawPipelines[index];
            if(!IsInstanced(entry.kind)||entry.mesh.prepared->program->source.compressedVertex
                ||IsForward(entry.kind)!=forward
                ||entry.mesh.prepared->colorFormat!=(forward?ColorFormat::Rgba16Float:ColorFormat::Rgba8Unorm)
                ||c.instances[index].size()!=c.occlusion.bounds[index].size())
                return Fail(Error::Unsupported);
            if(hizEnabled&&entry.mesh.prepared->depth!=DepthMode::ReadOnlyLessEqual)return Fail(Error::Unsupported);
        }
        if(!impl_->EnsureOcclusion(c.drawPipelines[0]->mesh,forward))return Fail(Error::BackendFailure);
        auto& resources=impl_->occlusion;
        if(hizEnabled)
        {
            if(!p.depth)return Fail(Error::BackendFailure);
            if(p.width!=resources.width||p.height!=resources.height||resources.hizOutput.depth.Get()!=p.depth.Get())
            {
                if(FAILED(resources.hiz.CreateOutput(*p.depth.Get(),p.width,p.height,resources.hizOutput)))
                    return Fail(Error::BackendFailure);
                resources.width=p.width;resources.height=p.height;
            }
        }
        occlusionCountReadback=c.occlusion.settings.counterReadback&&!resources.counterFences[frame];
        if(occlusionCountReadback&&!impl_->EnsureOcclusionReadback(frame,static_cast<UINT>(c.draws.size())))
            return Fail(Error::BackendFailure);
        // G06.11：telemetry 只在 HZB 开启时有效；0/1 prepass、2..4 HZB、5 main、6+4i 起为批次 cull。
        const UINT telemetryQueries=static_cast<UINT>(6+4*c.draws.size());
        occlusionTelemetry=c.occlusion.settings.telemetry&&hizEnabled&&!resources.telemetryFences[frame];
        if(occlusionTelemetry)
        {
            if(!impl_->EnsureOcclusionTelemetry(frame,telemetryQueries))return Fail(Error::BackendFailure);
            telemetry.queries=resources.telemetryQueries[frame].Get();
        }
        auto& outputs=resources.outputs[p.frameCursor];
        if(outputs.size()<c.draws.size())outputs.resize(c.draws.size());
        occlusionBatches.reserve(c.draws.size());
        std::vector<std::uint32_t> candidates;
        std::uint64_t instances=0;
        if(occlusionCountReadback)candidates.reserve(c.draws.size());
        for(size_t index=0;index<c.draws.size();++index)
        {
            const auto count=static_cast<UINT>(c.occlusion.bounds[index].size());
            auto& output=outputs[index];
            if(output.inputCapacity<count||output.outputCapacity<count)
                if(FAILED(resources.indirect.CreateOutput(count,count,output)))return Fail(Error::BackendFailure);
            D3D12IndirectBatch batch{};
            batch.kernel=&resources.indirect;batch.output=&output;
            batch.bounds=c.occlusion.bounds[index];batch.frustum=c.occlusion.frustum;
            batch.depthBias=c.occlusion.settings.depthBias;
            if(hizEnabled){batch.hizKernel=&resources.hiz;batch.hiz=&resources.hizOutput;batch.prepass=&resources.prepass;}
            // G06.09/G06.10：只有显式请求计数的帧才复制参数；多批次共用一块 READBACK 并按 offset 分区。
            if(occlusionCountReadback)
            {
                batch.countReadback=resources.counterReadbacks[frame].front().Get();
                batch.countReadbackOffset=static_cast<UINT>(index)*D3D12Indirect::argumentBytes;
                candidates.push_back(count);
            }
            if(occlusionTelemetry)
            {
                batch.timestamps={resources.telemetryQueries[frame].Get(),telemetryQueries};
                batch.timestampFirstQuery=static_cast<UINT>(6+4*index);
                batch.timestampReadback=resources.telemetryReadbacks[frame].Get();
                batch.hizQueries=resources.telemetryQueries[frame].Get();
                batch.hizQueryCapacity=telemetryQueries;
                batch.hizFirstQuery=2;
            }
            instances+=count;
            occlusionBatches.push_back(batch);
        }
        if(occlusionCountReadback)
        {
            resources.counterFrames[frame]=c.sourceFrame;
            resources.counterCandidates[frame]=std::move(candidates);
        }
        content.indirects=occlusionBatches.data();content.indirectCount=occlusionBatches.size();
        if(occlusionTelemetry)content.occlusionTelemetry=&telemetry;
        ++impl_->occlusionFrames;impl_->occlusionInstances+=instances;
    }
    // G15：输出 LUT 布局需要一张 2D strip；Rgba8 为兼容路径，Rgba16Float 保留 >1 的显示线性值与精度。
    // 句柄失效/格式不符直接拒绝，避免采样到占位内存。
    if(c.forward&&c.forward->colorLut)
    {
        auto lut=impl_->textures.Get(c.forward->colorLut,impl_->id);
        if(!lut||lut->target||lut->native.cube||lut->native.arrayLayers!=1
            ||(lut->native.format!=TextureFormat::Rgba8&&lut->native.format!=TextureFormat::Rgba16Float))
            return Fail(Error::InvalidHandle);
        content.colorLut=&lut->native;
        c.leases.push_back(std::move(lut));
    }
    content.skyPipeline=c.skyPipeline?&c.skyPipeline->mesh:nullptr;
    content.environment=c.environment?&*c.environment:nullptr;
    for(size_t i=0;i<c.environmentTextures.size();++i)if(c.environmentTextures[i])content.environmentTextures[i]=&c.environmentTextures[i]->native;
    content.post=c.post?&*c.post:nullptr;content.overlays=c.overlays;
    for(size_t i=0;i<c.postPipelines.size();++i)if(c.postPipelines[i])content.postPipelines[i]=&c.postPipelines[i]->mesh;
    std::vector<D3D12MeshDraw> shadowDraws;shadowDraws.reserve(c.shadowWorlds.size());
    for(size_t index=0;index<c.shadowWorlds.size();++index)shadowDraws.push_back({&c.shadowGeometry[index],c.shadowWorlds[index]});
    content.shadow=c.shadow?&*c.shadow:nullptr;content.shadowPipeline=c.shadowPipeline?&c.shadowPipeline->mesh:nullptr;content.shadowMeshes=shadowDraws;
    if(c.pipeline)
    {if(c.pipeline->kind==PipelineKind::Triangle)content.triangle=&c.pipeline->triangle;else content.meshPipeline=&c.pipeline->mesh;}
    ComPtr<ID3D12Resource> readback,hdrReadback,shadowReadback;D3D12FrameCapture copying,hdrCopying,shadowCopying;UINT64 bytes=0,hdrBytes=0,shadowBytes=0;
    const auto prepareCapture=[&](DXGI_FORMAT format,UINT width,UINT height,ComPtr<ID3D12Resource>& captureBuffer,D3D12FrameCapture& request,UINT64& sizeBytes){
        auto desc=p.backBuffers[p.swapChain->GetCurrentBackBufferIndex()]->GetDesc();desc.Format=format;desc.Width=width;desc.Height=height;
        desc.Flags=format==DXGI_FORMAT_R32_TYPELESS?D3D12_RESOURCE_FLAG_ALLOW_DEPTH_STENCIL:D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET;
        impl_->context.device->GetCopyableFootprints(&desc,0,1,0,&request.footprint,nullptr,nullptr,&sizeBytes);
        D3D12_RESOURCE_DESC buffer{};buffer.Dimension=D3D12_RESOURCE_DIMENSION_BUFFER;buffer.Width=sizeBytes;
        buffer.Height=buffer.DepthOrArraySize=buffer.MipLevels=1;buffer.SampleDesc={1,0};buffer.Layout=D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
        D3D12_HEAP_PROPERTIES heap{};heap.Type=D3D12_HEAP_TYPE_READBACK;heap.CreationNodeMask=heap.VisibleNodeMask=1;
        if(FAILED(impl_->context.device->CreateCommittedResource(&heap,D3D12_HEAP_FLAG_NONE,&buffer,D3D12_RESOURCE_STATE_COPY_DEST,
            nullptr,IID_PPV_ARGS(captureBuffer.GetAddressOf()))))return false;
        request.readback=captureBuffer.Get();return true;
    };
    if(capture&&!prepareCapture(DXGI_FORMAT_R8G8B8A8_UNORM,p.width,p.height,readback,copying,bytes))return Fail(Error::BackendFailure);
    if(hdrCapture&&!prepareCapture(DXGI_FORMAT_R16G16B16A16_FLOAT,p.width,p.height,hdrReadback,hdrCopying,hdrBytes))return Fail(Error::BackendFailure);
    if(shadowCapture&&!prepareCapture(DXGI_FORMAT_R32_TYPELESS,c.shadow->resolution,c.shadow->resolution,shadowReadback,shadowCopying,shadowBytes))return Fail(Error::BackendFailure);
    content.hdrCapture=hdrCapture?&hdrCopying:nullptr;
    content.shadowCapture=shadowCapture?&shadowCopying:nullptr;
    impl_->pending.insert(impl_->pending.end(),c.leases.begin(),c.leases.end());
    if(!RenderD3D12Frame(p,impl_->text,content,capture?&copying:nullptr))
    {ReleaseD3D12Presentation(p);impl_->ready=false;return Fail(Error::BackendFailure);}
    const auto fence=p.frames[frame].fenceValue;token={impl_->id,1,fence};
    // 复制已与 ExecuteIndirect 同帧提交；该 Fence 完成后 CPU 才能安全映射。
    if(occlusionCountReadback)impl_->occlusion.counterFences[frame]=fence;
    if(occlusionTelemetry)
    {
        impl_->occlusion.telemetryFences[frame]=fence;
        impl_->occlusion.telemetryFrames[frame]=c.sourceFrame;
    }
    for(const auto& entry:c.leases)entry->lastUse=std::max(entry->lastUse,fence);
    impl_->inFlight.push_back({fence,std::move(c.leases)});impl_->pending.clear();
    c.geometry.clear();c.draws.clear();c.drawPipelines.clear();c.instances.clear();c.shadowGeometry.clear();c.shadowWorlds.clear();c.pipeline.reset();c.outputPipeline.reset();c.skyPipeline.reset();c.shadowPipeline.reset();
    c.occlusion={};
    c.guiScene.reset();c.guiComposite.reset();c.guiPipeline.reset();c.guiTextures.clear();c.guiGeometry={};c.phase=6;
    c.composition.reset();c.compositionHdr.reset();c.compositionLdr.reset();
    c.gameUiPipeline.reset();c.gameUiTextures.clear();c.gameUiGeometry={};
    if(capture||hdrCapture||shadowCapture)
    {
        if(!WaitIdle()){ReleaseD3D12Presentation(p);impl_->ready=false;return Fail(Error::BackendFailure);}
    }
    if(capture)
    {
        Image image{p.width,p.height,std::vector<std::uint8_t>(size_t(p.width)*p.height*4)};
        void* mapped=nullptr;const D3D12_RANGE range{0,static_cast<SIZE_T>(bytes)};
        if(FAILED(readback->Map(0,&range,&mapped)))return Fail(Error::BackendFailure);
        for(UINT row=0;row<p.height;++row)std::memcpy(image.rgba8.data()+size_t(row)*p.width*4,
            static_cast<const std::byte*>(mapped)+copying.footprint.Offset+size_t(row)*copying.footprint.Footprint.RowPitch,size_t(p.width)*4);
        const D3D12_RANGE noWrites{0,0};readback->Unmap(0,&noWrites);*capture=std::move(image);
    }
    if(hdrCapture)
    {
        HdrImage image{p.width,p.height,std::vector<float>(size_t(p.width)*p.height*4)};
        void* mapped=nullptr;const D3D12_RANGE range{0,static_cast<SIZE_T>(hdrBytes)};
        if(FAILED(hdrReadback->Map(0,&range,&mapped)))return Fail(Error::BackendFailure);
        for(UINT row=0;row<p.height;++row)for(size_t column=0;column<size_t(p.width)*4;++column)
        {
            std::uint16_t half=0;std::memcpy(&half,static_cast<const std::byte*>(mapped)+hdrCopying.footprint.Offset+size_t(row)*hdrCopying.footprint.Footprint.RowPitch+column*2,2);
            image.rgba32[size_t(row)*p.width*4+column]=HalfToFloat(half);
        }
        const D3D12_RANGE noWrites{0,0};hdrReadback->Unmap(0,&noWrites);*hdrCapture=std::move(image);
    }
    if(shadowCapture)
    {
        const UINT resolution=c.shadow->resolution;DepthImage image{resolution,resolution,std::vector<float>(size_t(resolution)*resolution)};
        void* mapped=nullptr;const D3D12_RANGE range{0,static_cast<SIZE_T>(shadowBytes)};
        if(FAILED(shadowReadback->Map(0,&range,&mapped)))return Fail(Error::BackendFailure);
        for(UINT row=0;row<resolution;++row)std::memcpy(image.depth.data()+size_t(row)*resolution,
            static_cast<const std::byte*>(mapped)+shadowCopying.footprint.Offset+size_t(row)*shadowCopying.footprint.Footprint.RowPitch,size_t(resolution)*sizeof(float));
        const D3D12_RANGE noWrites{0,0};shadowReadback->Unmap(0,&noWrites);*shadowCapture=std::move(image);
    }
    impl_->Collect();return {};
}
bool Device::IsComplete(CompletionToken token) const
{return impl_&&impl_->ready&&token.device==impl_->id&&token.queue==1&&token.value&&token.value<=impl_->Completed()&&impl_->Completed()!=UINT64_MAX;}
Result Device::WaitIdle()
{
    if(!impl_||!impl_->ready||impl_->presentation.uploadStates||impl_->owner!=std::this_thread::get_id())return Fail(Error::InvalidState);
    if(!WaitForD3D12Gpu(impl_->presentation,impl_->text))return Fail(Error::BackendFailure);
    impl_->pending.clear();impl_->Collect();return {};
}
void Device::Collect(){if(impl_&&impl_->Usable())impl_->Collect();}
Statistics Device::Stats() const
{
    Statistics stats;if(!impl_)return stats;
    // G06.09：Stats 是只读查询而非提交点；这里只消费已完成回读，不等待 GPU。
    impl_->PollOcclusionCounters();
    impl_->PollOcclusionTelemetry();
    const auto& p=*impl_;stats.opaqueBindings=p.presentation.opaqueBindings;
    stats.frameInstances=p.presentation.frameInstances;stats.frameInstanceBytes=p.presentation.frameInstanceBytes;stats.frameDrawConstantBytes=p.presentation.frameDrawConstantBytes;
    stats.hdrCreations=p.presentation.hdrCreations;if(p.presentation.hdr){stats.hdrWidth=p.presentation.width;stats.hdrHeight=p.presentation.height;}
    stats.shadowCreations=p.presentation.shadowCreations;if(p.presentation.shadow)stats.shadowResolution=p.presentation.shadow->resolution;
    stats.postCreations=p.presentation.postCreations;
    if(p.presentation.post){const auto& post=*p.presentation.post;stats.postWidth=post.width;stats.postHeight=post.height;stats.bloomWidth=post.bloomWidth;stats.bloomHeight=post.bloomHeight;stats.postResourceBytes=post.allocatedBytes;}
    stats.buffers=p.buffers.Count();stats.textures=p.textures.Count();stats.pipelines=p.pipelines.Count();
    stats.retiring=p.retiring.size();stats.inFlightBatches=p.inFlight.size();stats.completedValue=p.Completed();stats.submittedFrames=p.presentation.submittedFrames;
    if(p.context.states)
    {
        const auto states=p.context.states->Stats();stats.trackedResources=states.resources;stats.trackedSubresources=states.subresources;
        stats.stateTransitions=states.transitions;stats.bufferDecays=states.bufferDecays;stats.stateActivations=states.activations;
    }
    for(const auto& frame:p.presentation.frames)
    {
        for(const auto upload:{frame.uploads.Stats(),frame.baselineConstants.Stats()})
        {
            stats.uploadAllocations+=upload.allocations;stats.uploadPagesCreated+=upload.pagesCreated;
            stats.uploadReservedBytes+=upload.reservedBytes;stats.uploadPeakUsedBytes+=upload.peakUsedBytes;stats.uploadFailures+=upload.failures;
        }
    }
    stats.uploadSubmissions=p.presentation.uploadSubmissions;stats.uploadedBytes=p.presentation.uploadedBytes;stats.descriptorFailures=p.descriptorRejections;
    stats.stagedCopies=p.stagedCopies;stats.stagedPayloadBytes=p.stagedPayloadBytes;stats.stagedFootprintBytes=p.stagedFootprintBytes;
    stats.occlusionFrames=p.occlusionFrames;stats.occlusionInstances=p.occlusionInstances;stats.occlusion=p.occlusion.counters;
    stats.occlusionTiming=p.occlusion.timing;
    if(p.context.srvDescriptors)
    {
        const auto srv=p.context.srvDescriptors->Stats();stats.persistentDescriptors=srv.used;stats.descriptorCapacity=srv.capacity;stats.descriptorPeak=srv.peak;
        stats.descriptorHeapsCreated+=srv.heapsCreated;stats.descriptorFailures+=srv.failures;
    }
    if(p.context.rtvDescriptors){const auto rtv=p.context.rtvDescriptors->Stats();stats.rtvDescriptors=rtv.used;stats.descriptorHeapsCreated+=rtv.heapsCreated;}
    if(p.context.dsvDescriptors){const auto dsv=p.context.dsvDescriptors->Stats();stats.dsvDescriptors=dsv.used;stats.descriptorHeapsCreated+=dsv.heapsCreated;}
    for(const auto& frame:p.presentation.frames)
    {
        for(const auto table:{frame.descriptors.Stats(),frame.outputDescriptors.Stats(),frame.environmentDescriptors.Stats(),frame.postDescriptors.Stats(),frame.guiDescriptors.Stats(),frame.gameUiDescriptors.Stats()})
        {
            stats.frameDescriptors+=table.used;stats.frameDescriptorCapacity+=table.capacity;stats.frameDescriptorPeak+=table.peak;
            stats.descriptorHeapsCreated+=table.heapsCreated;stats.descriptorCopies+=table.allocations;stats.descriptorFailures+=table.failures;
        }
    }
    for(const auto& entry:p.retiring)if(const auto texture=std::dynamic_pointer_cast<TextureEntry>(entry);texture&&texture->native.views)
        stats.retiringDescriptors+=texture->native.views->Count();
    if(p.context.memory)
    {
        const auto memory=p.context.memory->Stats();stats.memoryBudgetBytes=memory.budget;stats.memoryReservedBytes=memory.reserved;
        stats.memoryUsedBytes=memory.used;stats.memoryFreeBytes=memory.free;stats.memoryPeakReservedBytes=memory.peakReserved;stats.memoryPeakUsedBytes=memory.peakUsed;
        stats.memoryResources=memory.resources;stats.memoryHeaps=memory.placedHeaps;stats.memoryCommittedResources=memory.committedResources;
        stats.memoryHeapCreations=memory.heapCreations;stats.memoryCommittedCreations=memory.committedCreations;
        stats.memoryResourceCreations=memory.resourceCreations;stats.memoryBudgetFailures=memory.budgetFailures;
    }
    if(p.context.pipelines)
    {
        const auto cache=p.context.pipelines->Stats();stats.shaderProgramCompilations=cache.shaders.compilations;stats.shaderArtifactLoads=cache.shaders.artifactLoads;
        stats.shaderArtifactRejections=cache.shaders.artifactRejections;stats.shaderProgramHits=cache.programHits;
        stats.pipelineCacheHits=cache.pipelineHits;stats.pipelineCacheMisses=cache.pipelineMisses;stats.pipelineCreations=cache.pipelineCreations;stats.pipelineLibraryHits=cache.libraryHits;stats.pipelineLibraryStores=cache.libraryStores;
        stats.rootSignatureCreations=cache.rootCreations;stats.livePreparedPipelines=cache.livePipelines;stats.liveShaderPrograms=cache.livePrograms;
    }
    for(size_t i=1;i<p.presentation.flushCounts.size();++i)stats.globalWaits+=p.presentation.flushCounts[i];
    ComPtr<ID3D12InfoQueue> info;if(p.context.device&&SUCCEEDED(p.context.device.As(&info)))
    {
        for(UINT64 i=0;i<info->GetNumStoredMessages();++i)
        {
            SIZE_T bytes=0;info->GetMessage(i,nullptr,&bytes);std::vector<std::byte> storage(bytes);
            auto* message=reinterpret_cast<D3D12_MESSAGE*>(storage.data());
            if(SUCCEEDED(info->GetMessage(i,message,&bytes)))
            {if(message->Severity==D3D12_MESSAGE_SEVERITY_WARNING)++stats.debugWarnings;else if(message->Severity<=D3D12_MESSAGE_SEVERITY_ERROR)++stats.debugErrors;}
        }
    }
    return stats;
}
bool Device::Occluded() const{return impl_&&impl_->presentation.occluded;}
Result Device::EnableGpuTiming(bool enabled)
{
    if(!impl_||!impl_->Usable()||impl_->presentation.uploadStates)return Fail(Error::InvalidState);
    return SetD3D12GpuTiming(impl_->presentation,enabled,impl_->text)?Result{}:Fail(Error::BackendFailure);
}
void Device::SetCpuProfiler(Profiler* profiler)
{if(impl_&&impl_->Usable())impl_->presentation.cpuProfiler=profiler;}
std::vector<GpuTiming> Device::GpuTimings()
{return GpuTimingsSince(0);}
std::vector<GpuTiming> Device::GpuTimingsSince(std::uint64_t submission)
{
    if(!impl_||!impl_->Usable())return {};
    if(!ReadD3D12GpuTimings(impl_->presentation,impl_->text)){impl_->presentation.failed=true;return {};}
    const auto& samples=impl_->presentation.timings;
    const auto first=std::upper_bound(samples.begin(),samples.end(),submission,[](auto cursor,const GpuTiming& value){return cursor<value.submission;});
    return {first,samples.end()};
}
AdapterInformation Device::Adapter() const
{
    if(!impl_||!impl_->Usable())return {};DXGI_ADAPTER_DESC1 desc{};if(FAILED(impl_->context.adapter->GetDesc1(&desc)))return {};
    std::u16string units;for(const wchar_t c:std::wstring_view(desc.Description))units+=static_cast<char16_t>(c);std::u32string decoded;std::string name;
    if(!DecodeUtf16(units,decoded)||!EncodeUtf8(decoded,name))name="Adapter";
    return {name,desc.VendorId,desc.DeviceId,desc.DedicatedVideoMemory,(desc.Flags&DXGI_ADAPTER_FLAG_SOFTWARE)!=0};
}
Result Device::ExportGpuTimingsCsv(const std::filesystem::path& path)
{
    if(!WaitIdle())return Fail(Error::BackendFailure);
    const auto samples=GpuTimings();
    if(!impl_->Usable())return Fail(Error::BackendFailure);
    std::ostringstream csv;csv.imbue(std::locale::classic());
    // 每行同时保留原始 tick 与换算值，不能与 CPU 区间相加。
    csv<<std::setprecision(17)<<"source_frame,submission,queue,frequency_hz,frame_begin,geometry_begin,geometry_end,frame_end,draws,gpu_frame_ms,gpu_geometry_ms\n";
    for(const auto& sample:samples)
    {
        csv<<sample.sourceFrame<<','<<sample.submission<<",1,"<<sample.frequency;
        for(const auto tick:sample.ticks)csv<<','<<tick;
        csv<<','<<sample.draws<<','<<sample.frameMilliseconds<<','<<sample.geometryMilliseconds<<'\n';
    }
    std::string error;
    return WriteProfileCsv(path,csv.str(),error)?Result{}:Fail(Error::BackendFailure);
}
Result Device::ExportGpuPassTimingsCsv(const std::filesystem::path& path)
{
    if(!WaitIdle())return Fail(Error::BackendFailure);
    const auto samples=GpuTimings();if(!impl_->Usable())return Fail(Error::BackendFailure);
    std::ostringstream csv;csv.imbue(std::locale::classic());csv<<std::setprecision(17)<<"source_frame,submission,queue,pass,frequency_hz,begin,end,gpu_ms\n";
    const char* names[]{"Clear","Sky","Opaque","Output"};
    for(const auto& sample:samples)if(sample.forward)for(size_t pass=0;pass<4;++pass)
    {
        const auto first=sample.passTicks[pass*2],last=sample.passTicks[pass*2+1];
        csv<<sample.sourceFrame<<','<<sample.submission<<",1,"<<(sample.post&&pass==3?"PostProcess":names[pass])<<','<<sample.frequency<<','<<first<<','<<last<<','<<double(last-first)*1000/double(sample.frequency)<<'\n';
        if(pass==0&&sample.shadow)
        {
            const auto begin=sample.shadowTicks[0],end=sample.shadowTicks[1];
            csv<<sample.sourceFrame<<','<<sample.submission<<",1,Shadow,"<<sample.frequency<<','<<begin<<','<<end<<','<<double(end-begin)*1000/double(sample.frequency)<<'\n';
        }
        if(sample.post&&pass==3)
        {
            const char* postNames[]{"BloomExtract","BloomHorizontal","BloomVertical",sample.postToneMapping?"ToneMap":"ExposureClamp",sample.postFxaa?"FXAA":"PresentCopy","UI"};
            for(size_t stage=0;stage<6;++stage)
            {
                if((stage<3&&!sample.postBloom)||(stage==5&&!sample.postUi))continue;
                const auto begin=sample.postTicks[stage*2],end=sample.postTicks[stage*2+1];
                csv<<sample.sourceFrame<<','<<sample.submission<<",1,"<<postNames[stage]<<','<<sample.frequency<<','<<begin<<','<<end<<','<<double(end-begin)*1000/double(sample.frequency)<<'\n';
            }
        }
    }
    std::string error;return WriteProfileCsv(path,csv.str(),error)?Result{}:Fail(Error::BackendFailure);
}
void Device::Shutdown() noexcept
{
    if(!impl_)return;
    ReleaseD3D12Presentation(impl_->presentation);impl_->ready=false;
    impl_->inFlight.clear();impl_->pending.clear();impl_->retiring.clear();
    impl_->buffers.slots.clear();impl_->textures.slots.clear();impl_->pipelines.slots.clear();
    impl_->context={};
}
ID3D12Device* D3D12DiagnosticsAccess::NativeDevice(Device& d){return d.impl_?d.impl_->context.device.Get():nullptr;}
ID3D12CommandQueue* D3D12DiagnosticsAccess::Queue(Device& d){return d.impl_?d.impl_->presentation.queue.Get():nullptr;}
D3D12Presentation* D3D12DiagnosticsAccess::Presentation(Device& d){return d.impl_?&d.impl_->presentation:nullptr;}
std::shared_ptr<D3D12MemoryAllocator> D3D12DiagnosticsAccess::Memory(Device& d){return d.impl_?d.impl_->context.memory:nullptr;}
ID3D12PipelineState* D3D12DiagnosticsAccess::PipelineState(Device& d,PipelineHandle handle)
{if(!d.impl_)return nullptr;const auto pipeline=d.impl_->pipelines.Get(handle,d.impl_->id);return !pipeline?nullptr:pipeline->kind==PipelineKind::Triangle?pipeline->triangle.pipeline.Get():pipeline->mesh.pipeline.Get();}
std::weak_ptr<const D3D12PreparedPipeline> D3D12DiagnosticsAccess::Prepared(Device& d,PipelineHandle handle)
{if(!d.impl_)return {};const auto pipeline=d.impl_->pipelines.Get(handle,d.impl_->id);return !pipeline?std::weak_ptr<const D3D12PreparedPipeline>{}:pipeline->kind==PipelineKind::Triangle?pipeline->triangle.prepared:pipeline->mesh.prepared;}
D3D12_CPU_DESCRIPTOR_HANDLE D3D12DiagnosticsAccess::SrvCpu(Device& d,TextureHandle handle,UINT view)
{if(!d.impl_)return {};const auto texture=d.impl_->textures.Get(handle,d.impl_->id);return texture&&texture->native.views?texture->native.views->Cpu(view):D3D12_CPU_DESCRIPTOR_HANDLE{};}
ID3D12Resource* D3D12DiagnosticsAccess::Resource(Device& d,BufferHandle handle)
{if(!d.impl_)return nullptr;auto value=d.impl_->buffers.Get(handle,d.impl_->id);return value?value->resource.Get():nullptr;}
ID3D12Resource* D3D12DiagnosticsAccess::Resource(Device& d,TextureHandle handle)
{if(!d.impl_)return nullptr;auto value=d.impl_->textures.Get(handle,d.impl_->id);return value?value->native.resource.Get():nullptr;}
std::weak_ptr<void> D3D12DiagnosticsAccess::Lifetime(Device& d,BufferHandle handle)
{return d.impl_?d.impl_->buffers.Get(handle,d.impl_->id):std::shared_ptr<BufferEntry>{};}
std::weak_ptr<void> D3D12DiagnosticsAccess::Lifetime(Device& d,TextureHandle handle)
{return d.impl_?d.impl_->textures.Get(handle,d.impl_->id):std::shared_ptr<TextureEntry>{};}
std::weak_ptr<void> D3D12DiagnosticsAccess::Lifetime(Device& d,PipelineHandle handle)
{return d.impl_?d.impl_->pipelines.Get(handle,d.impl_->id):std::shared_ptr<PipelineEntry>{};}
std::shared_ptr<D3D12StateRegistry> D3D12DiagnosticsAccess::States(Device& d){return d.impl_?d.impl_->context.states:nullptr;}
}
