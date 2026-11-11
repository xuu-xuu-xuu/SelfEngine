#include "AssetTasks.h"
#include "AssetIO.h"
#include "StaticModelPreparation.h"

namespace se
{
bool PrepareModelImages(const StaticModel& model, const std::filesystem::path& source,
    const std::filesystem::path& allowedRoot, PreparedModelImages& output,
    std::vector<AssetDiagnostic>& diagnostics, std::uint64_t maximumRgbaBytes)
{
    PreparedModelImages candidate;
    candidate.images.resize(model.textures.size()); candidate.unavailable.resize(model.textures.size()); candidate.keys.resize(model.textures.size());
    for (std::size_t index = 0; index < model.textures.size(); ++index)
    {
        const auto& texture = model.textures[index];
        const auto diagnosticBegin = diagnostics.size();
        if (texture.missing) candidate.unavailable[index] = true;
        else if (!texture.embeddedBytes.empty())
            candidate.unavailable[index] = !DecodeImage(texture.embeddedBytes, candidate.images[index], diagnostics, maximumRgbaBytes);
        else if (!texture.relativePath.empty())
        {
            std::filesystem::path resolved; std::string relative;
            if (!asset_io::ResolveRelative(source, allowedRoot, texture.relativePath, resolved, relative))
            { asset_io::Diagnostic(diagnostics, "Assets.ReferenceOutsideRoot", texture.relativePath); return false; }
            candidate.unavailable[index] = !DecodeImage(resolved, candidate.images[index], diagnostics, maximumRgbaBytes);
        }
        else candidate.unavailable[index] = true;
        for (auto at = diagnosticBegin; at < diagnostics.size(); ++at)
            if (diagnostics[at].key == "Assets.ImageBudgetExceeded") return false;
        maximumRgbaBytes -= candidate.images[index].rgba8.size();
        if (!candidate.unavailable[index])
        {
            candidate.keys[index] = ImageContentKey(candidate.images[index]);
            if (candidate.keys[index].empty()) { asset_io::Diagnostic(diagnostics,"Assets.ReadFailed",texture.relativePath); return false; }
        }
    }
    output = std::move(candidate); return true;
}

CancellationToken ImportModelAsync(TaskQueue& queue, std::filesystem::path path,
    ImportOptions options, ModelCompletion completion)
{
    return queue.Submit([path = std::move(path), options = std::move(options), completion = std::move(completion)]
        (const CancellationToken& token) -> TaskQueue::Completion {
        if (token.IsCancelled()) return {};
        AssetTaskResult<StaticModel> result;
        auto model = std::make_shared<StaticModel>();
        if (ImportStaticModel(path, options, *model, result.diagnostics)) result.data = std::move(model);
        if (token.IsCancelled() || !completion) return {};
        return [completion, result = std::move(result)]() mutable { completion(std::move(result)); };
    });
}

CancellationToken DecodeImageAsync(TaskQueue& queue, std::filesystem::path path, ImageCompletion completion)
{
    return queue.Submit([path = std::move(path), completion = std::move(completion)]
        (const CancellationToken& token) -> TaskQueue::Completion {
        if (token.IsCancelled()) return {};
        AssetTaskResult<ImageData> result;
        auto image = std::make_shared<ImageData>();
        if (DecodeImage(path, *image, result.diagnostics)) result.data = std::move(image);
        if (token.IsCancelled() || !completion) return {};
        return [completion, result = std::move(result)]() mutable { completion(std::move(result)); };
    });
}
}
