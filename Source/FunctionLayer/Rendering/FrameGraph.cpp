#include "FrameGraph.h"
#include <algorithm>
#include <chrono>
#include <cstdarg>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <sstream>
#include <string>

namespace se
{
namespace
{
constexpr std::uint32_t kMaxRefsPerPass=16;

bool IsBlockCompressed(rhi::TextureFormat format)
{
    return format==rhi::TextureFormat::Bc1Unorm||format==rhi::TextureFormat::Bc1UnormSrgb
        ||format==rhi::TextureFormat::Bc5Unorm||format==rhi::TextureFormat::Bc4Unorm;
}

std::uint64_t BlockBytes(rhi::TextureFormat format)
{
    switch(format)
    {
    case rhi::TextureFormat::Bc1Unorm:
    case rhi::TextureFormat::Bc1UnormSrgb:
    case rhi::TextureFormat::Bc4Unorm: return 8;
    case rhi::TextureFormat::Bc5Unorm: return 16;
    default: return 0;
    }
}

std::uint64_t TextureBytes(const FrameGraphTextureDesc& desc)
{
    if(!desc.width||!desc.height)return 0;
    const auto mipCount=desc.mipLevels?desc.mipLevels:1;
    std::uint64_t total=0;
    for(std::uint32_t mip=0;mip<mipCount;++mip)
    {
        const auto width=(std::max)(1u,desc.width>>mip);
        const auto height=(std::max)(1u,desc.height>>mip);
        if(desc.depth)
        {
            total+=std::uint64_t(width)*height*4;
        }
        else if(IsBlockCompressed(desc.format))
        {
            const auto blocksX=(width+3)/4,blocksY=(height+3)/4;
            total+=std::uint64_t(blocksX)*blocksY*BlockBytes(desc.format);
        }
        else
        {
            const std::uint64_t bytesPerPixel=desc.format==rhi::TextureFormat::Rgba16Float?8:4;
            total+=std::uint64_t(width)*height*bytesPerPixel;
        }
    }
    return total;
}

const char* PassKindName(rhi::PassKind kind)
{
    switch(kind)
    {
    case rhi::PassKind::Clear: return "Clear";
    case rhi::PassKind::Opaque: return "Opaque";
    case rhi::PassKind::Triangle: return "Triangle";
    case rhi::PassKind::Sky: return "Sky";
    case rhi::PassKind::Output: return "Output";
    case rhi::PassKind::Shadow: return "Shadow";
    case rhi::PassKind::BloomExtract: return "BloomExtract";
    case rhi::PassKind::BloomHorizontal: return "BloomHorizontal";
    case rhi::PassKind::BloomVertical: return "BloomVertical";
    case rhi::PassKind::ToneMap: return "ToneMap";
    case rhi::PassKind::Fxaa: return "Fxaa";
    case rhi::PassKind::Ui: return "Ui";
    default: return "Unknown";
    }
}

const char* ResourceUseName(rhi::ResourceUse use)
{
    switch(use)
    {
    case rhi::ResourceUse::ColorWrite: return "ColorWrite";
    case rhi::ResourceUse::DepthWrite: return "DepthWrite";
    case rhi::ResourceUse::VertexRead: return "VertexRead";
    case rhi::ResourceUse::IndexRead: return "IndexRead";
    case rhi::ResourceUse::TextureRead: return "TextureRead";
    case rhi::ResourceUse::ColorRead: return "ColorRead";
    case rhi::ResourceUse::None: return "None";
    case rhi::ResourceUse::DepthRead: return "DepthRead";
    default: return "Unknown";
    }
}

const char* TextureFormatName(rhi::TextureFormat format)
{
    switch(format)
    {
    case rhi::TextureFormat::Rgba8: return "Rgba8";
    case rhi::TextureFormat::Rgba16Float: return "Rgba16Float";
    case rhi::TextureFormat::Bc1Unorm: return "Bc1Unorm";
    case rhi::TextureFormat::Bc1UnormSrgb: return "Bc1UnormSrgb";
    case rhi::TextureFormat::Bc5Unorm: return "Bc5Unorm";
    case rhi::TextureFormat::Bc4Unorm: return "Bc4Unorm";
    default: return "Unknown";
    }
}

std::string EscapeDot(std::string_view text)
{
    std::string output;
    output.reserve(text.size()+8);
    for(const auto character:text)
    {
        if(character=='"'||character=='\\')output.push_back('\\');
        output.push_back(character);
    }
    return output;
}

std::string EscapeJson(std::string_view text)
{
    std::string output;
    output.reserve(text.size()+8);
    for(const auto character:text)
    {
        switch(character)
        {
        case '"': output+="\\\""; break;
        case '\\': output+="\\\\"; break;
        case '\n': output+="\\n"; break;
        case '\r': output+="\\r"; break;
        case '\t': output+="\\t"; break;
        default:
            if(static_cast<unsigned char>(character)<0x20)
            {
                char buffer[8]{};
                std::snprintf(buffer,sizeof(buffer),"\\u%04x",static_cast<unsigned char>(character));
                output+=buffer;
            }
            else output.push_back(character);
            break;
        }
    }
    return output;
}

// 固定容量的帧 arena：构造时一次性分配缓冲，之后每帧只重置计数、复用同一批数组，
// 因此 Pass 声明与编译不会产生堆分配；容量不足时 Compile 返回 CapacityExceeded。
class FrameGraphArena
{
public:
    explicit FrameGraphArena(std::size_t bytes)
        : capacity_(bytes),buffer_(std::make_unique<std::byte[]>(bytes)) {}
    template<class T> T* Allocate(std::size_t count=1) noexcept
    {
        if(count>0&&sizeof(T)>SIZE_MAX/count)return nullptr;
        const auto alignment=alignof(T);
        const auto aligned=(offset_+alignment-1)&~(alignment-1);
        const auto bytes=sizeof(T)*count;
        if(aligned>capacity_||bytes>capacity_-aligned)return nullptr;
        auto* result=reinterpret_cast<T*>(buffer_.get()+aligned);
        std::memset(result,0,bytes);
        offset_=aligned+bytes;
        return result;
    }
    std::size_t Capacity() const noexcept {return capacity_;}
    std::size_t Used() const noexcept {return offset_;}
private:
    std::size_t capacity_=0,offset_=0;
    std::unique_ptr<std::byte[]> buffer_;
};

struct PassRecord
{
    std::string_view name;
    rhi::PassKind kind=rhi::PassKind::Clear;
    rhi::ResourceUse color=rhi::ResourceUse::ColorWrite,depth=rhi::ResourceUse::DepthWrite,input=rhi::ResourceUse::None;
    bool cacheBindings=false,sideEffect=false,enabled=true;
    std::uint32_t execute=0,declaration=0,readCount=0,writeCount=0,resolvedCount=0;
    FrameGraphResourceRef reads[kMaxRefsPerPass]{};
    FrameGraphResourceRef writes[kMaxRefsPerPass]{};
    FrameGraphResourceRef resolved[kMaxRefsPerPass]{};
};

struct ResourceRecord
{
    std::string_view name;
    FrameGraphTextureDesc desc;
    bool imported=false,used=false;
    std::uint32_t versionCount=0;
    std::uint32_t first=FrameGraphInvalid,last=FrameGraphInvalid,aliasGroup=FrameGraphInvalid;
    std::uint64_t bytes=0;
};

struct Edge
{
    std::uint32_t from=0,to=0,resource=0,version=0;
};
}

struct FrameGraph::Impl
{
    Impl(std::uint32_t passCapacity,std::uint32_t resourceCapacity,std::uint32_t edgeCapacity,std::uint32_t versionCapacity)
        : maxPasses(passCapacity),maxResources(resourceCapacity),maxEdges(edgeCapacity),maxVersions(versionCapacity),
          arena(ArenaBytes(passCapacity,resourceCapacity,edgeCapacity,versionCapacity))
    {
        Allocate();
        Reset();
    }

