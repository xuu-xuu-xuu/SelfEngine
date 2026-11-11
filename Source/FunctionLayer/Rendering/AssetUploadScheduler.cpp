#include "AssetUploadScheduler.h"
#include "ResourceLayer/Assets/AssetIO.h"
#include "CoreLayer/Profiler.h"
#include <algorithm>
#include <cstddef>
#include <map>
#include <stdexcept>
#include <thread>

namespace se
{
namespace
{
bool Valid(AssetUploadBudget budget){return budget.bytes>=512&&budget.bytes<=64ull*1024*1024&&budget.tasks&&budget.tasks<=65536;}
struct Work
{
    std::string name;
    rhi::Buffer* buffer=nullptr;
    rhi::BufferDescription bufferDescription;
    std::span<const std::byte> bytes;
    const ImageData* image=nullptr;
    std::vector<rhi::Texture*> textures;
    std::uint64_t offset=0;
};
struct Candidate
{
    AssetUploadRequest request;
    std::shared_ptr<RenderImageAsset> image;
    std::shared_ptr<StaticRenderModel> model;
    std::vector<Work> work;
    std::vector<AssetDiagnostic> diagnostics;
    std::size_t cursor=0;
    bool prepared=false,started=false,failed=false,cancelled=false,done=false,touched=false;
    rhi::CompletionToken last;
};
}
struct AssetUploadScheduler::Impl
{
    rhi::Device& device;AsyncAssetLoader& resources;AssetUploadBudget budget;Profiler* profiler;
    const std::thread::id owner=std::this_thread::get_id();std::vector<std::unique_ptr<Candidate>> pending;std::size_t cursor=0;bool closed=false;
    Impl(rhi::Device& d,AsyncAssetLoader& r,AssetUploadBudget b,Profiler* p):device(d),resources(r),budget(b),profiler(p)
    {if(!Valid(b))throw std::invalid_argument("Assets.UploadInvalidBudget");}
    void CheckOwner()const{if(std::this_thread::get_id()!=owner)throw std::logic_error("Assets.AsyncWrongThread");}
    void Fail(Candidate& candidate,const char* key,AssetUploadStep& step)
    {
        asset_io::Diagnostic(candidate.diagnostics,key,candidate.request.id);
        resources.CompleteUpload({candidate.request.token,false,{},std::move(candidate.diagnostics)});candidate.failed=true;++step.failed;
    }
    bool Prepare(Candidate& c)
    {
        const auto& data=*c.request.cpu;
        if(data.kind==AsyncAssetKind::Image)
        {
            if(!data.image||data.imageKey.empty())return false;
            c.image=std::make_shared<RenderImageAsset>();Work work;work.name=c.request.id;work.image=data.image.get();work.textures.push_back(&c.image->texture);c.work.push_back(std::move(work));
        }
        else
        {
            if(!data.model||!data.modelLayout||!data.modelImages)return false;
            static_assert(sizeof(MeshVertex)==sizeof(rhi::Vertex)&&offsetof(MeshVertex,normal)==offsetof(rhi::Vertex,normal)&&offsetof(MeshVertex,uv)==offsetof(rhi::Vertex,uv)&&offsetof(MeshVertex,tangent)==offsetof(rhi::Vertex,tangent));
            c.model=std::make_shared<StaticRenderModel>();c.model->source=data.model;c.model->layout=*data.modelLayout;c.model->meshes.resize(data.model->meshes.size());c.model->fallbackMeshes.resize(data.model->meshes.size());
            std::map<std::string,std::size_t> textures;
            const auto addTexture=[&](const ImageData& image,const std::string& key,rhi::Texture& target,const std::string& name)
            {
                auto found=textures.find(key);
                if(found==textures.end()){Work work;work.name=name;work.image=&image;work.textures.push_back(&target);textures.emplace(key,c.work.size());c.work.push_back(std::move(work));}
                else c.work[found->second].textures.push_back(&target);
            };
            for(std::size_t index=0;index<data.model->meshes.size();++index)
            {
                const auto& source=data.model->meshes[index];auto& mesh=c.model->meshes[index];ModelMaterialImages selected;
                if(!SelectModelMaterialImages(*data.model,*data.modelImages,index,selected))return false;
                auto material=std::make_shared<PbrRenderMaterial>();ConfigurePbrMaterial(selected.material,*material,source.name,c.diagnostics);material->normalMapping=selected.normalMapping;mesh.material=material;
                for(std::size_t channel=0;channel<3;++channel)if(selected.fallback[channel]){c.model->state=ModelRenderState::Fallback;asset_io::Diagnostic(c.diagnostics,"Rendering.FallbackMaterial",source.name,true);}
                c.model->fallbackMeshes[index]=selected.fallback[0]||selected.fallback[1]||selected.fallback[2];
                Work vertices;vertices.name=source.name+".vertices";vertices.buffer=&mesh.vertices;vertices.bufferDescription={rhi::BufferUsage::Vertex,sizeof(rhi::Vertex),vertices.name};vertices.bytes=std::as_bytes(std::span(source.vertices));c.work.push_back(std::move(vertices));
                Work indices;indices.name=source.name+".indices";indices.buffer=&mesh.indices;indices.bufferDescription={rhi::BufferUsage::Index,sizeof(std::uint32_t),indices.name};indices.bytes=std::as_bytes(std::span(source.indices));c.work.push_back(std::move(indices));
                addTexture(*selected.images[0],selected.keys[0],mesh.texture,source.name+".base");addTexture(*selected.images[0],selected.keys[0],material->baseColor,source.name+".base");
                addTexture(*selected.images[1],selected.keys[1],material->normal,source.name+".normal");addTexture(*selected.images[2],selected.keys[2],material->metallicRoughness,source.name+".metallicRoughness");
            }
        }
        c.prepared=true;return !c.work.empty();
    }
};
AssetUploadScheduler::AssetUploadScheduler(rhi::Device& device,AsyncAssetLoader& resources,AssetUploadBudget budget,Profiler* profiler)
    :impl_(std::make_unique<Impl>(device,resources,budget,profiler)){}
AssetUploadScheduler::~AssetUploadScheduler(){Shutdown();}
bool AssetUploadScheduler::SetBudget(AssetUploadBudget budget)
{impl_->CheckOwner();if(!Valid(budget)||impl_->closed)return false;impl_->budget=budget;return true;}
rhi::Result AssetUploadScheduler::Tick(AssetUploadStep& output)
{
    auto& p=*impl_;p.CheckOwner();output={};if(p.closed)return {rhi::Error::InvalidState};CpuScope scope(p.profiler,"CPU.AssetUpload");
    for(auto& candidate:p.pending)
    {
        auto& c=*candidate;c.touched=false;
        if(!p.resources.IsUploadCurrent(c.request.token)){c.cancelled=true;++output.cancelled;continue;}
        if(c.prepared&&c.cursor==c.work.size()&&c.last.value&&p.device.IsComplete(c.last))
        {
            std::shared_ptr<const RenderAssetLease> lease;
            if(c.image)lease=c.image;else{auto model=std::make_shared<RenderModelAsset>();model->model=c.model;lease=std::move(model);}
            if(p.resources.CompleteUpload({c.request.token,true,std::move(lease),std::move(c.diagnostics)})){c.done=true;++output.completed;}
            else{c.cancelled=true;++output.cancelled;}
        }
    }
    std::erase_if(p.pending,[](const auto& c){return c->done||c->cancelled||c->failed;});
    std::size_t admitted=0;
    for(auto& request:p.resources.QueuedUploads())
    {
        if(admitted==p.budget.tasks)break;
        if(std::any_of(p.pending.begin(),p.pending.end(),[&](const auto& c){return c->request.token==request.token;}))continue;
        auto candidate=std::make_unique<Candidate>();candidate->request=std::move(request);p.pending.push_back(std::move(candidate));++admitted;
    }
    bool batch=false;rhi::Result fatal{};
    struct CloseBatch{rhi::Device& device;bool& active;~CloseBatch(){if(active){rhi::CompletionToken ignored;device.EndUploadBatch(ignored);}}} close{p.device,batch};
    std::size_t skipped=0;
    while(!p.pending.empty()&&output.tasks<p.budget.tasks&&skipped<p.pending.size())
    {
        auto& c=*p.pending[p.cursor++%p.pending.size()];
        if(c.failed||c.cancelled||c.done||!p.resources.IsUploadCurrent(c.request.token)){++skipped;continue;}
        if(!c.prepared)
        {
            try{if(!p.Prepare(c)){p.Fail(c,"Assets.AsyncInvalidCandidate",output);++skipped;continue;}}
            catch(const std::exception&){p.Fail(c,"Assets.AsyncTaskFailed",output);++skipped;continue;}
        }
        if(c.cursor==c.work.size()){++skipped;continue;}
        auto& work=c.work[c.cursor];const auto remaining=p.budget.bytes-output.footprintBytes;rhi::UploadFootprint footprint;std::uint64_t amount=0;
        rhi::Result result;
        if(work.buffer)
        {
            rhi::UploadFootprint minimum;result=p.device.QueryBufferUploadFootprint(work.bufferDescription.stride,minimum);
            if(!result){p.Fail(c,result.Key(),output);++skipped;continue;}
            if(minimum.stagingBytes>p.budget.bytes){p.Fail(c,"Assets.UploadBudgetTooSmall",output);++skipped;continue;}
            const auto available=remaining/minimum.alignment*minimum.alignment;
            amount=std::min<std::uint64_t>(work.bytes.size()-work.offset,available);amount=amount/work.bufferDescription.stride*work.bufferDescription.stride;
            if(!amount){++skipped;continue;}result=p.device.QueryBufferUploadFootprint(amount,footprint);
        }
        else
        {
            rhi::TextureDescription description{work.image->width,work.image->height};rhi::UploadFootprint minimum;result=p.device.QueryTextureUploadFootprint(description,1,minimum);
            if(!result){p.Fail(c,result.Key(),output);++skipped;continue;}
            if(minimum.stagingBytes>p.budget.bytes){p.Fail(c,"Assets.UploadBudgetTooSmall",output);++skipped;continue;}
            const auto available=remaining/minimum.alignment*minimum.alignment;
            amount=std::min<std::uint64_t>(work.image->height-work.offset,available/minimum.rowPitch);
            if(!amount){++skipped;continue;}result=p.device.QueryTextureUploadFootprint(description,static_cast<std::uint32_t>(amount),footprint);
        }
        if(!result||footprint.stagingBytes>remaining){p.Fail(c,result?"Assets.UploadBudgetTooSmall":result.Key(),output);++skipped;continue;}
        if(!c.started){if(!p.resources.BeginUpload(c.request.token)){c.cancelled=true;++output.cancelled;++skipped;continue;}c.started=true;}
        if(work.buffer&&!*work.buffer){result=p.device.CreateStagedBuffer(work.bufferDescription,work.bytes.size(),*work.buffer);if(result)++output.creations;}
        else if(work.image&&!*work.textures[0])
        {
            rhi::Texture texture;result=p.device.CreateStagedTexture({work.image->width,work.image->height,1,1,rhi::TextureFormat::Rgba8,work.name},texture);
            if(result){for(auto* target:work.textures)*target=texture;++output.creations;}
        }
        if(!result){p.Fail(c,result.Key(),output);++skipped;continue;}
        if(!batch){result=p.device.BeginUploadBatch();if(!result){fatal=result;break;}batch=true;}
        if(work.buffer)result=p.device.UploadBufferRange(**work.buffer,work.offset,work.bytes.subspan(static_cast<size_t>(work.offset),static_cast<size_t>(amount)));
        else{const auto row=size_t(work.image->width)*4;result=p.device.UploadTextureRows(**work.textures[0],static_cast<std::uint32_t>(work.offset),static_cast<std::uint32_t>(amount),std::span(work.image->rgba8).subspan(size_t(work.offset)*row,size_t(amount)*row));}
        if(!result){p.Fail(c,result.Key(),output);++skipped;continue;}
        work.offset+=amount;c.touched=true;output.payloadBytes+=footprint.payloadBytes;output.footprintBytes+=footprint.stagingBytes;++output.tasks;skipped=0;
        if(work.offset==(work.buffer?work.bytes.size():work.image->height))++c.cursor;
    }
    if(batch)
    {
        rhi::CompletionToken token;const auto result=p.device.EndUploadBatch(token);batch=false;if(!result)return result;
        output.submitted=true;for(auto& c:p.pending)if(c->touched)c->last=token;
    }
    std::erase_if(p.pending,[](const auto& c){return c->done||c->cancelled||c->failed;});output.pending=p.pending.size();return fatal;
}
void AssetUploadScheduler::Shutdown()
{
    auto& p=*impl_;p.CheckOwner();if(p.closed)return;p.closed=true;
    for(auto& c:p.pending)if(p.resources.IsUploadCurrent(c->request.token))p.resources.CompleteUpload({c->request.token,false,{},{{"Assets.UploadSchedulerClosed",c->request.id,false}}});
    p.pending.clear();
}
}
