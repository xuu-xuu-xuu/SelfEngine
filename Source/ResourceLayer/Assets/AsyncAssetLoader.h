#pragma once
#include "StaticModelPreparation.h"
#include <cstdint>

namespace se
{
namespace detail { struct AssetDemand; struct CpuReservation; }
enum class AsyncAssetKind { Image, Model };
enum class AssetTarget { CpuOnly, Renderable };
enum class AssetLoadState { Unloaded, LoadingCPU, CpuReady, UploadQueued, UploadingGPU, Ready, Failed, Cancelled };
const char* AssetLoadStateKey(AssetLoadState state) noexcept;

struct CpuAssetData
{
    AsyncAssetKind kind = AsyncAssetKind::Image;
    std::shared_ptr<const ImageData> image;
    std::shared_ptr<const StaticModel> model;
    std::shared_ptr<const PreparedModelImages> modelImages;
    std::shared_ptr<const StaticModelLayout> modelLayout;
    std::string imageKey;
    std::uint64_t PayloadBytes() const noexcept { return bytes_; }
private:
    std::uint64_t bytes_ = 0;
    std::shared_ptr<detail::CpuReservation> reservation_;
    friend class AsyncAssetLoader;
};
struct CpuAssetResult
{
    bool success = false;
    CpuAssetData data;
    std::vector<AssetDiagnostic> diagnostics;
};
// 仅 CPU 准备：独立 AssetStore 视图复用原派生缓存；模型所有图片也在此线程解码。
CpuAssetResult LoadCpuAssetSnapshot(const AssetSourceSnapshot& source, AsyncAssetKind kind,
    const CancellationToken& cancellation, std::uint64_t maximumPayloadBytes = 256ull * 1024 * 1024);

// Resource 只持抽象所有权；Function 派生类持 RHI 资源，Fence 寿命继续由后端保证。
class RenderAssetLease
{
public:
    virtual ~RenderAssetLease() = default;
    virtual AsyncAssetKind Kind() const noexcept = 0;
};
struct AssetUploadToken
{
    std::uint64_t session = 0, operation = 0, revision = 0, attempt = 0;
    bool operator==(const AssetUploadToken&) const = default;
};
struct AssetUploadRequest
{
    AssetUploadToken token;
    AssetId id;
    std::shared_ptr<const CpuAssetData> cpu;
};
struct AssetUploadReceipt
{
    AssetUploadToken token;
    bool success = false;
    std::shared_ptr<const RenderAssetLease> lease;
    std::vector<AssetDiagnostic> diagnostics;
};
class AssetTicket
{
public:
    AssetTicket() = default;
    void Cancel() const noexcept;
    bool IsCancelled() const noexcept;
    bool Valid() const noexcept;
private:
    std::shared_ptr<detail::AssetDemand> demand_;
    friend class AsyncAssetLoader;
};
struct AssetView
{
    AssetLoadState state = AssetLoadState::Unloaded;
    AsyncAssetKind kind = AsyncAssetKind::Image;
    AssetTarget target = AssetTarget::CpuOnly;
    AssetId id;
    std::uint64_t operation = 0, revision = 0;
    std::shared_ptr<const CpuAssetData> cpu;
    std::shared_ptr<const RenderAssetLease> gpu;
    std::vector<AssetDiagnostic> diagnostics;
};
struct AssetStatusRow
{
    AssetId id; AsyncAssetKind kind = AsyncAssetKind::Image;
    AssetLoadState cpu = AssetLoadState::Unloaded, render = AssetLoadState::Unloaded;
    std::uint64_t operation = 0, revision = 0, cpuBytes = 0;
    std::size_t clients = 0;
};
struct AsyncAssetLimits
{
    std::size_t operations = 128, subscriptions = 512;
    std::uint64_t cpuBytes = 256ull * 1024 * 1024;
};
struct AsyncAssetStatistics
{
    std::size_t operations = 0, subscriptions = 0, loadingCpu = 0, queuedUploads = 0, uploadingGpu = 0, readyCpu = 0, readyRender = 0, failed = 0;
    std::uint64_t cpuBytes = 0, peakCpuBytes = 0, decodeStarts = 0, sharedRequests = 0, staleReceipts = 0, rejectedRequests = 0;
};
class AsyncAssetLoader
{
public:
    using CpuDecoder = std::function<CpuAssetResult(const AssetSourceSnapshot&, AsyncAssetKind, const CancellationToken&)>;
    // assets/tasks 必须活过本对象；只在构造线程调用，Ticket.Cancel 可跨线程。
    AsyncAssetLoader(AssetStore& assets, TaskQueue& tasks, AsyncAssetLimits limits = {}, CpuDecoder decoder = {});
    ~AsyncAssetLoader();
    AsyncAssetLoader(const AsyncAssetLoader&) = delete;
    AsyncAssetLoader& operator=(const AsyncAssetLoader&) = delete;
    AssetTicket Request(const AssetId& id, AsyncAssetKind kind, AssetTarget target = AssetTarget::CpuOnly);
    AssetView Query(const AssetTicket& ticket) const;
    std::size_t Pump(std::size_t completionLimit = 64);
    std::vector<AssetStatusRow> StatusRows() const;
    std::vector<AssetUploadRequest> QueuedUploads(std::size_t limit = 64) const;
    // 适配器在实际开始上传时调用 Begin；成功 Complete 仅接收 UploadingGPU。
    // 回执先保存，只有下一次 Pump 校验存活需求后发布 Ready。
    bool BeginUpload(const AssetUploadToken& token);
    bool IsUploadCurrent(const AssetUploadToken& token) const;
    bool CompleteUpload(AssetUploadReceipt receipt);
    void CollectUnused();
    void Shutdown(); // 不关闭借用的任务队列；worker/完成仅捕获 weak session。
    AsyncAssetStatistics Stats() const;
    static std::shared_ptr<const CpuAssetData> Placeholder(AsyncAssetKind kind);
private:
    struct Impl;
    std::shared_ptr<Impl> impl_;
};
}