    static std::size_t ArenaBytes(std::uint32_t passes,std::uint32_t resources,std::uint32_t edges,std::uint32_t versions)
    {
        std::size_t bytes=0;
        bytes+=std::size_t(passes)*(sizeof(PassRecord)+sizeof(FrameGraphPassView)+sizeof(std::uint8_t)*3+sizeof(std::uint32_t)*2);
        bytes+=std::size_t(resources)*(sizeof(ResourceRecord)+sizeof(FrameGraphTextureView)+sizeof(std::uint32_t)*4+sizeof(std::uint64_t)+sizeof(std::uint8_t)*2);
        bytes+=std::size_t(edges)*sizeof(Edge);
        bytes+=std::size_t(resources)*versions*sizeof(std::uint32_t);
        bytes+=64*1024; // 对齐与实现余量
        return (std::max)(bytes,std::size_t(64*1024));
    }

    void Allocate() noexcept
    {
        passes=arena.Allocate<PassRecord>(maxPasses);
        resources=arena.Allocate<ResourceRecord>(maxResources);
        edges=arena.Allocate<Edge>(maxEdges);
        order=arena.Allocate<std::uint32_t>(maxPasses);
        queue=arena.Allocate<std::uint32_t>(maxPasses);
        live=arena.Allocate<std::uint8_t>(maxPasses);
        emitted=arena.Allocate<std::uint8_t>(maxPasses);
        indegree=arena.Allocate<std::uint32_t>(maxPasses);
        writers=arena.Allocate<std::uint32_t>(std::size_t(maxResources)*maxVersions);
        group=arena.Allocate<std::uint32_t>(maxResources);
        slotEnd=arena.Allocate<std::uint32_t>(maxResources);
        slotBytes=arena.Allocate<std::uint64_t>(maxResources);
        passViews=arena.Allocate<FrameGraphPassView>(maxPasses);
        textureViews=arena.Allocate<FrameGraphTextureView>(maxResources);
        arenaFailed=!passes||!resources||!edges||!order||!queue||!live||!emitted||!indegree||!writers
            ||!group||!slotEnd||!slotBytes||!passViews||!textureViews;
    }

