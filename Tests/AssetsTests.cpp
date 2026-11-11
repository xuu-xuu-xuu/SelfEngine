#include "ResourceLayer/Assets/AssetStore.h"
#include "ResourceLayer/Assets/AssetIO.h"
#include "ResourceLayer/Assets/AssetTasks.h"
#include <nlohmann/json.hpp>
#include <algorithm>
#include <cmath>
#include <chrono>
#include <fstream>
#include <iostream>
#include <thread>
#include <Windows.h>

#define CHECK(condition) do { if (!(condition)) { std::cerr << "FAIL line " << __LINE__ << ": " << #condition << '\n'; for (const auto& d : diagnostics) std::cerr << d.key << ": " << d.detail << '\n'; return 1; } } while (false)
namespace fs = std::filesystem;
using Json = nlohmann::json;

static Json ReadJson(const fs::path& path) { std::ifstream input(path); return Json::parse(input); }
static void WriteJson(const fs::path& path, const Json& json) { std::ofstream(path) << json.dump(2); }
static bool Near(float a, float b) { return std::abs(a - b) < 0.0001f; }
static bool Has(const std::vector<se::AssetDiagnostic>& diagnostics, const std::string& key)
{ return std::any_of(diagnostics.begin(), diagnostics.end(), [&](const auto& d) { return d.key == key; }); }

