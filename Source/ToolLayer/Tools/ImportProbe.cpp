#include "ResourceLayer/Assets/StaticModel.h"
#include <nlohmann/json.hpp>
#include <iostream>

// 机器可读的 CPU 导入探针；JSON 字段是协议名，诊断 key 由上层语言表解释。
int wmain(int argc, wchar_t** argv)
{
    using Json = nlohmann::json;
    if (argc != 2)
    {
        std::cout << Json({ { "ok", false }, { "usage", "ImportProbe <model.fbx|model.gltf|model.glb>" } }).dump(2) << '\n';
        return 2;
    }
    se::StaticModel model;
    std::vector<se::AssetDiagnostic> diagnostics;
    const bool success = se::ImportStaticModel(argv[1], {}, model, diagnostics);
    Json report = { { "ok", success }, { "meshes", model.meshes.size() }, { "nodes", model.nodes.size() },
        { "materials", model.materials.size() }, { "textures", model.textures.size() },
        { "dependencies", model.dependencies }, { "diagnostics", Json::array() }, { "nodeHierarchy", Json::array() } };
    std::size_t vertexCount = 0, triangleCount = 0;
    for (const auto& mesh : model.meshes) { vertexCount += mesh.vertices.size(); triangleCount += mesh.indices.size() / 3; }
    report["vertices"] = vertexCount; report["triangles"] = triangleCount;
    for (const auto& node : model.nodes)
        report["nodeHierarchy"].push_back({ { "name", node.name }, { "parent", node.parent }, { "meshes", node.meshes }, { "localMatrix", node.localMatrix } });
    for (const auto& diagnostic : diagnostics)
        report["diagnostics"].push_back({ { "key", diagnostic.key }, { "detail", diagnostic.detail }, { "warning", diagnostic.warning } });
    std::cout << report.dump(2, ' ', true) << '\n';
    return success ? 0 : 1;
}
