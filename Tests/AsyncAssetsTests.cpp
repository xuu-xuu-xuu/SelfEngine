#include "ResourceLayer/Assets/AsyncAssetLoader.h"
#include "ResourceLayer/Assets/AssetIO.h"
#include <nlohmann/json.hpp>
#include <cstring>
#include <fstream>
#include <iostream>
#include <stdexcept>

using namespace se;
using namespace std::chrono_literals;
namespace fs = std::filesystem;
namespace
{
void Check(bool value, const char* message) { if (!value) throw std::runtime_error(message); }
bool Has(const AssetView& view, const char* key) { return std::any_of(view.diagnostics.begin(), view.diagnostics.end(), [&](const auto& d) { return d.key == key; }); }
struct Gate
{
    std::mutex mutex; std::condition_variable changed; bool entered = false, released = false;
    void Block() { std::unique_lock lock(mutex); entered = true; changed.notify_all(); Check(changed.wait_for(lock, 5s, [&] { return released; }), "controlled decode timed out"); }
    void Await() { std::unique_lock lock(mutex); Check(changed.wait_for(lock, 5s, [&] { return entered; }), "decoder did not enter gate"); }
    void Open() { std::lock_guard lock(mutex); released = true; changed.notify_all(); }
};
template<class Predicate> void Until(AsyncAssetLoader& loader, TaskQueue& tasks, Predicate ready)
{
    for (unsigned step = 0; step < 32; ++step)
    {
        loader.Pump(); loader.Pump(); if (ready()) return;
        Check(tasks.Pending() != 0, "no CPU operation can make the expected state progress");
        tasks.WaitForReady(2s);
    }
    throw std::runtime_error("resource state did not progress within bounded waits");
}
void Drain(TaskQueue& tasks)
{
    const auto deadline = std::chrono::steady_clock::now() + 10s;
    while (tasks.Pending()) { Check(std::chrono::steady_clock::now() < deadline, "task drain timed out"); tasks.WaitForReady(500ms); tasks.Pump(); }
}
void WriteBmp(const fs::path& path, std::uint8_t red, std::uint8_t blue)
{
    std::vector<std::uint8_t> bytes(70);
    const auto u16 = [&](size_t at, std::uint16_t value) { bytes[at] = std::uint8_t(value); bytes[at+1] = std::uint8_t(value >> 8); };
    const auto u32 = [&](size_t at, std::uint32_t value) { for (unsigned index = 0; index < 4; ++index) bytes[at+index] = std::uint8_t(value >> (index*8)); };
    bytes[0]='B'; bytes[1]='M'; u32(2,70); u32(10,54); u32(14,40); u32(18,2); u32(22,2); u16(26,1); u16(28,24); u32(34,16);
    for(size_t row=0;row<2;++row)for(size_t column=0;column<2;++column){const size_t at=54+row*8+column*3;bytes[at]=blue;bytes[at+2]=red;}
    Check(asset_io::WriteAtomic(path,bytes),"write independent BMP fixture");
}
void SameModel(const StaticModel& a, const StaticModel& b)
{
    Check(a.meshes.size()==b.meshes.size()&&a.nodes.size()==b.nodes.size()&&a.materials.size()==b.materials.size()&&a.textures.size()==b.textures.size()&&a.dependencies==b.dependencies,"model shape differs from synchronous cache");
    for(size_t i=0;i<a.meshes.size();++i){const auto& x=a.meshes[i];const auto& y=b.meshes[i];Check(x.vertices.size()==y.vertices.size()&&x.indices==y.indices&&x.material==y.material,"model mesh indices/material");for(size_t v=0;v<x.vertices.size();++v)Check(std::memcmp(&x.vertices[v],&y.vertices[v],sizeof(MeshVertex))==0,"canonical vertex differs");}
    for(size_t i=0;i<a.nodes.size();++i)Check(a.nodes[i].parent==b.nodes[i].parent&&a.nodes[i].localMatrix==b.nodes[i].localMatrix&&a.nodes[i].meshes==b.nodes[i].meshes,"model hierarchy differs");
    for(size_t i=0;i<a.materials.size();++i)Check(a.materials[i].baseColor==b.materials[i].baseColor&&a.materials[i].normalScale==b.materials[i].normalScale&&a.materials[i].normalTexture==b.materials[i].normalTexture,"model material differs");
}
struct Lease : RenderAssetLease
{
    AsyncAssetKind kind; explicit Lease(AsyncAssetKind value=AsyncAssetKind::Image):kind(value){}
    AsyncAssetKind Kind() const noexcept override { return kind; }
};
void SharedStates(AssetStore& assets, const AssetId& image)
{
    TaskQueue tasks(2,16); Gate gate; const auto owner=std::this_thread::get_id(); std::atomic_uint calls=0;
    AsyncAssetLoader loader(assets,tasks,{},[&](const auto& source,auto kind,const auto& token){Check(std::this_thread::get_id()!=owner,"decoder ran on Request thread");++calls;auto result=LoadCpuAssetSnapshot(source,kind,token);gate.Block();return result;});
    const auto begin=std::chrono::steady_clock::now();auto cpu=loader.Request(image,AsyncAssetKind::Image);auto render=loader.Request(image,AsyncAssetKind::Image,AssetTarget::Renderable);auto duplicate=loader.Request(image,AsyncAssetKind::Image,AssetTarget::Renderable);
    Check(std::chrono::steady_clock::now()-begin<3s,"Request blocked on decoding");gate.Await();
    Check(calls==1&&loader.Stats().operations==1&&loader.Stats().subscriptions==3,"duplicate targets share one CPU operation");
    for(unsigned i=0;i<100;++i){loader.Pump(0);Check(loader.Query(cpu).state==AssetLoadState::LoadingCPU,"blocked worker cannot publish");}
    duplicate.Cancel();loader.CollectUnused();Check(loader.Query(duplicate).state==AssetLoadState::Cancelled&&loader.Stats().subscriptions==2,"one cancellation preserves other clients");
    gate.Open();Check(tasks.WaitForReady(5s),"CPU result notification");loader.Pump(1);
    Check(loader.Query(cpu).state==AssetLoadState::CpuReady&&loader.Query(render).state==AssetLoadState::CpuReady,"CPU ready is observable before publication advance");
    loader.Pump(1);Check(loader.Query(cpu).state==AssetLoadState::Ready&&loader.Query(render).state==AssetLoadState::UploadQueued&&!loader.Query(render).gpu,"CPU ready does not imply GPU ready");
    auto uploads=loader.QueuedUploads();Check(uploads.size()==1&&uploads[0].cpu==loader.Query(cpu).cpu,"single upload shares immutable CPU candidate");
    auto token=uploads[0].token;auto lease=std::make_shared<Lease>();
    Check(!loader.CompleteUpload({token,true,lease,{}}),"success before BeginUpload rejected");
    Check(loader.BeginUpload(token)&&!loader.BeginUpload(token),"upload begins exactly once");
    for(unsigned field=0;field<4;++field){auto wrong=token;if(field==0)++wrong.session;else if(field==1)++wrong.operation;else if(field==2)++wrong.revision;else ++wrong.attempt;Check(!loader.CompleteUpload({wrong,true,lease,{}}),"full correlation token required");}
    Check(!loader.CompleteUpload({token,true,std::make_shared<Lease>(AsyncAssetKind::Model),{}}),"wrong resource type rejected");
    Check(loader.CompleteUpload({token,true,lease,{}})&&!loader.CompleteUpload({token,true,lease,{}}),"receipt accepted once");
    Check(loader.Query(render).state==AssetLoadState::UploadingGPU,"receipt does not directly publish Ready");loader.Pump();
    Check(loader.Query(render).state==AssetLoadState::Ready&&loader.Query(render).gpu==lease&&loader.Query(cpu).gpu==nullptr,"owner publishes target-specific Ready");
    render.Cancel();loader.CollectUnused();auto replacement=loader.Request(image,AsyncAssetKind::Image,AssetTarget::Renderable);loader.Pump();auto fresh=loader.QueuedUploads();
    Check(fresh.size()==1&&fresh[0].token.operation==token.operation&&fresh[0].token.attempt!=token.attempt&&calls==1,"new render attempt reuses CPU but replaces upload identity");
    Check(!loader.CompleteUpload({token,true,lease,{}}),"previous attempt cannot revive new subscription");
    Check(loader.CompleteUpload({fresh[0].token,false,{},{{"Assets.AsyncUploadFailed","controlled backend failure",false}}}),"failure before GPU start is reportable");loader.Pump();
    Check(loader.Query(replacement).state==AssetLoadState::Failed&&Has(loader.Query(replacement),"Assets.AsyncUploadFailed")&&loader.Query(cpu).state==AssetLoadState::Ready,"GPU failure preserves CPU-only availability");
    AsyncAssetLoader other(assets,tasks);Check(other.Query(cpu).state==AssetLoadState::Failed&&Has(other.Query(cpu),"Assets.AsyncInvalidTicket"),"cross-manager ticket rejected");
    std::atomic_uint wrongThread=0;std::thread wrong([&]{try{loader.Query(cpu);}catch(const std::logic_error&){++wrongThread;}try{loader.Pump();}catch(const std::logic_error&){++wrongThread;}try{loader.Request(image,AsyncAssetKind::Image);}catch(const std::logic_error&){++wrongThread;}replacement.Cancel();});wrong.join();
    Check(wrongThread==3&&loader.Query(replacement).state==AssetLoadState::Cancelled,"owner guards and cross-thread cancel");
    auto cancelAfterReceipt=loader.Request(image,AsyncAssetKind::Image,AssetTarget::Renderable);loader.Pump();auto pending=loader.QueuedUploads();
    auto owned=std::make_shared<Lease>();std::weak_ptr<Lease> weak=owned;
    Check(pending.size()==1&&loader.BeginUpload(pending[0].token)&&loader.CompleteUpload({pending[0].token,true,owned,{}}),"pending receipt for last-demand cancellation");
    owned.reset();cancelAfterReceipt.Cancel();loader.Pump();Check(weak.expired()&&loader.Query(cpu).state==AssetLoadState::Ready,"cancelled receipt drops candidate lease without harming CPU client");
}
void LateResults(AssetStore& assets,const AssetId& image,const fs::path& path)
{
    WriteBmp(path,255,0);TaskQueue tasks(2,8);Gate old;std::atomic_uint calls=0;
    AsyncAssetLoader loader(assets,tasks,{},[&](const auto& source,auto kind,const auto& token){auto result=LoadCpuAssetSnapshot(source,kind,token);if(++calls==1)old.Block();return result;});
    auto first=loader.Request(image,AsyncAssetKind::Image);old.Await();const auto firstOperation=loader.Query(first).operation;first.Cancel();loader.CollectUnused();WriteBmp(path,0,255);
    auto second=loader.Request(image,AsyncAssetKind::Image);Until(loader,tasks,[&]{return loader.Query(second).state==AssetLoadState::Ready;});
    auto view=loader.Query(second);Check(view.operation!=firstOperation&&view.cpu->image->rgba8[0]==0&&view.cpu->image->rgba8[2]==255,"new request receives new candidate");
    old.Open();Drain(tasks);loader.Pump();Check(loader.Query(second).cpu==view.cpu&&loader.Query(first).state==AssetLoadState::Cancelled,"late old CPU result cannot replace new revision");
    Gate closing;auto late=std::make_unique<AsyncAssetLoader>(assets,tasks,AsyncAssetLimits{},[&](const auto& source,auto kind,const auto& token){auto result=LoadCpuAssetSnapshot(source,kind,token);closing.Block();return result;});
    auto closingTicket=late->Request(image,AsyncAssetKind::Image);closing.Await();late->Shutdown();Check(late->Query(closingTicket).state==AssetLoadState::Cancelled&&late->Stats().operations==0,"shutdown invalidates session");late.reset();closing.Open();Drain(tasks);
    Check(loader.Query(second).cpu==view.cpu,"destroyed session cannot publish through another loader");
}
void BudgetsAndFailures(AssetStore& assets,const AssetId& image,const AssetId& second)
{
    TaskQueue tasks(2,16);AsyncAssetLoader loader(assets,tasks,{4,8,16});auto ticket=loader.Request(image,AsyncAssetKind::Image);Until(loader,tasks,[&]{return loader.Query(ticket).state==AssetLoadState::Ready;});
    auto held=loader.Query(ticket);auto leaf=held.cpu->image;Check(held.cpu->PayloadBytes()==16&&loader.Stats().cpuBytes==16,"retained payload counted");ticket.Cancel();loader.CollectUnused();
    Check(loader.Stats().operations==0&&loader.Stats().cpuBytes==16,"external old view keeps budget reservation");
    auto failed=loader.Request(second,AsyncAssetKind::Image);Until(loader,tasks,[&]{return loader.Query(failed).state==AssetLoadState::Failed;});
    Check(Has(loader.Query(failed),"Assets.AsyncCpuBudget")&&loader.Stats().peakCpuBytes==16,"CPU budget refuses candidate without hiding high-water");
    const auto starts=loader.Stats().decodeStarts;auto duplicate=loader.Request(second,AsyncAssetKind::Image);Check(loader.Query(duplicate).state==AssetLoadState::Failed&&loader.Stats().decodeStarts==starts,"failed operation does not auto-retry");
    held={};Check(loader.Stats().cpuBytes==16,"child image alias also retains revision budget");leaf.reset();failed.Cancel();duplicate.Cancel();loader.CollectUnused();Check(loader.Stats().cpuBytes==0,"last view releases old reservation");
    auto retry=loader.Request(second,AsyncAssetKind::Image);Until(loader,tasks,[&]{return loader.Query(retry).state==AssetLoadState::Ready;});Check(loader.Stats().cpuBytes==16,"explicit release then retry recovers");
    AsyncAssetLoader limited(assets,tasks,{1,2,1024});auto a=limited.Request(image,AsyncAssetKind::Image);auto b=limited.Request(image,AsyncAssetKind::Image);
    Check(!limited.Request(second,AsyncAssetKind::Image).Valid()&&!limited.Request(image,AsyncAssetKind::Image).Valid()&&limited.Stats().subscriptions==2,"operation and subscription bounds");
    AsyncAssetLoader operations(assets,tasks,{1,8,1024});auto existing=operations.Request(image,AsyncAssetKind::Image);Check(!operations.Request(second,AsyncAssetKind::Image).Valid(),"operation count independently bounded");
    AsyncAssetLoader throwing(assets,tasks,{},[](const auto&,auto,const auto&) -> CpuAssetResult {throw std::runtime_error("decoder-original");});auto error=throwing.Request(image,AsyncAssetKind::Image);Until(throwing,tasks,[&]{return throwing.Query(error).state==AssetLoadState::Failed;});
    Check(Has(throwing.Query(error),"Assets.AsyncTaskFailed")&&throwing.Query(error).diagnostics.back().detail=="decoder-original","worker exception reaches resource diagnostics");
    Check(!throwing.Request("unknown",AsyncAssetKind::Image).Valid(),"unknown catalog identity rejected");
    Gate full;TaskQueue busy(1,1);busy.Submit([&](const auto&){full.Block();return TaskQueue::Completion{};});full.Await();AsyncAssetLoader rejected(assets,busy);auto denied=rejected.Request(image,AsyncAssetKind::Image);
    Check(rejected.Query(denied).state==AssetLoadState::Failed&&Has(rejected.Query(denied),"Assets.AsyncTaskRejected")&&rejected.Stats().decodeStarts==0,"shared task queue saturation propagates");full.Open();Drain(busy);
    TaskQueue closed(1,2);AsyncAssetLoader closedLoader(assets,closed);auto cancelled=closedLoader.Request(image,AsyncAssetKind::Image);closed.Shutdown();closedLoader.Pump();Check(closedLoader.Query(cancelled).state==AssetLoadState::Failed,"external queue shutdown cannot leave LoadingCPU stuck");
}
void RealModels(AssetStore& assets,const std::vector<AssetId>& models,const AssetId& png)
{
    TaskQueue tasks(2,16);AsyncAssetLoader loader(assets,tasks);std::vector<AssetTicket> tickets;
    for(const auto& id:models)tickets.push_back(loader.Request(id,AsyncAssetKind::Model));auto image=loader.Request(png,AsyncAssetKind::Image);
    Until(loader,tasks,[&]{return std::all_of(tickets.begin(),tickets.end(),[&](const auto& ticket){return loader.Query(ticket).state==AssetLoadState::Ready;})&&loader.Query(image).state==AssetLoadState::Ready;});
    std::vector<AssetDiagnostic> diagnostics;for(size_t i=0;i<models.size();++i){auto sync=assets.LoadModel(models[i],diagnostics);auto view=loader.Query(tickets[i]);Check(bool(sync)&&view.cpu&&view.cpu->model&&!view.gpu,"real model CPU ready");SameModel(*sync,*view.cpu->model);Check(view.cpu->modelImages->images.size()==sync->textures.size(),"model images prepared in CPU bundle");}
    auto syncImage=assets.LoadTextureImage(png,diagnostics);Check(syncImage&&syncImage->rgba8==loader.Query(image).cpu->image->rgba8,"real PNG matches synchronous image");
    ImageData unchanged{77,1,{1,2,3,4}};Check(!DecodeImage(assets.Resolve(png),unchanged,diagnostics,0)&&unchanged.width==77&&unchanged.rgba8.size()==4,"decoder rejects budget before replacing caller output");
    Check(!assets.LoadTextureImage(png,diagnostics,0),"existing CPU image cache cannot bypass a smaller limit");
    auto render=loader.Request(models[0],AsyncAssetKind::Model,AssetTarget::Renderable);loader.Pump();auto requests=loader.QueuedUploads();Check(requests.size()==1,"model dependency-ready candidate queued");
    Check(loader.BeginUpload(requests[0].token)&&loader.CompleteUpload({requests[0].token,true,std::make_shared<Lease>(AsyncAssetKind::Model),{}}),"controlled model port receipt");loader.Pump();Check(loader.Query(render).state==AssetLoadState::Ready,"controlled model Ready contract, not real GPU proof");
    auto placeholder=AsyncAssetLoader::Placeholder(AsyncAssetKind::Image);auto cube=AsyncAssetLoader::Placeholder(AsyncAssetKind::Model);Check(placeholder->image->rgba8.size()==16&&ValidateStaticModel(*cube->model),"explicit placeholders are valid CPU data");
    AssetSourceSnapshot snapshot;Check(assets.DescribeSource(models[0],snapshot,diagnostics),"immutable source descriptor");StaticModel escaped=*loader.Query(tickets[0]).cpu->model;escaped.textures.push_back({"../outside.png","",{},false});PreparedModelImages output;output.images.push_back({77,1,{}});
    Check(!PrepareModelImages(escaped,snapshot.source,snapshot.sourceRoot,output,diagnostics)&&output.images.size()==1&&output.images[0].width==77,"path rejection preserves caller image output");
    Check(!PrepareModelImages(*loader.Query(tickets[0]).cpu->model,snapshot.source,snapshot.sourceRoot,output,diagnostics,0)&&output.images.size()==1&&output.images[0].width==77,"cumulative model image limit is an atomic failure");
    AsyncAssetLoader smallBudget(assets,tasks,{4,8,16});auto tooLarge=smallBudget.Request(models[0],AsyncAssetKind::Model);Until(smallBudget,tasks,[&]{return smallBudget.Query(tooLarge).state==AssetLoadState::Failed;});Check(Has(smallBudget.Query(tooLarge),"Assets.AsyncCpuBudget")&&smallBudget.Stats().cpuBytes==0,"default model decoder stops before preparing over-budget image bundle");
}
void InvalidSources(AssetStore& assets,const AssetId& model,const AssetId& png,const fs::path& source)
{
    std::vector<std::uint8_t> original;Check(asset_io::Read(source/"checker.png",original),"save texture fixture");
    const std::vector<std::uint8_t> broken{1,2,3,4};Check(asset_io::WriteAtomic(source/"checker.png",broken),"corrupt only test copy");
    TaskQueue tasks(2,16);AsyncAssetLoader loader(assets,tasks);auto cpu=loader.Request(model,AsyncAssetKind::Model);auto render=loader.Request(model,AsyncAssetKind::Model,AssetTarget::Renderable);auto image=loader.Request(png,AsyncAssetKind::Image);
    Until(loader,tasks,[&]{return loader.Query(cpu).state==AssetLoadState::Ready&&loader.Query(render).state==AssetLoadState::Failed&&loader.Query(image).state==AssetLoadState::Failed;});
    Check(Has(loader.Query(render),"Assets.AsyncDependencyFailed")&&Has(loader.Query(image),"Assets.ImageDecodeFailed")&&loader.QueuedUploads().empty(),"bad required texture cannot mark complete model renderable");
    Check(asset_io::WriteAtomic(source/"checker.png",original),"restore texture fixture");
    std::vector<AssetDiagnostic> diagnostics;AssetId missing;Check(assets.RegisterSource("second.bmp",{},missing,diagnostics),"existing source ID");
    std::vector<std::uint8_t> saved;Check(asset_io::Read(source/"second.bmp",saved),"save missing-file fixture");fs::remove(source/"second.bmp");
    auto absent=loader.Request(missing,AsyncAssetKind::Image);Until(loader,tasks,[&]{return loader.Query(absent).state==AssetLoadState::Failed;});Check(Has(loader.Query(absent),"Assets.ReadFailed"),"missing file failure explained");Check(asset_io::WriteAtomic(source/"second.bmp",saved),"restore missing fixture");
    AsyncAssetLoader invalid(assets,tasks,{},[](const auto&,auto,const auto&){CpuAssetResult result;result.success=true;return result;});auto bad=invalid.Request(png,AsyncAssetKind::Image);Until(invalid,tasks,[&]{return invalid.Query(bad).state==AssetLoadState::Failed;});Check(Has(invalid.Query(bad),"Assets.AsyncInvalidCandidate"),"invalid decoder payload rejected before budget/upload");
}
}
int wmain(int argc,wchar_t** argv)
{
    try
    {
        Check(argc==3,"usage: AsyncAssetsTests samples output");const auto root=fs::absolute(argv[2])/asset_io::NewId();const auto source=root/"source";fs::create_directories(source);
        for(const auto& entry:fs::directory_iterator(argv[1]))if(entry.is_regular_file())fs::copy_file(entry.path(),source/entry.path().filename());
        WriteBmp(source/"mutable.bmp",255,0);WriteBmp(source/"second.bmp",0,255);AssetStore assets(source,root/"cache");std::vector<AssetDiagnostic> diagnostics;AssetId image,second,png;std::vector<AssetId> models;
        Check(assets.RegisterSource("mutable.bmp",{},image,diagnostics)&&assets.RegisterSource("second.bmp",{},second,diagnostics)&&assets.RegisterSource("checker.png",{},png,diagnostics),"register image fixture catalog");
        for(const auto& name:{"Hierarchy.glb","Hierarchy.fbx","Hierarchy.gltf"}){AssetId id;Check(assets.RegisterSource(name,{},id,diagnostics),"register model fixture");models.push_back(id);}
        SharedStates(assets,image);LateResults(assets,image,source/"mutable.bmp");BudgetsAndFailures(assets,image,second);RealModels(assets,models,png);InvalidSources(assets,models[2],png,source);
        std::ofstream(root/"result.json")<<nlohmann::json{{"success",true},{"real_models",3},{"real_png",true},{"controlled_gpu_port_only",true}}.dump(2);
        std::cout<<"Async assets: real decode/cache, shared targets, CPU budget/leases, cancellation, stale receipts, shutdown and main-thread publication passed\n";return 0;
    }
    catch(const std::exception& error){std::cerr<<error.what()<<'\n';return 1;}
}