    void Reset() noexcept
    {
        // 数组在 Allocate 中一次性从 arena 取得；每帧只重置逻辑计数，不重新分配。
        passCount=resourceCount=edgeCount=0;
        orderCount=0;
        passLimit=0;
        status=FrameGraphStatus::Success;
        error[0]='\0';
        stats={};
        if(arenaFailed)Fail(FrameGraphStatus::CapacityExceeded,"CapacityExceeded: frame arena is too small");
    }

    FrameGraphStatus Fail(FrameGraphStatus value,const char* message) noexcept
    {
        status=value;
        std::snprintf(error,sizeof(error),"%s",message?message:"");
        return value;
    }

    FrameGraphStatus FailFormat(FrameGraphStatus value,const char* format,...) noexcept
    {
        status=value;
        va_list arguments;
        va_start(arguments,format);
        std::vsnprintf(error,sizeof(error),format,arguments);
        va_end(arguments);
        return value;
    }

    bool ValidPass(std::uint32_t pass) const noexcept {return pass<passCount;}

    std::uint32_t Producer(std::uint32_t resource,std::uint32_t version) const noexcept
    {
        if(resource>=resourceCount||version==0||version>resources[resource].versionCount)return FrameGraphInvalid;
        return writers[std::size_t(resource)*maxVersions+(version-1)];
    }

    bool AddRead(std::uint32_t pass,FrameGraphResourceRef reference) noexcept
    {
        if(!ValidPass(pass))
        {
            Fail(FrameGraphStatus::InvalidArgument,"InvalidArgument: pass index out of range");
            return false;
        }
        if(reference.resource>=resourceCount)
        {
            Fail(FrameGraphStatus::InvalidArgument,"InvalidArgument: resource index out of range");
            return false;
        }
        auto& record=passes[pass];
        if(record.readCount>=kMaxRefsPerPass)
        {
            Fail(FrameGraphStatus::CapacityExceeded,"CapacityExceeded: too many reads in one pass");
            return false;
        }
        record.reads[record.readCount++]=reference;
        return true;
    }

    bool AddEdge(std::uint32_t from,std::uint32_t to,std::uint32_t resource,std::uint32_t version) noexcept
    {
        if(from==to||from>=passCount||to>=passCount)return true;
        for(std::uint32_t index=0;index<edgeCount;++index)
            if(edges[index].from==from&&edges[index].to==to&&edges[index].resource==resource&&edges[index].version==version)
                return true;
        if(edgeCount>=maxEdges)
        {
            Fail(FrameGraphStatus::CapacityExceeded,"CapacityExceeded: too many dependency edges");
            return false;
        }
        edges[edgeCount++]={from,to,resource,version};
        return true;
    }

