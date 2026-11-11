#include "AssetStore.h"
#include "AssetIO.h"
#include <nlohmann/json.hpp>
#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstring>
#include <fstream>
#include <functional>
#include <set>
#include <Windows.h>
#include <wincodec.h>
#include <wrl/client.h>

namespace se
{
namespace
{
using Json = nlohmann::json;
using asset_io::Diagnostic;
bool ValidId(const std::string& id)
{
    return id.size() == 32 && std::all_of(id.begin(), id.end(), [](char c) { return (c >= '0' && c <= '9') || (c >= 'a' && c <= 'f'); });
}
bool SamePath(const std::filesystem::path& a,const std::filesystem::path& b)
{return asset_io::Within(a,b)&&asset_io::Within(b,a);}
std::filesystem::path Metadata(const std::filesystem::path& source) { auto result = source; result += ".asset.json"; return result; }
Json ReadJson(const std::filesystem::path& path)
{
    std::vector<std::uint8_t> bytes;
    if (!asset_io::Read(path, bytes)) return Json();
    return Json::parse(bytes.begin(), bytes.end(), nullptr, false);
}
bool WriteJson(const std::filesystem::path& path, const Json& json)
{
    const std::string text = json.dump(2);
    if (text.size() > asset_io::MaxFileBytes) return false;
    return asset_io::WriteAtomic(path, { reinterpret_cast<const std::uint8_t*>(text.data()), text.size() });
}
Json OptionsJson(const ImportOptions& options)
{ return { { "scale", options.scale }, { "generateTangents", options.generateTangents } }; }
bool ReadMetadata(const std::filesystem::path& source, AssetId& id, ImportOptions& options)
{
    const auto json = ReadJson(Metadata(source));
    if (!json.is_object() || json.value("version", 0) != 1) return false;
    id = json.at("id").get<std::string>();
    options.scale = json.at("options").at("scale").get<float>();
    options.generateTangents = json.at("options").at("generateTangents").get<bool>();
    return ValidId(id) && std::isfinite(options.scale) && options.scale > 0;
}

std::string CacheKey(const std::filesystem::path& source, const ImportOptions& options,
    const std::vector<std::string>& dependencies)
{
    std::vector<std::uint8_t> bytes;
    if (!asset_io::Read(source, bytes)) return {};
    const auto sourceHash = asset_io::Hash(bytes);
    if (sourceHash.empty()) return {};
    std::string text = std::string(StaticModelImporterVersion) + "\n" + OptionsJson(options).dump() + "\n" + sourceHash;
    for (const auto& dependency : dependencies)
    {
        std::filesystem::path resolved; std::string relative;
        if (!asset_io::ResolveRelative(source, options.allowedRoot, dependency, resolved, relative)) return {};
        text += "\n" + relative + "=";
        if (std::filesystem::is_regular_file(resolved))
        {
            if (!asset_io::Read(resolved, bytes)) return {};
            const auto hash = asset_io::Hash(bytes);
            if (hash.empty()) return {};
            text += hash;
        }
        else text += "missing";
    }
    return asset_io::Hash({ reinterpret_cast<const std::uint8_t*>(text.data()), text.size() });
}

Json Encode(const StaticModel& model)
{
    Json json = { { "meshes", Json::array() }, { "materials", Json::array() },
        { "nodes", Json::array() }, { "textures", Json::array() }, { "dependencies", model.dependencies } };
    for (const auto& mesh : model.meshes)
    {
        Json vertices = Json::array();
        for (const auto& v : mesh.vertices)
            vertices.push_back({ { "p", v.position }, { "n", v.normal }, { "uv", v.uv }, { "t", v.tangent } });
        json["meshes"].push_back({ { "name", mesh.name }, { "vertices", vertices }, { "indices", mesh.indices }, { "material", mesh.material } });
    }
    for (const auto& material : model.materials)
        json["materials"].push_back({ { "name", material.name }, { "baseColor", material.baseColor },
            { "metallic", material.metallic }, { "roughness", material.roughness },
            { "baseColorTexture", material.baseColorTexture }, { "normalTexture", material.normalTexture },
            { "metallicRoughnessTexture", material.metallicRoughnessTexture },
            { "doubleSided", material.doubleSided }, { "needsFallbackTexture", material.needsFallbackTexture }, {"normalScale",material.normalScale} });
    for (const auto& node : model.nodes)
        json["nodes"].push_back({ { "name", node.name }, { "parent", node.parent }, { "matrix", node.localMatrix }, { "meshes", node.meshes } });
    for (const auto& texture : model.textures)
        json["textures"].push_back({ { "path", texture.relativePath }, { "mime", texture.mimeType },
            { "embedded", texture.embeddedBytes }, { "missing", texture.missing } });
    return json;
}

bool Decode(const Json& json, StaticModel& model)
{
    if (!json.is_object() || json.at("nodes").size() > 100'000 || json.at("meshes").size() > 100'000) return false;
    model.dependencies = json.at("dependencies").get<std::vector<std::string>>();
    for (const auto& value : json.at("meshes"))
    {
        if (value.at("vertices").size() > 4'000'000 || value.at("indices").size() > 24'000'000) return false;
        MeshData mesh;
        mesh.name = value.at("name").get<std::string>();
        mesh.material = value.at("material").get<std::int32_t>();
        mesh.indices = value.at("indices").get<std::vector<std::uint32_t>>();
        for (const auto& v : value.at("vertices"))
            mesh.vertices.push_back({ v.at("p").get<std::array<float, 3>>(), v.at("n").get<std::array<float, 3>>(),
                v.at("uv").get<std::array<float, 2>>(), v.at("t").get<std::array<float, 4>>() });
        model.meshes.push_back(std::move(mesh));
    }
    for (const auto& value : json.at("materials"))
        model.materials.push_back({ value.at("name").get<std::string>(), value.at("baseColor").get<std::array<float, 4>>(),
            value.at("metallic").get<float>(), value.at("roughness").get<float>(), value.at("baseColorTexture").get<std::int32_t>(),
            value.at("normalTexture").get<std::int32_t>(), value.at("metallicRoughnessTexture").get<std::int32_t>(),
            value.at("doubleSided").get<bool>(), value.at("needsFallbackTexture").get<bool>(), value.value("normalScale",1.0f) });
    for (const auto& value : json.at("nodes"))
        model.nodes.push_back({ value.at("name").get<std::string>(), value.at("parent").get<std::int32_t>(),
            value.at("matrix").get<std::array<float, 16>>(), value.at("meshes").get<std::vector<std::uint32_t>>() });
    for (const auto& value : json.at("textures"))
        model.textures.push_back({ value.at("path").get<std::string>(), value.at("mime").get<std::string>(),
            value.at("embedded").get<std::vector<std::uint8_t>>(), value.at("missing").get<bool>() });
    return ValidateStaticModel(model);
}
}

bool DecodeImage(std::span<const std::uint8_t> encoded, ImageData& output, std::vector<AssetDiagnostic>& diagnostics, std::uint64_t maximumRgbaBytes)
{
    try
    {
    if (encoded.empty() || encoded.size() > asset_io::MaxFileBytes)
    { Diagnostic(diagnostics, "Assets.ImageDecodeFailed", "encoded image size"); return false; }
    struct Apartment
    {
        HRESULT result = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
        ~Apartment() { if (SUCCEEDED(result)) CoUninitialize(); }
    } apartment;
    if (FAILED(apartment.result) && apartment.result != RPC_E_CHANGED_MODE)
    { Diagnostic(diagnostics, "Assets.ImageDecodeFailed", "COM initialization"); return false; }
    using Microsoft::WRL::ComPtr;
    ComPtr<IWICImagingFactory> factory;
    ComPtr<IWICStream> stream;
    ComPtr<IWICBitmapDecoder> decoder;
    ComPtr<IWICBitmapFrameDecode> frame;
    ComPtr<IWICFormatConverter> converter;
    ImageData image;
    if (FAILED(CoCreateInstance(CLSID_WICImagingFactory, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(factory.GetAddressOf())))
        || FAILED(factory->CreateStream(&stream))
        || FAILED(stream->InitializeFromMemory(const_cast<BYTE*>(encoded.data()), static_cast<DWORD>(encoded.size())))
        || FAILED(factory->CreateDecoderFromStream(stream.Get(), nullptr, WICDecodeMetadataCacheOnLoad, &decoder))
        || FAILED(decoder->GetFrame(0, &frame)) || FAILED(frame->GetSize(&image.width, &image.height))
        || image.width == 0 || image.height == 0 || static_cast<std::uint64_t>(image.width) * image.height > 64ull * 1024 * 1024)
    { Diagnostic(diagnostics, "Assets.ImageDecodeFailed"); return false; }
    if (static_cast<std::uint64_t>(image.width) * image.height * 4 > maximumRgbaBytes)
    { Diagnostic(diagnostics, "Assets.ImageBudgetExceeded"); return false; }
    if (FAILED(factory->CreateFormatConverter(&converter))
        || FAILED(converter->Initialize(frame.Get(), GUID_WICPixelFormat32bppRGBA, WICBitmapDitherTypeNone,
            nullptr, 0, WICBitmapPaletteTypeCustom)))
    { Diagnostic(diagnostics, "Assets.ImageDecodeFailed"); return false; }
    image.rgba8.resize(static_cast<std::size_t>(image.width) * image.height * 4);
    if (FAILED(converter->CopyPixels(nullptr, image.width * 4, static_cast<UINT>(image.rgba8.size()), image.rgba8.data())))
    { Diagnostic(diagnostics, "Assets.ImageDecodeFailed"); return false; }
    output = std::move(image);
    return true;
    }
    catch (const std::exception& error)
    { Diagnostic(diagnostics, "Assets.ImageDecodeFailed", error.what()); return false; }
}

bool DecodeImage(const std::filesystem::path& path, ImageData& output, std::vector<AssetDiagnostic>& diagnostics, std::uint64_t maximumRgbaBytes)
{
    std::vector<std::uint8_t> bytes;
    if (!asset_io::Read(path, bytes))
    { Diagnostic(diagnostics, "Assets.ImageDecodeFailed", asset_io::Utf8(path)); return false; }
    return DecodeImage(bytes, output, diagnostics, maximumRgbaBytes);
}

AssetStore::AssetStore(std::filesystem::path sourceRoot, std::filesystem::path cacheRoot)
    : sourceRoot_(std::filesystem::weakly_canonical(std::move(sourceRoot))),
      cacheRoot_(std::filesystem::weakly_canonical(std::move(cacheRoot))) {}

bool AssetStore::DescribeSource(const AssetId& id, AssetSourceSnapshot& output,
    std::vector<AssetDiagnostic>& diagnostics) const
{
    const auto found = records_.find(id);
    if (found == records_.end()) { Diagnostic(diagnostics, "Assets.UnknownId", id); return false; }
    output = {id, found->second.source, sourceRoot_, cacheRoot_, found->second.options};
    return true;
}

std::shared_ptr<const StaticModel> AssetStore::LoadModelSnapshot(const AssetSourceSnapshot& source,
    std::vector<AssetDiagnostic>& diagnostics)
{
    if (!ValidId(source.id) || source.sourceRoot.empty() || source.cacheRoot.empty()
        || !asset_io::Within(source.sourceRoot, source.source))
    { Diagnostic(diagnostics, "Assets.ReferenceOutsideRoot", source.id); return {}; }
    AssetStore worker(source.sourceRoot, source.cacheRoot);
    auto options = source.options; options.allowedRoot = worker.sourceRoot_;
    worker.records_.emplace(source.id, Record{source.source, std::move(options)});
    return worker.LoadModel(source.id, diagnostics);
}

std::shared_ptr<const ImageData> AssetStore::LoadImageSnapshot(const AssetSourceSnapshot& source,
    std::vector<AssetDiagnostic>& diagnostics, std::uint64_t maximumRgbaBytes)
{
    if (!ValidId(source.id) || source.sourceRoot.empty() || source.cacheRoot.empty()
        || !asset_io::Within(source.sourceRoot, source.source))
    { Diagnostic(diagnostics, "Assets.ReferenceOutsideRoot", source.id); return {}; }
    AssetStore worker(source.sourceRoot, source.cacheRoot);
    auto options = source.options; options.allowedRoot = worker.sourceRoot_;
    worker.records_.emplace(source.id, Record{source.source, std::move(options)});
    return worker.LoadTextureImage(source.id, diagnostics, maximumRgbaBytes);
}

bool AssetStore::Scan(std::vector<AssetDiagnostic>& diagnostics)
{
    try
    {
        std::unordered_map<AssetId, Record> found;
        if (!std::filesystem::is_directory(sourceRoot_))
        { Diagnostic(diagnostics, "Assets.ReadFailed", asset_io::Utf8(sourceRoot_)); return false; }
        for (const auto& entry : std::filesystem::recursive_directory_iterator(sourceRoot_))
        {
            if (!entry.is_regular_file()) continue;
            const auto filename = asset_io::Utf8(entry.path().filename());
            constexpr std::string_view suffix = ".asset.json";
            if (!filename.ends_with(suffix)) continue;
            const auto source = entry.path().parent_path() / asset_io::FromUtf8(filename.substr(0, filename.size() - suffix.size()));
            AssetId id; ImportOptions options;
            if (!asset_io::Within(sourceRoot_,entry.path())||!asset_io::Within(sourceRoot_,source)||!ReadMetadata(source,id,options))
            { Diagnostic(diagnostics, "Assets.InvalidMetadata", asset_io::Utf8(entry.path())); return false; }
            if (found.contains(id))
            { Diagnostic(diagnostics, "Assets.DuplicateId", id); return false; }
            options.allowedRoot = sourceRoot_;
            found.emplace(id, Record{ source, options });
        }
        records_ = std::move(found);
        return true;
    }
    catch (const std::exception& error)
    { Diagnostic(diagnostics, "Assets.InvalidMetadata", error.what()); return false; }
}

bool AssetStore::RegisterSource(const std::filesystem::path& requested, const ImportOptions& requestedOptions,
    AssetId& output, std::vector<AssetDiagnostic>& diagnostics)
{
    try
    {
        const auto source = std::filesystem::weakly_canonical(requested.is_absolute() ? requested : sourceRoot_ / requested);
        if (!asset_io::Within(sourceRoot_, source) || !std::filesystem::is_regular_file(source))
        { Diagnostic(diagnostics, "Assets.ReadFailed", asset_io::Utf8(source)); return false; }
        if(!asset_io::Within(sourceRoot_,Metadata(source))){Diagnostic(diagnostics,"Assets.ReferenceOutsideRoot");return false;}
        if (!std::isfinite(requestedOptions.scale) || requestedOptions.scale <= 0)
        { Diagnostic(diagnostics, "Assets.InvalidOptions"); return false; }
        ImportOptions options = requestedOptions;
        options.allowedRoot = sourceRoot_;
        AssetId id;
        if (std::filesystem::exists(Metadata(source)))
        {
            ImportOptions previous;
            if (!ReadMetadata(source, id, previous))
            { Diagnostic(diagnostics, "Assets.InvalidMetadata", asset_io::Utf8(Metadata(source))); return false; }
        }
        else id = asset_io::NewId();
        if (!ValidId(id)) { Diagnostic(diagnostics, "Assets.IdCreationFailed"); return false; }
        if (const auto found = records_.find(id); found != records_.end()
            && !SamePath(found->second.source,source) && std::filesystem::exists(found->second.source))
        { Diagnostic(diagnostics, "Assets.DuplicateId", id); return false; }
        if (!WriteJson(Metadata(source), { { "version", 1 }, { "id", id }, { "options", OptionsJson(options) } }))
        { Diagnostic(diagnostics, "Assets.WriteFailed", asset_io::Utf8(Metadata(source))); return false; }
        records_[id] = { source, options };
        output = id;
        return true;
    }
    catch (const std::exception& error)
    { Diagnostic(diagnostics, "Assets.InvalidMetadata", error.what()); return false; }
}

std::shared_ptr<const StaticModel> AssetStore::LoadModel(const AssetId& id, std::vector<AssetDiagnostic>& diagnostics)
{
    const auto record = records_.find(id);
    if (record == records_.end()) { Diagnostic(diagnostics, "Assets.UnknownId", id); return {}; }
    try
    {
        const auto manifestPath = cacheRoot_ / (id + ".manifest.json");
        auto manifest = ReadJson(manifestPath);
        std::vector<std::string> dependencies;
        try
        {
            if (manifest.is_object() && manifest.value("version", 0u) == StaticModelFormatVersion)
            {
                (void)manifest.at("key").get<std::string>();
                dependencies = manifest.at("dependencies").get<std::vector<std::string>>();
            }
            else manifest = Json();
        }
        catch (const std::exception&)
        {
            manifest = Json();
            Diagnostic(diagnostics, "Assets.CacheRebuilt", id, true);
        }
        std::string key = CacheKey(record->second.source, record->second.options, dependencies);
        if (key.empty() && !dependencies.empty())
        {
            // 派生清单可能损坏；重新解析源文件才能决定实际依赖是否有效。
            dependencies.clear(); manifest = Json();
            key = CacheKey(record->second.source, record->second.options, dependencies);
            Diagnostic(diagnostics, "Assets.CacheRebuilt", id, true);
        }
        if (key.empty()) { Diagnostic(diagnostics, "Assets.ReadFailed", asset_io::Utf8(record->second.source)); return {}; }
        if (const auto loaded = models_.find(id); loaded != models_.end() && loaded->second.key == key)
            if (auto shared = loaded->second.data.lock()) return shared;

        const auto cachePath = cacheRoot_ / (id + ".model.json");
        if (manifest.is_object() && manifest.value("key", std::string{}) == key)
        {
            try
            {
                const auto cached = ReadJson(cachePath);
                auto model = std::make_shared<StaticModel>();
                const std::string payload = cached.is_object() && cached.contains("model") ? cached.at("model").dump() : "";
                const auto payloadHash = asset_io::Hash({ reinterpret_cast<const std::uint8_t*>(payload.data()), payload.size() });
                if (cached.is_object() && cached.value("version", 0u) == StaticModelFormatVersion
                    && cached.value("importer", std::string{}) == StaticModelImporterVersion
                    && !payloadHash.empty() && cached.value("payloadHash", std::string{}) == payloadHash
                    && cached.value("key", std::string{}) == key && Decode(cached.at("model"), *model)
                    && model->dependencies == dependencies)
                {
                    for (const auto& texture : model->textures)
                        if (!texture.relativePath.empty())
                        {
                            std::filesystem::path resolved; std::string relative;
                            if (!asset_io::ResolveRelative(record->second.source, sourceRoot_, texture.relativePath, resolved, relative)) throw std::runtime_error("cache texture path");
                        }
                    models_[id] = { key, model };
                    return model;
                }
            }
            catch (const std::exception&) {}
            Diagnostic(diagnostics, "Assets.CacheRebuilt", id, true);
        }
        auto model = std::make_shared<StaticModel>();
        const auto importDiagnosticStart=diagnostics.size();
        if (!ImportStaticModel(record->second.source, record->second.options, *model, diagnostics)) return {};
        auto importedKey = CacheKey(record->second.source, record->second.options, model->dependencies);
        if (importedKey.empty()) { Diagnostic(diagnostics, "Assets.ReadFailed", id); return {}; }
        if(model->dependencies!=dependencies||importedKey!=key)
        {
            // 首次发现外部依赖或导入期间源发生变化：以完整键再读一次，保证键与 CPU 内容对应。
            auto stable=std::make_shared<StaticModel>();
            diagnostics.resize(importDiagnosticStart);
            if(!ImportStaticModel(record->second.source,record->second.options,*stable,diagnostics))return {};
            const auto stableKey=CacheKey(record->second.source,record->second.options,stable->dependencies);
            if(stableKey.empty()||stable->dependencies!=model->dependencies||stableKey!=importedKey)
            {Diagnostic(diagnostics,"Assets.SourceChanged",id);return {};}
            model=std::move(stable);
        }
        key=std::move(importedKey);
        auto encoded = Encode(*model);
        const auto payload = encoded.dump();
        const auto payloadHash = asset_io::Hash({ reinterpret_cast<const std::uint8_t*>(payload.data()), payload.size() });
        const Json cached = { { "version", StaticModelFormatVersion }, { "importer", StaticModelImporterVersion },
            { "key", key }, { "payloadHash", payloadHash }, { "model", std::move(encoded) } };
        if (!WriteJson(cachePath, cached) || !WriteJson(manifestPath,
            { { "version", StaticModelFormatVersion }, { "key", key }, { "dependencies", model->dependencies } }))
            Diagnostic(diagnostics, "Assets.CacheWriteFailed", id, true);
        models_[id] = { key, model };
        return model;
    }
    catch (const std::exception& error)
    { Diagnostic(diagnostics, "Assets.InvalidModel", error.what()); return {}; }
}

std::shared_ptr<const ImageData> AssetStore::LoadTextureImage(const AssetId& id, std::vector<AssetDiagnostic>& diagnostics, std::uint64_t maximumRgbaBytes)
{
    const auto record = records_.find(id);
    if (record == records_.end()) { Diagnostic(diagnostics, "Assets.UnknownId", id); return {}; }
    std::vector<std::uint8_t> bytes;
    if (!asset_io::Read(record->second.source, bytes))
    { Diagnostic(diagnostics, "Assets.ReadFailed", asset_io::Utf8(record->second.source)); return {}; }
    const auto key = asset_io::Hash(bytes);
    if (key.empty()) { Diagnostic(diagnostics, "Assets.ReadFailed", id); return {}; }
    if (const auto found = images_.find(id); found != images_.end() && found->second.key == key)
        if (auto existing = found->second.data.lock())
        {
            if (existing->rgba8.size() > maximumRgbaBytes) { Diagnostic(diagnostics, "Assets.ImageBudgetExceeded", id); return {}; }
            return existing;
        }
    auto image = std::make_shared<ImageData>();
    if (!DecodeImage(bytes, *image, diagnostics, maximumRgbaBytes)) return {};
    images_[id] = { key, image };
    return image;
}

std::filesystem::path AssetStore::Resolve(const AssetId& id) const
{
    const auto found = records_.find(id);
    return found == records_.end() ? std::filesystem::path{} : found->second.source;
}
std::shared_ptr<const MaterialDocument> AssetStore::LoadMaterial(const AssetId& id,std::vector<AssetDiagnostic>& diagnostics)
{
    const auto record=records_.find(id);
    if(record==records_.end()){Diagnostic(diagnostics,"Assets.UnknownId",id);return {};}
    std::vector<std::uint8_t> bytes;
    if(!IsMaterialDocumentPath(record->second.source))
    {Diagnostic(diagnostics,"Assets.MaterialPathInvalid",asset_io::Utf8(record->second.source));return {};}
    if(!asset_io::Read(record->second.source,bytes)){Diagnostic(diagnostics,"Assets.ReadFailed",id);return {};}
    const auto key=asset_io::Hash(bytes);if(key.empty())return {};
    if(const auto found=materials_.find(id);found!=materials_.end()&&found->second.key==key)
        if(auto shared=found->second.data.lock())return shared;
    auto material=std::make_shared<MaterialDocument>();
    if(!DeserializeMaterialDocument(std::string_view(reinterpret_cast<const char*>(bytes.data()),bytes.size()),*material,diagnostics))return {};
    materials_[id]={key,material};return material;
}
bool AssetStore::LoadShaderArtifact(const AssetId& id,ShaderArtifactSource& output,std::vector<AssetDiagnostic>& diagnostics) const
{
    const auto record=records_.find(id);
    if(record==records_.end()){Diagnostic(diagnostics,"Assets.UnknownId",id);return false;}
    auto name=asset_io::Utf8(record->second.source.filename());
    std::transform(name.begin(),name.end(),name.begin(),[](unsigned char c){return static_cast<char>(std::tolower(c));});
    // 只接受 A01.01 已固定的单文件容器；不把 HLSL 当成 Shader 产物，也不猜测源路径。
    constexpr std::string_view suffix=".sebin";
    constexpr std::string_view magic="SESHDR1";
    if(name.size()<=suffix.size()||!name.ends_with(suffix))
    {Diagnostic(diagnostics,"Assets.ShaderArtifactInvalid",asset_io::Utf8(record->second.source));return false;}
    std::vector<std::uint8_t> bytes;
    if(!asset_io::Read(record->second.source,bytes)||bytes.size()<magic.size()||std::memcmp(bytes.data(),magic.data(),magic.size()))
    {Diagnostic(diagnostics,"Assets.ShaderArtifactInvalid",asset_io::Utf8(record->second.source));return false;}
    const auto key=asset_io::Hash(bytes);
    if(key.empty()){Diagnostic(diagnostics,"Assets.ShaderArtifactInvalid",id);return false;}
    output={record->second.source,key};return true;
}
bool AssetStore::SaveMaterialSource(const std::filesystem::path& requested,const MaterialDocument& material,
    AssetId& id,std::vector<AssetDiagnostic>& diagnostics)
{
    try
    {
        const auto path=std::filesystem::weakly_canonical(requested.is_absolute()?requested:sourceRoot_/requested);
        if(!asset_io::Within(sourceRoot_,path))
        {Diagnostic(diagnostics,"Assets.ReferenceOutsideRoot",asset_io::Utf8(path));return false;}
        if(!IsMaterialDocumentPath(path))
        {Diagnostic(diagnostics,"Assets.MaterialPathInvalid",asset_io::Utf8(path));return false;}
        if(!ValidateMaterialDocument(material)){Diagnostic(diagnostics,"Assets.InvalidMaterial");return false;}
        // 先验证所有依赖类型和已存在的元数据，再修改源文件。
        for(const auto& texture:MaterialTextureDependencies(material))if(!LoadTextureImage(texture,diagnostics))return false;
        if(!material.shader.empty()){ShaderArtifactSource artifact;if(!LoadShaderArtifact(material.shader,artifact,diagnostics))return false;}
        const auto metadataPath=Metadata(path);
        if(!asset_io::Within(sourceRoot_,metadataPath)){Diagnostic(diagnostics,"Assets.ReferenceOutsideRoot");return false;}
        const bool hadSource=std::filesystem::exists(path),hadMetadata=std::filesystem::exists(metadataPath);
        AssetId targetId;ImportOptions options;options.allowedRoot=sourceRoot_;
        if(hadMetadata)
        {
            if(!ReadMetadata(path,targetId,options)){Diagnostic(diagnostics,"Assets.InvalidMetadata",asset_io::Utf8(metadataPath));return false;}
            options.allowedRoot=sourceRoot_;const auto existing=records_.find(targetId);
            if(existing!=records_.end()&&!SamePath(existing->second.source,path)&&std::filesystem::exists(existing->second.source))
            {Diagnostic(diagnostics,"Assets.DuplicateId",targetId);return false;}
        }
        else
        {
            if(hadSource){Diagnostic(diagnostics,"Assets.MaterialUnregistered",asset_io::Utf8(path));return false;}
            targetId=asset_io::NewId();if(!ValidId(targetId)||records_.contains(targetId)){Diagnostic(diagnostics,"Assets.IdCreationFailed");return false;}
        }
        std::string encoded;if(!SerializeMaterialDocument(material,encoded,diagnostics))return false;
        const std::string metadataText=Json{{"version",1},{"id",targetId},{"options",OptionsJson(options)}}.dump(2);
        Record committedRecord{path,options};AssetId committedId=targetId;
        bool inserted=false,createdMetadata=false;auto record=records_.end();
        struct Rollback
        {
            std::function<void()> action;bool committed=false;
            ~Rollback(){if(!committed)try{action();}catch(...){}}
        } rollback{[&]{
            if(inserted)records_.erase(targetId);
            if(createdMetadata)
            {
                std::error_code error;
                if(!std::filesystem::is_symlink(metadataPath,error)&&std::filesystem::file_size(metadataPath,error)==metadataText.size())
                {
                    std::vector<std::uint8_t> current;
                    if(asset_io::Read(metadataPath,current)&&std::string(current.begin(),current.end())==metadataText)std::filesystem::remove(metadataPath,error);
                }
            }
        }};
        const auto prepared=records_.try_emplace(targetId,committedRecord);record=prepared.first;inserted=prepared.second;
        if(!hadMetadata)
        {
            if(!asset_io::WriteAtomic(metadataPath,{reinterpret_cast<const std::uint8_t*>(metadataText.data()),metadataText.size()},false))
            {Diagnostic(diagnostics,"Assets.WriteFailed",asset_io::Utf8(metadataPath));return false;}
            createdMetadata=true;
        }
        // 既有 sidecar 不重写；源文件是提交点，新目标不覆盖并发出现的文件。
        if(!asset_io::WriteAtomic(path,{reinterpret_cast<const std::uint8_t*>(encoded.data()),encoded.size()},hadSource))
        {Diagnostic(diagnostics,"Assets.WriteFailed",asset_io::Utf8(path));return false;}
        record->second=std::move(committedRecord);id=std::move(committedId);rollback.committed=true;return true;
    }
    catch(const std::exception& error){Diagnostic(diagnostics,"Assets.InvalidMaterial",error.what());return false;}
}
std::shared_ptr<const EnvironmentAsset> AssetStore::LoadEnvironment(const AssetId& id,std::vector<AssetDiagnostic>& diagnostics)
{
    const auto record=records_.find(id);if(record==records_.end()){Diagnostic(diagnostics,"Assets.UnknownId",id);return {};}
    if(record->second.source.extension()!=L".seenv"){Diagnostic(diagnostics,"Environment.ContainerInvalid",id);return {};}
    std::vector<std::uint8_t> bytes;if(!asset_io::Read(record->second.source,bytes)){Diagnostic(diagnostics,"Assets.ReadFailed",id);return {};}
    const auto key=asset_io::Hash(bytes);if(key.empty()){Diagnostic(diagnostics,"Assets.ReadFailed",id);return {};}
    const auto previous=environments_.find(id);
    if(previous!=environments_.end()&&previous->second.key==key)if(auto existing=previous->second.data.lock())return existing;
    auto candidate=std::make_shared<EnvironmentAsset>();if(!DecodeEnvironment(bytes,*candidate,diagnostics))return {};
    environments_.insert_or_assign(id,EnvironmentEntry{key,candidate});return candidate;
}
std::vector<AssetId> AssetStore::ListIds() const
{
    std::vector<AssetId> ids;
    for (const auto& [id, record] : records_) { (void)record; ids.push_back(id); }
    std::sort(ids.begin(), ids.end());
    return ids;
}
std::shared_ptr<const TODProfile> AssetStore::LoadTODProfile(const AssetId& id,std::vector<AssetDiagnostic>& diagnostics)
{
    const auto record=records_.find(id);if(record==records_.end()){Diagnostic(diagnostics,"Assets.UnknownId",id);return {};}
    if(record->second.source.extension()!=L".tod"){Diagnostic(diagnostics,"TOD.InvalidProfile",id);return {};}
    std::error_code sizeError;const auto size=std::filesystem::file_size(record->second.source,sizeError);
    if(sizeError||size>1024*1024){Diagnostic(diagnostics,"TOD.ReadFailed",id);return {};}
    std::vector<std::uint8_t> bytes;if(!asset_io::Read(record->second.source,bytes)||bytes.size()>1024*1024){Diagnostic(diagnostics,"TOD.ReadFailed",id);return {};}
    const auto key=asset_io::Hash(bytes);if(key.empty()){Diagnostic(diagnostics,"TOD.ReadFailed",id);return {};}
    const auto previous=todProfiles_.find(id);if(previous!=todProfiles_.end()&&previous->second.key==key)if(auto existing=previous->second.data.lock())return existing;
    auto candidate=std::make_shared<TODProfile>();std::string error;
    if(!ParseTODProfile(std::string_view(reinterpret_cast<const char*>(bytes.data()),bytes.size()),*candidate,error)){Diagnostic(diagnostics,error.c_str(),id);return {};}
    todProfiles_.insert_or_assign(id,TODEntry{key,candidate});return candidate;
}
void AssetStore::CollectUnused()
{
    std::erase_if(models_, [](const auto& value) { return value.second.data.expired(); });
    std::erase_if(images_, [](const auto& value) { return value.second.data.expired(); });
    std::erase_if(materials_, [](const auto& value) { return value.second.data.expired(); });
    std::erase_if(environments_, [](const auto& value) { return value.second.data.expired(); });
    std::erase_if(todProfiles_, [](const auto& value) { return value.second.data.expired(); });
}
}
