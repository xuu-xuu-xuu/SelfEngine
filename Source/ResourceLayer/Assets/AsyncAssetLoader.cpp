#include "AsyncAssetLoader.h"
#include "AssetIO.h"
#include "Primitives.h"
#include <algorithm>
#include <limits>
#include <map>
#include <optional>
#include <stdexcept>

namespace se
{
namespace detail
{
struct CpuBudget
{
    std::mutex mutex; std::uint64_t limit = 0, used = 0, peak = 0;
    bool Acquire(std::uint64_t bytes)
    { std::lock_guard lock(mutex); if (bytes > limit - used) return false; used += bytes; peak = (std::max)(peak, used); return true; }
};
struct CpuReservation
{
    std::shared_ptr<CpuBudget> budget; std::uint64_t bytes = 0;
    std::shared_ptr<const ImageData> image;
    std::shared_ptr<const StaticModel> model;
    std::shared_ptr<const PreparedModelImages> modelImages;
    std::shared_ptr<const StaticModelLayout> modelLayout;
    ~CpuReservation()
    {
        image.reset(); model.reset(); modelImages.reset(); modelLayout.reset();
        if (bytes) { std::lock_guard lock(budget->mutex); budget->used -= bytes; }
    }
};
struct AssetRecord;
struct AssetDemand
{
    std::uint64_t session = 0, operation = 0, revision = 0;
    AssetId id; AsyncAssetKind kind = AsyncAssetKind::Image; AssetTarget target = AssetTarget::CpuOnly;
    std::atomic_bool cancelled = false; bool rejected = false;
    std::weak_ptr<AssetRecord> record; std::vector<AssetDiagnostic> diagnostics;
};
struct AssetRecord
{
    AssetSourceSnapshot source; AsyncAssetKind kind = AsyncAssetKind::Image;
    std::uint64_t operation = 0, revision = 0;
    AssetLoadState cpuState = AssetLoadState::LoadingCPU, renderState = AssetLoadState::Unloaded;
    std::vector<std::weak_ptr<AssetDemand>> clients; CancellationToken worker;
    std::shared_ptr<const CpuAssetData> cpu; std::shared_ptr<const RenderAssetLease> gpu;
    std::vector<AssetDiagnostic> cpuDiagnostics, renderDiagnostics;
    AssetUploadToken upload; std::optional<AssetUploadReceipt> receipt;
};
}
namespace
{
using Record = detail::AssetRecord;
std::atomic<std::uint64_t> nextSession = 1;
std::uint64_t NewSession()
{
    auto value = nextSession.load();
    do { if (value == (std::numeric_limits<std::uint64_t>::max)()) throw std::overflow_error("Assets.AsyncIdentityExhausted"); }
    while (!nextSession.compare_exchange_weak(value, value + 1));
    return value;
}
bool ValidImage(const ImageData& image)
{
    const auto pixels = std::uint64_t(image.width) * image.height;
    return image.width && image.height && pixels <= 64ull * 1024 * 1024 && image.rgba8.size() == pixels * 4;
}
bool PayloadSize(const CpuAssetData& data, AsyncAssetKind kind, std::uint64_t& bytes)
{
    bytes = 0;
    const auto add = [&](std::size_t count, std::size_t stride = 1) {
        if (count > ((std::numeric_limits<std::uint64_t>::max)() - bytes) / stride) return false;
        bytes += count * stride; return true;
    };
    if (data.kind != kind) return false;
    if (kind == AsyncAssetKind::Image)
        return data.image && !data.model && !data.modelImages && !data.modelLayout && !data.imageKey.empty() && ValidImage(*data.image) && add(data.image->rgba8.size());
    if (!data.model || data.image || !data.modelImages || !ValidateStaticModel(*data.model)) return false;
    const auto& model = *data.model;
    if (data.modelImages->images.size() != model.textures.size() || data.modelImages->unavailable.size() != model.textures.size() || data.modelImages->keys.size() != model.textures.size()) return false;
    if (data.modelLayout && (!add(data.modelLayout->parts.size(),sizeof(StaticModelPart)) || !add(2,sizeof(Vec3)))) return false;
    for (const auto& mesh : model.meshes)
        if (!add(mesh.vertices.size(), sizeof(MeshVertex)) || !add(mesh.indices.size(), sizeof(std::uint32_t)) || !add(mesh.name.size())) return false;
    for (const auto& node : model.nodes)
        if (!add(node.localMatrix.size(), sizeof(float)) || !add(1, sizeof(node.parent)) || !add(node.meshes.size(), sizeof(std::uint32_t)) || !add(node.name.size())) return false;
    for (const auto& material : model.materials) if (!add(sizeof(MaterialData)) || !add(material.name.size())) return false;
    for (const auto& dependency : model.dependencies) if (!add(dependency.size())) return false;
    for (std::size_t index = 0; index < model.textures.size(); ++index)
    {
        const auto& texture = model.textures[index]; const auto& image = data.modelImages->images[index];
        if (!data.modelImages->unavailable[index] && (!ValidImage(image) || data.modelImages->keys[index].empty())) return false;
        if (!add(texture.embeddedBytes.size()) || !add(texture.relativePath.size()) || !add(texture.mimeType.size()) || !add(image.rgba8.size()) || !add(data.modelImages->keys[index].size())) return false;
    }
    return bytes != 0;
}
bool RenderDependenciesValid(const CpuAssetData& data)
{
    if (data.kind == AsyncAssetKind::Image) return true;
    if (!data.modelLayout) return false;
    for (const auto& material : data.model->materials)
        for (const auto index : {material.baseColorTexture, material.normalTexture, material.metallicRoughnessTexture})
            if (index >= 0 && data.modelImages->unavailable[static_cast<std::size_t>(index)]
                && !data.model->textures[static_cast<std::size_t>(index)].missing
                && !(index == material.baseColorTexture && material.needsFallbackTexture)) return false;
    return true;
}
std::pair<std::size_t, std::size_t> Demands(const Record& record)
{
    std::pair<std::size_t, std::size_t> counts{};
    for (const auto& weak : record.clients) if (auto client = weak.lock(); client && !client->cancelled.load())
        ++(client->target == AssetTarget::CpuOnly ? counts.first : counts.second);
    return counts;
}
std::string ErrorDetail(const TaskErrors& errors)
{
    const auto error = errors.work ? errors.work : errors.completion ? errors.completion : errors.handler;
    if (error) try { std::rethrow_exception(error); } catch (const std::exception& value) { return value.what(); } catch (...) { return "unknown exception"; }
    return {};
}
}
const char* AssetLoadStateKey(AssetLoadState state) noexcept
{
    switch (state)
    {
    case AssetLoadState::Unloaded: return "Assets.State.Unloaded";
    case AssetLoadState::LoadingCPU: return "Assets.State.LoadingCPU";
    case AssetLoadState::CpuReady: return "Assets.State.CpuReady";
    case AssetLoadState::UploadQueued: return "Assets.State.UploadQueued";
    case AssetLoadState::UploadingGPU: return "Assets.State.UploadingGPU";
    case AssetLoadState::Ready: return "Assets.State.Ready";
    case AssetLoadState::Cancelled: return "Assets.State.Cancelled";
    default: return "Assets.State.Failed";
    }
}
CpuAssetResult LoadCpuAssetSnapshot(const AssetSourceSnapshot& source, AsyncAssetKind kind, const CancellationToken& cancellation, std::uint64_t maximumPayloadBytes)
{
    CpuAssetResult result; result.data.kind = kind; if (cancellation.IsCancelled()) return result;
    if (kind == AsyncAssetKind::Image)
    {
        result.data.image = AssetStore::LoadImageSnapshot(source, result.diagnostics, maximumPayloadBytes);
        if (result.data.image) result.data.imageKey = ImageContentKey(*result.data.image);
    }
    else
    {
        result.data.model = AssetStore::LoadModelSnapshot(source, result.diagnostics);
        if (result.data.model && !cancellation.IsCancelled())
        {
            auto images = std::make_shared<PreparedModelImages>();
            images->images.resize(result.data.model->textures.size()); images->unavailable.resize(result.data.model->textures.size(), true); images->keys.resize(result.data.model->textures.size());
            auto layout = std::make_shared<StaticModelLayout>();
            if (BuildStaticModelLayout(*result.data.model,*layout,result.diagnostics)) result.data.modelLayout = std::move(layout);
            result.data.modelImages = images; std::uint64_t baseBytes = 0;
            if (!PayloadSize(result.data, kind, baseBytes) || baseBytes > maximumPayloadBytes)
            { result.data.modelImages.reset(); asset_io::Diagnostic(result.diagnostics, "Assets.AsyncCpuBudget", source.id); }
            else if (!PrepareModelImages(*result.data.model, source.source, source.sourceRoot, *images, result.diagnostics, maximumPayloadBytes - baseBytes)) result.data.modelImages.reset();
        }
    }
    result.success = kind == AsyncAssetKind::Image ? result.data.image && !result.data.imageKey.empty() : result.data.model && result.data.modelImages;
    return result;
}
void AssetTicket::Cancel() const noexcept { if (demand_) demand_->cancelled.store(true); }
bool AssetTicket::IsCancelled() const noexcept { return !demand_ || demand_->rejected || demand_->cancelled.load(); }
bool AssetTicket::Valid() const noexcept { return demand_ && !demand_->rejected; }

struct AsyncAssetLoader::Impl : std::enable_shared_from_this<Impl>
{
    AssetStore& assets; TaskQueue& tasks; const AsyncAssetLimits limits; const CpuDecoder decoder;
    const std::thread::id owner = std::this_thread::get_id(); const std::uint64_t session = NewSession();
    std::uint64_t nextOperation = 1, nextUpload = 1, sharedRequests = 0, staleReceipts = 0, rejectedRequests = 0;
    std::map<std::pair<AsyncAssetKind, AssetId>, std::shared_ptr<Record>> records;
    std::shared_ptr<detail::CpuBudget> budget = std::make_shared<detail::CpuBudget>();
    std::shared_ptr<std::atomic<std::uint64_t>> decodeStarts = std::make_shared<std::atomic<std::uint64_t>>(0);
    bool closed = false;
    Impl(AssetStore& a, TaskQueue& t, AsyncAssetLimits l, CpuDecoder d) : assets(a), tasks(t), limits(l), decoder(d ? std::move(d) : CpuDecoder([bytes = l.cpuBytes](const auto& source, auto kind, const auto& token) { return LoadCpuAssetSnapshot(source, kind, token, bytes); }))
    {
        if (!l.operations || l.operations > 4096 || !l.subscriptions || l.subscriptions > 65536 || !l.cpuBytes || l.cpuBytes > (1ull << 40))
            throw std::invalid_argument("Assets.AsyncInvalidLimits");
        budget->limit = l.cpuBytes;
    }
    void CheckOwner() const { if (std::this_thread::get_id() != owner) throw std::logic_error("Assets.AsyncWrongThread"); }
    std::shared_ptr<Record> Find(std::uint64_t operation) const
    { for (const auto& [_, record] : records) if (record->operation == operation) return record; return {}; }
    std::shared_ptr<Record> Find(const AssetUploadToken& token) const
    { if (token.session != session || !token.attempt) return {}; auto record = Find(token.operation); return record && record->upload == token ? record : nullptr; }
    void Fail(Record& record, const char* key, std::string detail)
    {
        record.cpuState = AssetLoadState::Failed;
        if (Demands(record).second) record.renderState = AssetLoadState::Failed;
        asset_io::Diagnostic(record.cpuDiagnostics, key, std::move(detail));
    }
    void Start(const std::shared_ptr<Record>& record)
    {
        const auto weak = weak_from_this(); const auto source = record->source; const auto operation = record->operation;
        record->worker = tasks.Submit([weak, source, operation, decode = decoder, memory = budget, starts = decodeStarts, kind = record->kind](const CancellationToken& cancellation) -> TaskQueue::Completion {
            ++*starts; auto result = decode(source, kind, cancellation); if (cancellation.IsCancelled()) return {};
            std::uint64_t bytes = 0;
            if (result.success && !PayloadSize(result.data, kind, bytes))
            { result.success = false; asset_io::Diagnostic(result.diagnostics, "Assets.AsyncInvalidCandidate", source.id); }
            std::shared_ptr<const CpuAssetData> published;
            if (result.success)
            {
                auto data = std::make_shared<CpuAssetData>(std::move(result.data)); auto reservation = std::make_shared<detail::CpuReservation>(); reservation->budget = memory;
                if (memory->Acquire(bytes))
                {
                    reservation->bytes = bytes; reservation->image = std::move(data->image); reservation->model = std::move(data->model); reservation->modelImages = std::move(data->modelImages); reservation->modelLayout = std::move(data->modelLayout);
                    if (reservation->image) data->image = {reservation, reservation->image.get()};
                    if (reservation->model) data->model = {reservation, reservation->model.get()};
                    if (reservation->modelImages) data->modelImages = {reservation, reservation->modelImages.get()};
                    if (reservation->modelLayout) data->modelLayout = {reservation, reservation->modelLayout.get()};
                    data->bytes_ = bytes; data->reservation_ = std::move(reservation); published = std::move(data);
                }
                else asset_io::Diagnostic(result.diagnostics, "Assets.AsyncCpuBudget", source.id);
            }
            return [weak, operation, data = std::move(published), diagnostics = std::move(result.diagnostics)]() mutable {
                if (auto manager = weak.lock(); manager && !manager->closed)
                    if (auto current = manager->Find(operation))
                    {
                        current->cpuDiagnostics = std::move(diagnostics);
                        if (!data) manager->Fail(*current, "Assets.AsyncCpuFailed", current->source.id);
                        else { current->cpu = std::move(data); current->cpuState = AssetLoadState::CpuReady; if (Demands(*current).second) current->renderState = AssetLoadState::CpuReady; }
                    }
            };
        }, [weak, operation](const TaskErrors& errors) {
            if (auto manager = weak.lock(); manager && !manager->closed)
                if (auto current = manager->Find(operation)) manager->Fail(*current, "Assets.AsyncTaskFailed", ErrorDetail(errors));
        });
        if (!record->worker.Accepted()) { ++rejectedRequests; Fail(*record, "Assets.AsyncTaskRejected", record->source.id); }
    }
    void Prune()
    {
        for (auto it = records.begin(); it != records.end();)
        {
            auto& record = *it->second;
            std::erase_if(record.clients, [](const auto& weak) { auto client = weak.lock(); return !client || client->cancelled.load(); });
            const auto [cpu, render] = Demands(record);
            if (!cpu && !render) { record.worker.Cancel(); it = records.erase(it); continue; }
            if (!render) { record.renderState = AssetLoadState::Unloaded; record.upload = {}; record.receipt.reset(); record.gpu.reset(); record.renderDiagnostics.clear(); }
            ++it;
        }
    }
    void QueueRender(Record& record)
    {
        if (!RenderDependenciesValid(*record.cpu))
        { record.renderState = AssetLoadState::Failed; asset_io::Diagnostic(record.renderDiagnostics, "Assets.AsyncDependencyFailed", record.source.id); return; }
        if (nextUpload == (std::numeric_limits<std::uint64_t>::max)())
        { record.renderState = AssetLoadState::Failed; asset_io::Diagnostic(record.renderDiagnostics, "Assets.AsyncIdentityExhausted", record.source.id); return; }
        record.upload = {session, record.operation, record.revision, nextUpload++}; record.renderState = AssetLoadState::UploadQueued;
    }
};
AsyncAssetLoader::AsyncAssetLoader(AssetStore& assets, TaskQueue& tasks, AsyncAssetLimits limits, CpuDecoder decoder)
    : impl_(std::make_shared<Impl>(assets, tasks, limits, std::move(decoder))) {}
AsyncAssetLoader::~AsyncAssetLoader() { Shutdown(); }
AssetTicket AsyncAssetLoader::Request(const AssetId& id, AsyncAssetKind kind, AssetTarget target)
{
    auto& p = *impl_; p.CheckOwner(); p.Prune(); AssetTicket ticket; ticket.demand_ = std::make_shared<detail::AssetDemand>(); auto& demand = *ticket.demand_;
    demand.id = id; demand.kind = kind; demand.target = target; demand.session = p.session;
    const auto reject = [&](const char* key) { demand.rejected = true; ++p.rejectedRequests; asset_io::Diagnostic(demand.diagnostics, key, id); return ticket; };
    if (p.closed) return reject("Assets.AsyncClosed");
    if ((kind != AsyncAssetKind::Image && kind != AsyncAssetKind::Model) || (target != AssetTarget::CpuOnly && target != AssetTarget::Renderable)) return reject("Assets.AsyncInvalidCandidate");
    std::size_t clients = 0; for (const auto& [_, record] : p.records) clients += record->clients.size();
    if (clients >= p.limits.subscriptions) return reject("Assets.AsyncRequestCapacity");
    const auto key = std::make_pair(kind, id); auto found = p.records.find(key); std::shared_ptr<Record> record;
    if (found == p.records.end())
    {
        if (p.records.size() == p.limits.operations) return reject("Assets.AsyncRequestCapacity");
        if (p.nextOperation == (std::numeric_limits<std::uint64_t>::max)()) return reject("Assets.AsyncIdentityExhausted");
        record = std::make_shared<Record>();
        if (!p.assets.DescribeSource(id, record->source, demand.diagnostics)) { demand.rejected = true; ++p.rejectedRequests; return ticket; }
        record->kind = kind; record->operation = record->revision = p.nextOperation++; p.records.emplace(key, record);
    }
    else { record = found->second; ++p.sharedRequests; }
    demand.operation = record->operation; demand.revision = record->revision; demand.record = record; record->clients.push_back(ticket.demand_);
    if (target == AssetTarget::Renderable && record->renderState == AssetLoadState::Unloaded)
        record->renderState = record->cpuState == AssetLoadState::Ready ? AssetLoadState::CpuReady : record->cpuState;
    if (found == p.records.end()) p.Start(record);
    return ticket;
}
AssetView AsyncAssetLoader::Query(const AssetTicket& ticket) const
{
    const auto& p = *impl_; p.CheckOwner(); AssetView view;
    if (!ticket.demand_ || ticket.demand_->session != p.session)
    { view.state = AssetLoadState::Failed; asset_io::Diagnostic(view.diagnostics, "Assets.AsyncInvalidTicket"); return view; }
    const auto& demand = *ticket.demand_; view.id = demand.id; view.kind = demand.kind; view.target = demand.target; view.operation = demand.operation; view.revision = demand.revision;
    if (demand.rejected) { view.state = AssetLoadState::Failed; view.diagnostics = demand.diagnostics; return view; }
    if (p.closed || demand.cancelled.load())
    { view.state = AssetLoadState::Cancelled; asset_io::Diagnostic(view.diagnostics, p.closed ? "Assets.AsyncClosed" : "Assets.AsyncCancelled", demand.id); return view; }
    const auto record = demand.record.lock(); if (!record) { view.state = AssetLoadState::Unloaded; return view; }
    view.state = demand.target == AssetTarget::CpuOnly ? record->cpuState : record->renderState;
    view.cpu = record->cpu; view.diagnostics = record->cpuDiagnostics;
    if (demand.target == AssetTarget::Renderable)
    { view.gpu = record->gpu; view.diagnostics.insert(view.diagnostics.end(), record->renderDiagnostics.begin(), record->renderDiagnostics.end()); }
    return view;
}
std::size_t AsyncAssetLoader::Pump(std::size_t completionLimit)
{
    auto& p = *impl_; p.CheckOwner(); if (p.closed) return 0; p.Prune(); std::size_t processed = 0;
    for (auto& [_, value] : p.records)
    {
        auto& record = *value; if (processed == completionLimit) break;
        if (record.receipt)
        {
            auto receipt = std::move(*record.receipt); record.receipt.reset(); record.renderDiagnostics = std::move(receipt.diagnostics);
            if (receipt.success) { record.gpu = std::move(receipt.lease); record.renderState = AssetLoadState::Ready; }
            else { record.renderState = AssetLoadState::Failed; asset_io::Diagnostic(record.renderDiagnostics, "Assets.AsyncUploadFailed", record.source.id); }
            ++processed;
        }
        else if (record.cpuState == AssetLoadState::CpuReady || record.renderState == AssetLoadState::CpuReady)
        {
            record.cpuState = AssetLoadState::Ready;
            if (Demands(record).second && record.renderState == AssetLoadState::CpuReady) p.QueueRender(record);
            ++processed;
        }
        else if (record.cpuState == AssetLoadState::LoadingCPU && (record.worker.Status() == TaskStatus::Cancelled || record.worker.Status() == TaskStatus::Failed))
        { p.Fail(record, "Assets.AsyncTaskRejected", record.source.id); ++processed; }
    }
    processed += p.tasks.Pump(completionLimit - processed); p.Prune(); return processed;
}
std::vector<AssetStatusRow> AsyncAssetLoader::StatusRows() const
{
    const auto& p = *impl_; p.CheckOwner(); std::vector<AssetStatusRow> rows;
    for (const auto& [_, record] : p.records) { const auto counts = Demands(*record); rows.push_back({record->source.id, record->kind, record->cpuState, record->renderState, record->operation, record->revision, record->cpu ? record->cpu->PayloadBytes() : 0, counts.first + counts.second}); }
    return rows;
}
std::vector<AssetUploadRequest> AsyncAssetLoader::QueuedUploads(std::size_t limit) const
{
    const auto& p = *impl_; p.CheckOwner(); std::vector<AssetUploadRequest> requests;
    for (const auto& [_, record] : p.records)
        if (requests.size() < limit && record->renderState == AssetLoadState::UploadQueued && Demands(*record).second)
            requests.push_back({record->upload, record->source.id, record->cpu});
    return requests;
}
bool AsyncAssetLoader::BeginUpload(const AssetUploadToken& token)
{
    auto& p = *impl_; p.CheckOwner(); p.Prune(); auto record = p.Find(token);
    if (p.closed || !record || record->renderState != AssetLoadState::UploadQueued || !Demands(*record).second || record->receipt) return false;
    record->renderState = AssetLoadState::UploadingGPU; return true;
}
bool AsyncAssetLoader::IsUploadCurrent(const AssetUploadToken& token) const
{
    const auto& p = *impl_; p.CheckOwner(); const auto record = p.Find(token);
    return !p.closed && record && Demands(*record).second && !record->receipt
        && (record->renderState == AssetLoadState::UploadQueued || record->renderState == AssetLoadState::UploadingGPU);
}
bool AsyncAssetLoader::CompleteUpload(AssetUploadReceipt receipt)
{
    auto& p = *impl_; p.CheckOwner(); p.Prune(); auto record = p.Find(receipt.token);
    if (p.closed || !record || record->receipt || !Demands(*record).second
        || (receipt.success && (record->renderState != AssetLoadState::UploadingGPU || !receipt.lease || receipt.lease->Kind() != record->kind))
        || (!receipt.success && record->renderState != AssetLoadState::UploadingGPU && record->renderState != AssetLoadState::UploadQueued))
    { ++p.staleReceipts; return false; }
    record->receipt = std::move(receipt); return true;
}
void AsyncAssetLoader::CollectUnused() { impl_->CheckOwner(); impl_->Prune(); }
void AsyncAssetLoader::Shutdown()
{
    auto& p = *impl_; p.CheckOwner(); if (p.closed) return; p.closed = true;
    for (auto& [_, record] : p.records) record->worker.Cancel(); p.records.clear();
}
AsyncAssetStatistics AsyncAssetLoader::Stats() const
{
    const auto& p = *impl_; p.CheckOwner(); AsyncAssetStatistics stats; stats.operations = p.records.size();
    for (const auto& [_, record] : p.records)
    {
        const auto counts = Demands(*record); stats.subscriptions += counts.first + counts.second;
        stats.loadingCpu += record->cpuState == AssetLoadState::LoadingCPU; stats.readyCpu += record->cpuState == AssetLoadState::Ready;
        stats.queuedUploads += record->renderState == AssetLoadState::UploadQueued; stats.uploadingGpu += record->renderState == AssetLoadState::UploadingGPU;
        stats.readyRender += record->renderState == AssetLoadState::Ready;
        stats.failed += record->cpuState == AssetLoadState::Failed || record->renderState == AssetLoadState::Failed;
    }
    { std::lock_guard lock(p.budget->mutex); stats.cpuBytes = p.budget->used; stats.peakCpuBytes = p.budget->peak; }
    stats.decodeStarts = p.decodeStarts->load(); stats.sharedRequests = p.sharedRequests; stats.staleReceipts = p.staleReceipts; stats.rejectedRequests = p.rejectedRequests;
    return stats;
}
std::shared_ptr<const CpuAssetData> AsyncAssetLoader::Placeholder(AsyncAssetKind kind)
{
    static const auto image = [] { auto data = std::make_shared<CpuAssetData>(); data->image = std::make_shared<ImageData>(DefaultTextureImage(DefaultTextureKind::Fallback)); data->imageKey=ImageContentKey(*data->image); return data; }();
    static const auto model = [] { auto data = std::make_shared<CpuAssetData>(); data->kind = AsyncAssetKind::Model; auto cube = std::make_shared<StaticModel>(); cube->meshes.push_back(MakeUnitCube()); ModelNode node; node.meshes = {0}; cube->nodes.push_back(std::move(node)); data->model = std::move(cube); data->modelImages = std::make_shared<PreparedModelImages>(); auto layout=std::make_shared<StaticModelLayout>();std::vector<AssetDiagnostic> diagnostics;BuildStaticModelLayout(*data->model,*layout,diagnostics);data->modelLayout=std::move(layout);return data; }();
    return kind == AsyncAssetKind::Model ? model : image;
}
}