    std::uint32_t maxPasses=0,maxResources=0,maxEdges=0,maxVersions=0;
    FrameGraphArena arena;
    bool arenaFailed=false;
    PassRecord* passes=nullptr;
    ResourceRecord* resources=nullptr;
    Edge* edges=nullptr;
    std::uint32_t* order=nullptr;
    std::uint32_t* queue=nullptr;
    std::uint8_t* live=nullptr;
    std::uint8_t* emitted=nullptr;
    std::uint32_t* indegree=nullptr;
    std::uint32_t* writers=nullptr;
    std::uint32_t* group=nullptr;
    std::uint32_t* slotEnd=nullptr;
    std::uint64_t* slotBytes=nullptr;
    FrameGraphPassView* passViews=nullptr;
    FrameGraphTextureView* textureViews=nullptr;
    std::uint32_t passCount=0,resourceCount=0,edgeCount=0,orderCount=0,passLimit=0;
    FrameGraphStatus status=FrameGraphStatus::Success;
    char error[512]{};
    FrameGraphStats stats;
};

FrameGraph::FrameGraph(std::uint32_t maxPasses,std::uint32_t maxResources,std::uint32_t maxEdges,std::uint32_t maxVersions)
    : impl_(std::make_unique<Impl>(maxPasses,maxResources,maxEdges,maxVersions?maxVersions:1))
{
}

FrameGraph::~FrameGraph()=default;
FrameGraph::FrameGraph(FrameGraph&&) noexcept=default;
FrameGraph& FrameGraph::operator=(FrameGraph&&) noexcept=default;

void FrameGraph::Reset() noexcept
{
    impl_->Reset();
}

std::uint32_t FrameGraph::AddTexture(std::string_view name,const FrameGraphTextureDesc& desc,bool imported)
{
    auto& state=*impl_;
    if(state.resourceCount>=state.maxResources)
    {
        state.Fail(FrameGraphStatus::CapacityExceeded,"CapacityExceeded: too many resources");
        return FrameGraphInvalid;
    }
    const auto index=state.resourceCount++;
    auto& resource=state.resources[index];
    resource.name=name;
    resource.desc=desc;
    resource.imported=imported;
    resource.versionCount=0;
    resource.used=false;
    resource.first=resource.last=resource.aliasGroup=FrameGraphInvalid;
    resource.bytes=TextureBytes(desc);
    return index;
}

std::uint32_t FrameGraph::AddPass(const FrameGraphPassDesc& desc)
{
    auto& state=*impl_;
    if(state.passCount>=state.maxPasses)
    {
        state.Fail(FrameGraphStatus::CapacityExceeded,"CapacityExceeded: too many passes");
        return FrameGraphInvalid;
    }
    const auto index=state.passCount++;
    auto& pass=state.passes[index];
    pass={};
    pass.name=desc.name;
    pass.kind=desc.kind;
    pass.color=desc.color;
    pass.depth=desc.depth;
    pass.input=desc.input;
    pass.cacheBindings=desc.cacheBindings;
    pass.sideEffect=desc.sideEffect;
    pass.enabled=true;
    pass.execute=desc.execute;
    pass.declaration=index;
    return index;
}

void FrameGraph::Read(std::uint32_t pass,std::uint32_t resource)
{
    auto& state=*impl_;
    if(resource>=state.resourceCount)
    {
        state.Fail(FrameGraphStatus::InvalidArgument,"InvalidArgument: read of unknown resource");
        return;
    }
    state.AddRead(pass,{resource,state.resources[resource].versionCount});
}

void FrameGraph::ReadVersion(std::uint32_t pass,FrameGraphResourceRef reference)
{
    impl_->AddRead(pass,reference);
}

FrameGraphResourceRef FrameGraph::Write(std::uint32_t pass,std::uint32_t resource)
{
    auto& state=*impl_;
    if(!state.ValidPass(pass)||resource>=state.resourceCount)
    {
        state.Fail(FrameGraphStatus::InvalidArgument,"InvalidArgument: write to unknown pass/resource");
        return {};
    }
    auto& record=state.passes[pass];
    for(std::uint32_t index=0;index<record.writeCount;++index)
        if(record.writes[index].resource==resource)
        {
            state.FailFormat(FrameGraphStatus::DuplicateWrite,"DuplicateWrite: pass '%.*s' writes resource %u twice",
                static_cast<int>(record.name.size()),record.name.data(),resource);
            return {};
        }
    auto& target=state.resources[resource];
    if(target.versionCount>=state.maxVersions)
    {
        state.Fail(FrameGraphStatus::CapacityExceeded,"CapacityExceeded: too many resource versions");
        return {};
    }
    if(record.writeCount>=kMaxRefsPerPass)
    {
        state.Fail(FrameGraphStatus::CapacityExceeded,"CapacityExceeded: too many writes in one pass");
        return {};
    }
    const auto version=++target.versionCount;
    state.writers[std::size_t(resource)*state.maxVersions+(version-1)]=pass;
    const FrameGraphResourceRef reference{resource,version};
    record.writes[record.writeCount++]=reference;
    return reference;
}

void FrameGraph::SetPassEnabled(std::uint32_t pass,bool enabled) noexcept
{
    if(impl_->ValidPass(pass))impl_->passes[pass].enabled=enabled;
}

void FrameGraph::SetPassLimit(std::uint32_t limit) noexcept
{
    impl_->passLimit=limit;
}

std::uint32_t FrameGraph::LatestVersion(std::uint32_t resource) const noexcept
{
    return resource<impl_->resourceCount?impl_->resources[resource].versionCount:0;
}

FrameGraphStatus FrameGraph::Compile() noexcept
{
    auto& state=*impl_;
    const auto start=std::chrono::steady_clock::now();
    if(state.status!=FrameGraphStatus::Success)
    {
        state.stats={};
        return state.status;
    }
    state.orderCount=0;
    state.edgeCount=0;
    state.stats={};
    for(std::uint32_t index=0;index<state.passCount;++index)
    {
        auto& pass=state.passes[index];
        pass.resolvedCount=0;
        state.live[index]=0;
        state.emitted[index]=0;
        state.indegree[index]=0;
    }
    for(std::uint32_t index=0;index<state.resourceCount;++index)
    {
        auto& resource=state.resources[index];
        resource.used=false;
        resource.first=resource.last=resource.aliasGroup=FrameGraphInvalid;
    }
    if(state.passCount==0)
        return state.Fail(FrameGraphStatus::InvalidArgument,"InvalidArgument: frame graph has no passes");
    for(std::uint32_t first=0;first<state.passCount;++first)
        for(std::uint32_t second=first+1;second<state.passCount;++second)
            if(state.passes[first].name==state.passes[second].name)
                return state.FailFormat(FrameGraphStatus::DuplicatePass,"DuplicatePass: '%.*s'",
                    static_cast<int>(state.passes[first].name.size()),state.passes[first].name.data());

    // 1. 解析版本：禁用 Pass 的写入不生效，读者回退到更早的启用版本。
    for(std::uint32_t index=0;index<state.passCount;++index)
    {
        auto& pass=state.passes[index];
        if(!pass.enabled)continue;
        for(std::uint32_t read=0;read<pass.readCount;++read)
        {
            const auto reference=pass.reads[read];
            if(reference.resource>=state.resourceCount)
                return state.FailFormat(FrameGraphStatus::InvalidArgument,"InvalidArgument: pass '%.*s' reads unknown resource",
                    static_cast<int>(pass.name.size()),pass.name.data());
            const auto& resource=state.resources[reference.resource];
            if(reference.version>resource.versionCount)
                return state.FailFormat(FrameGraphStatus::UndefinedRead,
                    "UndefinedRead: pass '%.*s' reads '%.*s' v%u before any producer",
                    static_cast<int>(pass.name.size()),pass.name.data(),static_cast<int>(resource.name.size()),
                    resource.name.data(),reference.version);
            auto version=reference.version;
            auto producer=state.Producer(reference.resource,version);
            while(version>0&&producer!=FrameGraphInvalid&&!state.passes[producer].enabled)
            {
                --version;
                producer=state.Producer(reference.resource,version);
            }
            if(version==0)
            {
                if(!resource.imported)
                    return state.FailFormat(FrameGraphStatus::UndefinedRead,
                        "UndefinedRead: pass '%.*s' reads transient '%.*s' before any write",
                        static_cast<int>(pass.name.size()),pass.name.data(),static_cast<int>(resource.name.size()),
                        resource.name.data());
                pass.resolved[pass.resolvedCount++]={reference.resource,0};
            }
            else pass.resolved[pass.resolvedCount++]={reference.resource,version};
        }
    }

    // 2. 从“有副作用或写外部资源”的 Pass 反向标记存活；其余 Pass 剔除。
    std::uint32_t liveCount=0,queueHead=0,queueTail=0;
    for(std::uint32_t index=0;index<state.passCount;++index)
    {
        auto& pass=state.passes[index];
        if(!pass.enabled)continue;
        bool output=pass.sideEffect;
        for(std::uint32_t write=0;!output&&write<pass.writeCount;++write)
            output=state.resources[pass.writes[write].resource].imported;
        if(output)
        {
            state.live[index]=1;
            state.queue[queueTail++]=index;
            ++liveCount;
        }
    }
    while(queueHead<queueTail)
    {
        const auto index=state.queue[queueHead++];
        const auto& pass=state.passes[index];
        for(std::uint32_t read=0;read<pass.resolvedCount;++read)
        {
            const auto reference=pass.resolved[read];
            if(reference.version==0)continue;
            const auto producer=state.Producer(reference.resource,reference.version);
            if(producer!=FrameGraphInvalid&&!state.live[producer])
            {
                state.live[producer]=1;
                state.queue[queueTail++]=producer;
                ++liveCount;
            }
        }
    }

    // 3. 在存活子图上建 RAW/WAR/WAW 边。
    for(std::uint32_t index=0;index<state.passCount;++index)
    {
        if(!state.live[index])continue;
        const auto& pass=state.passes[index];
        for(std::uint32_t read=0;read<pass.resolvedCount;++read)
        {
            const auto reference=pass.resolved[read];
            const auto& resource=state.resources[reference.resource];
            if(reference.version>0)
            {
                const auto producer=state.Producer(reference.resource,reference.version);
                state.AddEdge(producer,index,reference.resource,reference.version);
            }
            // WAR：读者必须先于下一次写入，否则旧版本会在读取前被覆盖。
            for(auto version=reference.version+1;version<=resource.versionCount;++version)
            {
                const auto writer=state.Producer(reference.resource,version);
                if(writer!=FrameGraphInvalid&&writer!=index&&state.live[writer])
                {
                    state.AddEdge(index,writer,reference.resource,version);
                    break;
                }
            }
        }
        for(std::uint32_t write=0;write<pass.writeCount;++write)
        {
            const auto reference=pass.writes[write];
            for(auto version=reference.version-1;version>0;--version)
            {
                const auto writer=state.Producer(reference.resource,version);
                if(writer!=FrameGraphInvalid&&writer!=index&&state.live[writer])
                {
                    state.AddEdge(writer,index,reference.resource,reference.version);
                    break;
                }
            }
        }
    }

    // 4. 稳定拓扑排序：多个就绪 Pass 时按声明顺序选择，未截断时与手写顺序一致。
    for(std::uint32_t index=0;index<state.edgeCount;++index)
        ++state.indegree[state.edges[index].to];
    std::uint32_t orderedCount=0;
    for(std::uint32_t step=0;step<liveCount;++step)
    {
        std::uint32_t selected=FrameGraphInvalid;
        for(std::uint32_t index=0;index<state.passCount;++index)
            if(state.live[index]&&!state.emitted[index]&&state.indegree[index]==0)
            {
                selected=index;
                break;
            }
        if(selected==FrameGraphInvalid)
        {
            std::size_t used=0;
            state.error[0]='\0';
            used+=std::snprintf(state.error+used,sizeof(state.error)-used,"Cycle: ");
            for(std::uint32_t index=0;index<state.passCount&&used<sizeof(state.error)-32;++index)
                if(state.live[index]&&!state.emitted[index])
                    used+=std::snprintf(state.error+used,sizeof(state.error)-used,"%.*s ",
                        static_cast<int>(state.passes[index].name.size()),state.passes[index].name.data());
            state.status=FrameGraphStatus::Cycle;
            state.stats.compileMilliseconds=std::chrono::duration<double,std::milli>(
                std::chrono::steady_clock::now()-start).count();
            return FrameGraphStatus::Cycle;
        }
        state.emitted[selected]=1;
        state.order[orderedCount++]=selected;
        for(std::uint32_t edge=0;edge<state.edgeCount;++edge)
            if(state.edges[edge].from==selected&&state.indegree[state.edges[edge].to]>0)
                --state.indegree[state.edges[edge].to];
    }
    state.orderCount=orderedCount;
    if(state.passLimit>0&&state.orderCount>state.passLimit)
    {
        state.orderCount=state.passLimit;
        state.stats.clipped=true;
    }
    for(std::uint32_t index=0;index<state.orderCount;++index)
    {
        const auto passIndex=state.order[index];
        if(passIndex==FrameGraphInvalid)continue;
        auto& pass=state.passes[passIndex];
        for(std::uint32_t read=0;read<pass.resolvedCount;++read)
        {
            auto& resource=state.resources[pass.resolved[read].resource];
            resource.used=true;
            if(resource.first==FrameGraphInvalid)resource.first=index;
            resource.last=index;
        }
        for(std::uint32_t write=0;write<pass.writeCount;++write)
        {
            auto& resource=state.resources[pass.writes[write].resource];
            resource.used=true;
            if(resource.first==FrameGraphInvalid)resource.first=index;
            resource.last=index;
        }
    }

    // 5. 生命周期与别名：只对描述相同、区间不重叠的瞬态纹理复用槽位。
    std::uint32_t nextAliasGroup=0;
    for(std::uint32_t resourceIndex=0;resourceIndex<state.resourceCount;++resourceIndex)
    {
        auto& first=state.resources[resourceIndex];
        if(first.aliasGroup!=FrameGraphInvalid||first.imported||!first.used)continue;
        std::uint32_t memberCount=0;
        for(std::uint32_t candidate=resourceIndex;candidate<state.resourceCount;++candidate)
        {
            const auto& resource=state.resources[candidate];
            if(resource.aliasGroup!=FrameGraphInvalid||resource.imported||!resource.used||resource.desc!=first.desc)continue;
            state.group[memberCount++]=candidate;
        }
        // 按首次使用插入排序；同首次使用时保持声明顺序，结果确定。
        for(std::uint32_t index=1;index<memberCount;++index)
        {
            const auto value=state.group[index];
            auto position=index;
            while(position>0&&state.resources[state.group[position-1]].first>state.resources[value].first)
            {
                state.group[position]=state.group[position-1];
                --position;
            }
            state.group[position]=value;
        }
        std::uint32_t slots=0;
        std::uint64_t groupBytes=0;
        for(std::uint32_t member=0;member<memberCount;++member)
        {
            auto& resource=state.resources[state.group[member]];
            std::uint32_t slot=slots;
            for(std::uint32_t candidate=0;candidate<slots;++candidate)
                if(state.slotEnd[candidate]<resource.first)
                {
                    slot=candidate;
                    break;
                }
            if(slot==slots)
            {
                state.slotEnd[slots]=resource.last;
                state.slotBytes[slots]=0;
                ++slots;
            }
            else state.slotEnd[slot]=resource.last;
            state.slotBytes[slot]=(std::max)(state.slotBytes[slot],resource.bytes);
            resource.aliasGroup=nextAliasGroup+slot;
            groupBytes+=resource.bytes;
        }
        std::uint64_t slotTotal=0;
        for(std::uint32_t slot=0;slot<slots;++slot)slotTotal+=state.slotBytes[slot];
        state.stats.transientBytes+=groupBytes;
        state.stats.aliasedBytes+=slotTotal;
        state.stats.aliasGroups+=slots;
        nextAliasGroup+=slots;
    }
    state.stats.savedBytes=state.stats.transientBytes>state.stats.aliasedBytes
        ?state.stats.transientBytes-state.stats.aliasedBytes:0;

    // 6. 编译视图（Pass 视图按执行顺序排列，declaration 保留原始声明下标）。
    for(std::uint32_t orderIndex=0;orderIndex<state.orderCount;++orderIndex)
    {
        auto& view=state.passViews[orderIndex];
        view={};
        const auto& pass=state.passes[state.order[orderIndex]];
        view.name=pass.name;
        view.kind=pass.kind;
        view.color=pass.color;
        view.depth=pass.depth;
        view.input=pass.input;
        view.cacheBindings=pass.cacheBindings;
        view.execute=pass.execute;
        view.declaration=pass.declaration;
        view.reads=std::span<const FrameGraphResourceRef>(pass.resolved,pass.resolvedCount);
        view.writes=std::span<const FrameGraphResourceRef>(pass.writes,pass.writeCount);
    }
    for(std::uint32_t index=0;index<state.resourceCount;++index)
    {
        auto& view=state.textureViews[index];
        view={};
        const auto& resource=state.resources[index];
        view.name=resource.name;
        view.desc=resource.desc;
        view.imported=resource.imported;
        view.firstPass=resource.first;
        view.lastPass=resource.last;
        view.aliasGroup=resource.aliasGroup;
        view.bytes=resource.bytes;
    }

    std::uint32_t disabledCount=0;
    for(std::uint32_t index=0;index<state.passCount;++index)
        if(!state.passes[index].enabled)++disabledCount;
    state.stats.declaredPasses=state.passCount;
    state.stats.executedPasses=state.orderCount;
    state.stats.disabledPasses=disabledCount;
    state.stats.culledPasses=state.passCount-disabledCount-liveCount;
    state.stats.edges=state.edgeCount;
    state.stats.compileMilliseconds=std::chrono::duration<double,std::milli>(
        std::chrono::steady_clock::now()-start).count();
    state.status=FrameGraphStatus::Success;
    state.error[0]='\0';
    return FrameGraphStatus::Success;
}

std::string_view FrameGraph::LastError() const noexcept
{
    return impl_->error;
}

std::span<const FrameGraphPassView> FrameGraph::Passes() const noexcept
{
    return std::span<const FrameGraphPassView>(impl_->passViews,impl_->orderCount);
}

std::span<const FrameGraphTextureView> FrameGraph::Textures() const noexcept
{
    return std::span<const FrameGraphTextureView>(impl_->textureViews,impl_->resourceCount);
}

std::span<const std::uint32_t> FrameGraph::ExecutionOrder() const noexcept
{
    return std::span<const std::uint32_t>(impl_->order,impl_->orderCount);
}

const FrameGraphStats& FrameGraph::Stats() const noexcept
{
    return impl_->stats;
}

bool FrameGraph::WriteDot(const std::filesystem::path& path) const
{
    const auto& state=*impl_;
    std::ostringstream output;
    output<<"digraph FrameGraph {\n  rankdir=LR;\n  node [fontname=\"Consolas\"];\n";
    for(std::uint32_t order=0;order<state.orderCount;++order)
    {
        const auto& pass=state.passes[state.order[order]];
        output<<"  p"<<pass.declaration<<" [label=\""<<EscapeDot(pass.name)<<"\", shape=box, ordering=out];\n";
    }
    for(std::uint32_t index=0;index<state.resourceCount;++index)
    {
        const auto& resource=state.resources[index];
        if(!resource.used)continue;
        output<<"  r"<<index<<" [label=\""<<EscapeDot(resource.name)<<"\\n"
            <<resource.desc.width<<"x"<<resource.desc.height<<" "<<TextureFormatName(resource.desc.format)
            <<(resource.desc.depth?" depth":"")<<"\", shape=ellipse";
        if(resource.aliasGroup!=FrameGraphInvalid)output<<", style=dashed";
        output<<"];\n";
    }
    for(std::uint32_t order=0;order<state.orderCount;++order)
    {
        const auto& pass=state.passes[state.order[order]];
        for(std::uint32_t read=0;read<pass.resolvedCount;++read)
        {
            const auto reference=pass.resolved[read];
            const auto& resource=state.resources[reference.resource];
            if(reference.version==0)
                output<<"  r"<<reference.resource<<" -> p"<<pass.declaration<<" [label=\"external\"];\n";
            else
                output<<"  r"<<reference.resource<<" -> p"<<pass.declaration<<" [label=\"read v"
                    <<reference.version<<"\"];\n";
            (void)resource;
        }
        for(std::uint32_t write=0;write<pass.writeCount;++write)
        {
            const auto reference=pass.writes[write];
            output<<"  p"<<pass.declaration<<" -> r"<<reference.resource<<" [label=\"write v"
                <<reference.version<<"\"];\n";
        }
    }
    // 同一别名槽的纹理用虚线相连，便于在图上看到“复用但没有数据依赖”。
    for(std::uint32_t first=0;first<state.resourceCount;++first)
        if(state.resources[first].aliasGroup!=FrameGraphInvalid)
            for(std::uint32_t second=first+1;second<state.resourceCount;++second)
                if(state.resources[second].aliasGroup==state.resources[first].aliasGroup)
                    output<<"  r"<<first<<" -> r"<<second<<" [style=dashed, color=gray, label=\"alias\"];\n";
    output<<"  // passes="<<state.stats.declaredPasses<<", executed="<<state.stats.executedPasses
        <<", culled="<<state.stats.culledPasses<<", aliasGroups="<<state.stats.aliasGroups
        <<", saved="<<state.stats.savedBytes<<" B\n}\n";
    std::ofstream file(path,std::ios::binary);
    if(!file)return false;
    file<<output.str();
    return file.good();
}

bool FrameGraph::WriteJson(const std::filesystem::path& path) const
{
    const auto& state=*impl_;
    std::ostringstream output;
    output<<"{\n  \"passes\": [\n";
    for(std::uint32_t order=0;order<state.orderCount;++order)
    {
        const auto& pass=state.passes[state.order[order]];
        if(order)output<<",\n";
        output<<"    {\"order\":"<<order<<",\"declaration\":"<<pass.declaration
            <<",\"name\":\""<<EscapeJson(pass.name)<<"\",\"kind\":\""<<PassKindName(pass.kind)
            <<"\",\"color\":\""<<ResourceUseName(pass.color)<<"\",\"depth\":\""<<ResourceUseName(pass.depth)
            <<"\",\"input\":\""<<ResourceUseName(pass.input)<<"\",\"execute\":"<<pass.execute<<",\"reads\":[";
        for(std::uint32_t read=0;read<pass.resolvedCount;++read)
        {
            if(read)output<<",";
            const auto reference=pass.resolved[read];
            output<<"{\"resource\":\""<<EscapeJson(state.resources[reference.resource].name)
                <<"\",\"version\":"<<reference.version<<"}";
        }
        output<<"],\"writes\":[";
        for(std::uint32_t write=0;write<pass.writeCount;++write)
        {
            if(write)output<<",";
            const auto reference=pass.writes[write];
            output<<"{\"resource\":\""<<EscapeJson(state.resources[reference.resource].name)
                <<"\",\"version\":"<<reference.version<<"}";
        }
        output<<"]}";
    }
    output<<"\n  ],\n  \"resources\": [\n";
    for(std::uint32_t index=0;index<state.resourceCount;++index)
    {
        const auto& resource=state.resources[index];
        if(index)output<<",\n";
        output<<"    {\"name\":\""<<EscapeJson(resource.name)<<"\",\"imported\":"<<(resource.imported?"true":"false")
            <<",\"width\":"<<resource.desc.width<<",\"height\":"<<resource.desc.height
            <<",\"mips\":"<<resource.desc.mipLevels<<",\"format\":\""<<TextureFormatName(resource.desc.format)
            <<"\",\"depth\":"<<(resource.desc.depth?"true":"false")<<",\"bytes\":"<<resource.bytes
            <<",\"first\":"<<(resource.first==FrameGraphInvalid?-1:static_cast<int>(resource.first))
            <<",\"last\":"<<(resource.last==FrameGraphInvalid?-1:static_cast<int>(resource.last))
            <<",\"aliasGroup\":"<<(resource.aliasGroup==FrameGraphInvalid?-1:static_cast<int>(resource.aliasGroup))<<"}";
    }
    output<<"\n  ],\n  \"stats\": {\"declaredPasses\":"<<state.stats.declaredPasses
        <<",\"executedPasses\":"<<state.stats.executedPasses<<",\"culledPasses\":"<<state.stats.culledPasses
        <<",\"disabledPasses\":"<<state.stats.disabledPasses<<",\"edges\":"<<state.stats.edges
        <<",\"aliasGroups\":"<<state.stats.aliasGroups<<",\"transientBytes\":"<<state.stats.transientBytes
        <<",\"aliasedBytes\":"<<state.stats.aliasedBytes<<",\"savedBytes\":"<<state.stats.savedBytes
        <<",\"clipped\":"<<(state.stats.clipped?"true":"false")
        <<",\"compileMilliseconds\":"<<state.stats.compileMilliseconds<<"}\n}\n";
    std::ofstream file(path,std::ios::binary);
    if(!file)return false;
    file<<output.str();
    return file.good();
}
}
