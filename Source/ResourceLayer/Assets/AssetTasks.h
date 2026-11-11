#pragma once
#include "AssetStore.h"
#include "CoreLayer/TaskQueue.h"
#include <functional>

namespace se
{
template<class T> struct AssetTaskResult
{
    std::shared_ptr<const T> data;
    std::vector<AssetDiagnostic> diagnostics;
};
using ModelCompletion = std::function<void(AssetTaskResult<StaticModel>)>;
using ImageCompletion = std::function<void(AssetTaskResult<ImageData>)>;

struct PreparedModelImages
{
    std::vector<ImageData> images;
    std::vector<bool> unavailable;
    std::vector<std::string> keys;
};
// 同步渲染和后台候选共用 CPU 解码；非法路径失败不改变 output，缺图状态明确保留。
bool PrepareModelImages(const StaticModel& model, const std::filesystem::path& source,
    const std::filesystem::path& allowedRoot, PreparedModelImages& output,
    std::vector<AssetDiagnostic>& diagnostics, std::uint64_t maximumRgbaBytes = UINT64_MAX);

// 路径和参数复制到后台；回调只在 queue.Pump() 的所有者线程执行。
// 取消会丢弃结果，不强行中断第三方解析器；回调捕获的对象应存活到完成或主动取消。
CancellationToken ImportModelAsync(TaskQueue& queue, std::filesystem::path path,
    ImportOptions options, ModelCompletion completion);
CancellationToken DecodeImageAsync(TaskQueue& queue, std::filesystem::path path,
    ImageCompletion completion);
}