int wmain(int argc, wchar_t** argv)
{
    std::vector<se::AssetDiagnostic> diagnostics;
    const auto samples = fs::absolute(argc > 1 ? fs::path(argv[1]) : fs::path("Resources/Samples/Models"));
    const auto scratch = fs::absolute(argc > 2 ? fs::path(argv[2]) : fs::path("out/validation/assets")) / se::asset_io::NewId();
    const auto source = scratch / L"资源";
    const auto cache = scratch / "cache";
    fs::create_directories(source);
    for (const auto& entry : fs::directory_iterator(samples))
        if (entry.is_regular_file()) fs::copy_file(entry.path(), source / entry.path().filename());
    const auto originalJson = ReadJson(source / "Hierarchy.gltf");

    // 三种真实解析器入口必须得到独立的 CPU 数据、层级、米制几何及外部贴图。
    for (const auto* filename : { "Hierarchy.fbx", "Hierarchy.gltf", "Hierarchy.glb" })
    {
        se::StaticModel model;
        diagnostics.clear();
        CHECK(se::ImportStaticModel(source / filename, {}, model, diagnostics));
        CHECK(diagnostics.empty() && se::ValidateStaticModel(model));
        CHECK(model.meshes.size() == 1 && model.meshes[0].indices.size() == 3);
        CHECK(model.materials.size() == 1 && model.textures.size() == 1);
        CHECK(model.textures[0].relativePath == "checker.png" && !model.textures[0].missing);
        CHECK(model.materials[0].baseColorTexture == 0 && !model.materials[0].needsFallbackTexture);
        const auto parent = std::find_if(model.nodes.begin(), model.nodes.end(), [](const auto& node) { return node.name == "Parent"; });
        const auto child = std::find_if(model.nodes.begin(), model.nodes.end(), [](const auto& node) { return node.name == "Triangle"; });
        CHECK(parent != model.nodes.end() && child != model.nodes.end());
        CHECK(child->parent == parent - model.nodes.begin() && Near(parent->localMatrix[12], 1));
        CHECK(Near(parent->localMatrix[0], 1) && Near(parent->localMatrix[10], 1));
        const auto& mesh = model.meshes[0];
        CHECK(std::any_of(mesh.vertices.begin(), mesh.vertices.end(), [](const auto& v) { return Near(v.position[0], 1) && Near(v.position[1], 0); }));
        CHECK(std::any_of(mesh.vertices.begin(), mesh.vertices.end(), [](const auto& v) { return Near(v.position[0], 0) && Near(v.position[1], 2); }));
        CHECK(Near(mesh.vertices[0].normal[2], 1) && Near(mesh.vertices[0].uv[1], 0));
        CHECK(Near(mesh.vertices[0].tangent[0], 1));
        // 导入函数内部解析树已销毁；这些访问只依赖自有数据。
        se::StaticModel invalid = model;
        invalid.nodes[0].parent = 0;
        CHECK(!se::ValidateStaticModel(invalid));
    }
    se::StaticModel model;
    se::ImportOptions scaled; scaled.scale = 2;
    CHECK(se::ImportStaticModel(source / "Hierarchy.glb", scaled, model, diagnostics));
    CHECK(Near(model.nodes[0].localMatrix[12], 2) && Near(model.meshes[0].vertices[1].position[0], 2));
    const auto savedName = model.nodes[0].name;
    std::ofstream(source / "broken.glb") << "broken";
    CHECK(!se::ImportStaticModel(source / "broken.glb", {}, model, diagnostics) && model.nodes[0].name == savedName);
    CHECK(!se::ImportStaticModel(source / "absent.glb", {}, model, diagnostics));

    auto edited = originalJson;
    edited["animations"] = Json::array({ { { "samplers", Json::array() }, { "channels", Json::array() } } });
    WriteJson(source / "animated.gltf", edited); diagnostics.clear();
    CHECK(!se::ImportStaticModel(source / "animated.gltf", {}, model, diagnostics));
    CHECK(Has(diagnostics, "Assets.UnsupportedAnimation"));
    edited = originalJson;
    edited["meshes"][0]["primitives"][0]["targets"] = Json::array({ { { "POSITION", 0 } } });
    WriteJson(source / "morph.gltf", edited); diagnostics.clear();
    CHECK(!se::ImportStaticModel(source / "morph.gltf", {}, model, diagnostics));
    CHECK(Has(diagnostics, "Assets.UnsupportedFeature"));
    edited = originalJson; edited["images"][0]["uri"] = "missing.png";
    WriteJson(source / "missing.gltf", edited); diagnostics.clear();
    CHECK(se::ImportStaticModel(source / "missing.gltf", {}, model, diagnostics));
    CHECK(Has(diagnostics, "Assets.MissingTexture") && model.materials[0].needsFallbackTexture);
    edited["images"][0]["uri"] = "../outside.png";
    WriteJson(source / "escape.gltf", edited); diagnostics.clear();
    CHECK(!se::ImportStaticModel(source / "escape.gltf", {}, model, diagnostics));
    CHECK(Has(diagnostics, "Assets.ReferenceOutsideRoot"));
    edited = originalJson; edited["accessors"][0]["count"] = 9999;
    WriteJson(source / "bad-accessor.gltf", edited);
    CHECK(!se::ImportStaticModel(source / "bad-accessor.gltf", {}, model, diagnostics));
    se::ImageData image;
    CHECK(se::DecodeImage(source / "checker.png", image, diagnostics));
    CHECK(image.width == 2 && image.height == 2 && image.rgba8.size() == 16);
    CHECK(image.rgba8[4] == 255 && image.rgba8[5] == 0 && image.rgba8[6] == 0 && image.rgba8[7] == 255);
    CHECK(!se::DecodeImage(source / "broken.glb", image, diagnostics) && image.width == 2);
    std::vector<std::uint8_t> embedded,buffer;
    CHECK(se::asset_io::Read(source / "checker.png",embedded)&&se::asset_io::Read(source / "Hierarchy.bin",buffer));
    while(buffer.size()%4)buffer.push_back(0);
    const auto imageOffset=buffer.size();buffer.insert(buffer.end(),embedded.begin(),embedded.end());
    CHECK(se::asset_io::WriteAtomic(source / "embedded.bin",buffer));
    edited=originalJson;edited["buffers"][0]["uri"]="embedded.bin";edited["buffers"][0]["byteLength"]=buffer.size();
    edited["bufferViews"].push_back({{"buffer",0},{"byteOffset",imageOffset},{"byteLength",embedded.size()}});
    edited["images"][0]={{"bufferView",4},{"mimeType","image/png"}};
    WriteJson(source / "embedded.gltf",edited);
    CHECK(se::ImportStaticModel(source / "embedded.gltf",{},model,diagnostics));
    CHECK(model.textures[0].embeddedBytes==embedded&&model.textures[0].relativePath.empty());
    CHECK(se::DecodeImage(model.textures[0].embeddedBytes,image,diagnostics)&&image.width==2);
    // URI 解码只执行一次；CPU 缓存中的相对路径保留真实 '%' 字符。
    fs::copy_file(source / "Hierarchy.bin",source / "buffer%20.bin");
    edited=originalJson;edited["buffers"][0]["uri"]="buffer%2520.bin";
    WriteJson(source / "percent.gltf",edited);
    se::AssetStore percentStore(source,cache/"percent");se::AssetId percentId;
    CHECK(percentStore.RegisterSource("percent.gltf",{},percentId,diagnostics));
    CHECK(percentStore.LoadModel(percentId,diagnostics));

    se::TaskQueue tasks(1);
    const auto owner = std::this_thread::get_id();
    bool modelPublished = false, imagePublished = false, wrongThread = false, cancelledPublished = false;
    se::ImportModelAsync(tasks, source / "Hierarchy.glb", {}, [&](auto result) {
        modelPublished = result.data && result.data->meshes.size() == 1;
        wrongThread = wrongThread || std::this_thread::get_id() != owner;
    });
    se::DecodeImageAsync(tasks, source / "checker.png", [&](auto result) {
        imagePublished = result.data && result.data->width == 2;
        wrongThread = wrongThread || std::this_thread::get_id() != owner;
    });
    auto cancelled = se::ImportModelAsync(tasks, source / "Hierarchy.glb", {}, [&](auto) { cancelledPublished = true; });
    cancelled.Cancel();
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
    while (tasks.Pending() && std::chrono::steady_clock::now() < deadline)
    { tasks.Pump(); std::this_thread::sleep_for(std::chrono::milliseconds(1)); }
    CHECK(modelPublished && imagePublished && !wrongThread && !cancelledPublished && tasks.Pending() == 0);
    tasks.Shutdown();

    se::AssetStore store(source, cache);
    se::AssetId id, sameId, imageId;
    diagnostics.clear();
    CHECK(store.RegisterSource("Hierarchy.gltf", {}, id, diagnostics) && id.size() == 32);
    CHECK(store.RegisterSource("Hierarchy.gltf", {}, sameId, diagnostics) && id == sameId);
    CHECK(store.RegisterSource("checker.png", {}, imageId, diagnostics));
    auto first = store.LoadModel(id, diagnostics);
    CHECK(first && first == store.LoadModel(id, diagnostics));
    auto pixels = store.LoadTextureImage(imageId, diagnostics);
    CHECK(pixels && pixels == store.LoadTextureImage(imageId, diagnostics));
    se::MaterialDocument material;material.name="CPU材质";material.baseColorTexture=imageId;material.normalTexture=imageId;
    CHECK(se::MaterialDependencies(material).size()==1);
    se::AssetId materialId;
    CHECK(store.SaveMaterialSource("test.material.json",material,materialId,diagnostics));
    auto sharedMaterial=store.LoadMaterial(materialId,diagnostics);
    CHECK(sharedMaterial&&sharedMaterial->name==material.name&&sharedMaterial==store.LoadMaterial(materialId,diagnostics));
    const auto materialJson=ReadJson(source/"test.material.json");
    auto badMaterial=material;badMaterial.metallic=2;
    CHECK(!store.SaveMaterialSource("test.material.json",badMaterial,sameId,diagnostics));
    CHECK(ReadJson(source/"test.material.json")==materialJson);
    CHECK(!store.SaveMaterialSource("Hierarchy.gltf",material,sameId,diagnostics));
    CHECK(ReadJson(source/"Hierarchy.gltf")==originalJson);
    CHECK(!store.SaveMaterialSource(scratch/"outside.material.json",material,sameId,diagnostics));
    CHECK(!fs::exists(scratch/"outside.material.json"));
    badMaterial=material;badMaterial.baseColorTexture=id;
    CHECK(!store.SaveMaterialSource("test.material.json",badMaterial,sameId,diagnostics));
    CHECK(ReadJson(source/"test.material.json")==materialJson);
    badMaterial=material;badMaterial.baseColorTexture=std::string(32,'0');
    CHECK(!store.SaveMaterialSource("test.material.json",badMaterial,sameId,diagnostics));
    material.roughness=0.25f;
    CHECK(store.SaveMaterialSource("test.material.json",material,sameId,diagnostics)&&sameId==materialId);
    auto changedMaterial=store.LoadMaterial(materialId,diagnostics);
    CHECK(changedMaterial&&changedMaterial!=sharedMaterial&&Near(changedMaterial->roughness,0.25f)&&Near(sharedMaterial->roughness,1));
    // A01.02：材质可选用户 Shader 产物（.sebin）的注册、内容身份、依赖与坏输入拒绝。
    {
        const auto artifactPath=source/"user-pbr.sebin";
        {std::ofstream artifact(artifactPath,std::ios::binary);artifact.write("SESHDR1\0\0\0\0",12);}
        se::AssetId shaderId;diagnostics.clear();
        CHECK(store.RegisterSource("user-pbr.sebin",{},shaderId,diagnostics));
        se::ShaderArtifactSource artifactSource;
        CHECK(store.LoadShaderArtifact(shaderId,artifactSource,diagnostics)&&artifactSource.content_sha256.size()==64
            &&artifactSource.source==fs::weakly_canonical(artifactPath));
        se::MaterialDocument shaderMaterial=material;shaderMaterial.shader=shaderId;
        CHECK(se::MaterialDependencies(shaderMaterial).size()==2&&se::MaterialTextureDependencies(shaderMaterial).size()==1);
        se::AssetId shaderMaterialId;
        CHECK(store.SaveMaterialSource("shader.material.json",shaderMaterial,shaderMaterialId,diagnostics));
        const auto storedShader=store.LoadMaterial(shaderMaterialId,diagnostics);
        CHECK(storedShader&&storedShader->shader==shaderId);
        auto badShader=shaderMaterial;badShader.shader=id; // 已注册但不是 .sebin 产物。
        CHECK(!store.SaveMaterialSource("shader.material.json",badShader,shaderMaterialId,diagnostics)&&Has(diagnostics,"Assets.ShaderArtifactInvalid"));
        auto missingShader=shaderMaterial;missingShader.shader=std::string(32,'a');
        CHECK(!store.SaveMaterialSource("shader.material.json",missingShader,shaderMaterialId,diagnostics));
        {std::ofstream broken(artifactPath,std::ios::binary|std::ios::trunc);broken<<"not a container";}
        se::ShaderArtifactSource rejected;
        CHECK(!store.LoadShaderArtifact(shaderId,rejected,diagnostics));
        auto legacyMaterial=ReadJson(source/"shader.material.json");legacyMaterial.erase("shader");
        se::MaterialDocument legacy;CHECK(se::DeserializeMaterialDocument(legacyMaterial.dump(),legacy,diagnostics)&&legacy.shader.empty());
        {std::ofstream restore(artifactPath,std::ios::binary|std::ios::trunc);restore.write("SESHDR1\0\0\0\0",12);}
        CHECK(store.LoadShaderArtifact(shaderId,artifactSource,diagnostics));
    }
    auto unsupportedMaterial=materialJson;unsupportedMaterial["version"]=999;
    se::MaterialDocument preserved=material;
    CHECK(!se::DeserializeMaterialDocument(unsupportedMaterial.dump(),preserved,diagnostics)&&Near(preserved.roughness,0.25f));
    unsupportedMaterial=materialJson;unsupportedMaterial["type"]="SelfEngine.Scene";
    CHECK(!se::DeserializeMaterialDocument(unsupportedMaterial.dump(),preserved,diagnostics));
    auto badMetadata=ReadJson(source/"test.material.json.asset.json");const auto validMetadata=badMetadata;badMetadata["version"]=100;
    WriteJson(source/"test.material.json.asset.json",badMetadata);
    material.roughness=0.5f;
    CHECK(!store.SaveMaterialSource("test.material.json",material,sameId,diagnostics));
    CHECK(Near(ReadJson(source/"test.material.json")["roughness"].get<float>(),0.25f));
    WriteJson(source/"test.material.json.asset.json",validMetadata);
    {
        const auto metadataPath=source/"test.material.json.asset.json";
        struct RestoreAttributes{fs::path path;DWORD attributes;~RestoreAttributes(){SetFileAttributesW(path.c_str(),attributes);}} restore{metadataPath,GetFileAttributesW(metadataPath.c_str())};
        CHECK(SetFileAttributesW(metadataPath.c_str(),restore.attributes|FILE_ATTRIBUTE_READONLY));
        material.roughness=.4f;CHECK(store.SaveMaterialSource("test.material.json",material,sameId,diagnostics)&&sameId==materialId);
        CHECK(ReadJson(metadataPath)==validMetadata&&Near(ReadJson(source/"test.material.json")["roughness"].get<float>(),.4f));
    }
    {
        const auto materialPath=source/"test.material.json";const auto before=ReadJson(materialPath);const auto callerId=sameId;
        struct RestoreAttributes{fs::path path;DWORD attributes;~RestoreAttributes(){SetFileAttributesW(path.c_str(),attributes);}} restore{materialPath,GetFileAttributesW(materialPath.c_str())};
        CHECK(SetFileAttributesW(materialPath.c_str(),restore.attributes|FILE_ATTRIBUTE_READONLY));
        material.roughness=.6f;CHECK(!store.SaveMaterialSource("test.material.json",material,sameId,diagnostics));
        CHECK(ReadJson(materialPath)==before&&ReadJson(source/"test.material.json.asset.json")==validMetadata&&sameId==callerId);
    }
    const auto unregistered=source/"unregistered.material.json";CHECK(se::SaveMaterialDocument(unregistered,material,diagnostics));
    const auto unregisteredBytes=ReadJson(unregistered);material.roughness=.7f;
    CHECK(!store.SaveMaterialSource(unregistered,material,sameId,diagnostics)&&Has(diagnostics,"Assets.MaterialUnregistered")&&ReadJson(unregistered)==unregisteredBytes&&!fs::exists(source/"unregistered.material.json.asset.json"));
    auto manifest = ReadJson(cache / (id + ".manifest.json"));
    const std::string initialKey = manifest["key"];

    // 源内容变化、参数变化、依赖内容变化均产生新键；旧 shared_ptr 继续有效。
    edited = originalJson; edited["nodes"][0]["translation"][0] = 3;
    WriteJson(source / "Hierarchy.gltf", edited);
    auto changed = store.LoadModel(id, diagnostics);
    CHECK(changed && changed != first && Near(changed->nodes[0].localMatrix[12], 3));
    CHECK(Near(first->nodes[0].localMatrix[12], 1));
    CHECK(ReadJson(cache / (id + ".manifest.json"))["key"] != initialKey);
    CHECK(store.RegisterSource("Hierarchy.gltf", scaled, sameId, diagnostics) && sameId == id);
    auto scaledModel = store.LoadModel(id, diagnostics);
    CHECK(scaledModel && Near(scaledModel->nodes[0].localMatrix[12], 6));
    const auto beforeDependency = ReadJson(cache / (id + ".manifest.json"))["key"];
    { std::ofstream append(source / "checker.png", std::ios::binary | std::ios::app); append.put('\0'); }
    auto dependencyChanged = store.LoadModel(id, diagnostics);
    CHECK(dependencyChanged && ReadJson(cache / (id + ".manifest.json"))["key"] != beforeDependency);

    // 新实例读磁盘派生缓存；格式损坏必须重建，不能把派生文件当作源数据。
    auto cacheJson = ReadJson(cache / (id + ".model.json")); cacheJson["version"] = 999;
    WriteJson(cache / (id + ".model.json"), cacheJson);
    se::AssetStore reload(source, cache);
    CHECK(reload.Scan(diagnostics)); diagnostics.clear();
    CHECK(reload.LoadModel(id, diagnostics) && Has(diagnostics, "Assets.CacheRebuilt"));
    cacheJson = ReadJson(cache / (id + ".model.json"));
    cacheJson["model"]["meshes"][0]["vertices"][0]["p"][0] = 999;
    WriteJson(cache / (id + ".model.json"), cacheJson);
    se::AssetStore corruptPayload(source, cache); CHECK(corruptPayload.Scan(diagnostics)); diagnostics.clear();
    CHECK(corruptPayload.LoadModel(id, diagnostics) && Has(diagnostics, "Assets.CacheRebuilt"));
    WriteJson(cache / (id + ".manifest.json"), { { "version", 1 } });
    se::AssetStore corruptManifest(source, cache); CHECK(corruptManifest.Scan(diagnostics));
    CHECK(corruptManifest.LoadModel(id, diagnostics));

    // 模型与 sidecar 一起改名，场景继续通过原 ID 找到它。
    fs::rename(source / "Hierarchy.gltf", source / L"改名.gltf");
    fs::rename(source / "Hierarchy.gltf.asset.json", source / L"改名.gltf.asset.json");
    CHECK(store.Scan(diagnostics));
    CHECK(store.Resolve(id).filename() == L"改名.gltf" && store.LoadModel(id, diagnostics));
    fs::copy_file(source / L"改名.gltf", source / "duplicate.gltf");
    fs::copy_file(source / L"改名.gltf.asset.json", source / "duplicate.gltf.asset.json");
    diagnostics.clear(); CHECK(!store.Scan(diagnostics) && Has(diagnostics, "Assets.DuplicateId"));
    CHECK(store.Resolve(id).filename() == L"改名.gltf");
    fs::remove(source / "duplicate.gltf.asset.json"); // 仅删除本测试本轮创建的单个文件。
    auto metadata = ReadJson(source / L"改名.gltf.asset.json"); metadata["version"] = 100;
    WriteJson(source / L"改名.gltf.asset.json", metadata);
    CHECK(!store.RegisterSource(L"改名.gltf", {}, sameId, diagnostics));
    CHECK(ReadJson(source / L"改名.gltf.asset.json")["version"] == 100);
    CHECK(!store.LoadModel("unknown", diagnostics));
    fs::remove(source / L"改名.gltf");
    CHECK(!store.LoadModel(id, diagnostics));
    store.CollectUnused();
    std::cout << "AssetsTests passed (static FBX/glTF/GLB, WIC, materials, metadata, cache, ownership, invalid inputs).\n";
    return 0;
}
