#pragma once
#include "StaticModelRender.h"
#include "ResourceLayer/Assets/AsyncAssetLoader.h"

namespace se
{
class Profiler;
struct RenderImageAsset final : RenderAssetLease
{
    rhi::Texture texture;
    AsyncAssetKind Kind() const noexcept override { return AsyncAssetKind::Image; }
};
struct RenderModelAsset final : RenderAssetLease
{
    std::shared_ptr<const StaticRenderModel> model;
    AsyncAssetKind Kind() const noexcept override { return AsyncAssetKind::Model; }
};
struct AssetUploadBudget {std::uint64_t bytes=256*1024;std::size_t tasks=8;};
struct AssetUploadStep
{
    std::uint64_t payloadBytes=0,footprintBytes=0;
    std::size_t tasks=0,creations=0,completed=0,cancelled=0,failed=0,pending=0;
    bool submitted=false;
};
class AssetUploadScheduler
{
public:
    // device/resources/profiler 活过调度器；所有调用仅限构造线程。
    AssetUploadScheduler(rhi::Device& device,AsyncAssetLoader& resources,AssetUploadBudget budget={},Profiler* profiler=nullptr);
    ~AssetUploadScheduler();
    AssetUploadScheduler(const AssetUploadScheduler&)=delete;
    AssetUploadScheduler& operator=(const AssetUploadScheduler&)=delete;
    bool SetBudget(AssetUploadBudget budget);
    rhi::Result Tick(AssetUploadStep& output);
    void Shutdown(); // 只丢逻辑候选，不为取消调用 WaitIdle。
private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};
}
