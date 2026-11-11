#include "StaticModel.h"
#include "AssetIO.h"
#include <ufbx/ufbx.h>
#include <cgltf/cgltf.h>
#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdlib>
#include <cstring>
#include <limits>
#include <memory>
#include <unordered_map>

namespace se
{
namespace
{
using asset_io::Diagnostic;
using F3 = std::array<float, 3>;
constexpr std::size_t MaxVertices = 4'000'000;
constexpr std::size_t MaxNodes = 100'000;
F3 Sub(F3 a, F3 b) { return { a[0] - b[0], a[1] - b[1], a[2] - b[2] }; }
F3 Add(F3 a, F3 b) { return { a[0] + b[0], a[1] + b[1], a[2] + b[2] }; }
F3 Mul(F3 a, float x) { return { a[0] * x, a[1] * x, a[2] * x }; }
float Dot(F3 a, F3 b) { return a[0] * b[0] + a[1] * b[1] + a[2] * b[2]; }
F3 Cross(F3 a, F3 b) { return { a[1] * b[2] - a[2] * b[1], a[2] * b[0] - a[0] * b[2], a[0] * b[1] - a[1] * b[0] }; }
F3 Normalized(F3 v, F3 fallback = { 0, 1, 0 })
{
    const float length = std::sqrt(Dot(v, v));
    return length > 1e-12f && std::isfinite(length) ? Mul(v, 1.0f / length) : fallback;
}

void GenerateBasis(MeshData& mesh, bool normals, bool tangents)
{
    std::vector<F3> normalSums(mesh.vertices.size()), tangentSums(mesh.vertices.size()), bitangentSums(mesh.vertices.size());
    for (std::size_t triangle = 0; triangle + 2 < mesh.indices.size(); triangle += 3)
    {
        const auto a = mesh.indices[triangle], b = mesh.indices[triangle + 1], c = mesh.indices[triangle + 2];
        const auto& va = mesh.vertices[a]; const auto& vb = mesh.vertices[b]; const auto& vc = mesh.vertices[c];
        const auto e1 = Sub(vb.position, va.position), e2 = Sub(vc.position, va.position);
        const auto n = Cross(e1, e2);
        for (const auto index : { a, b, c }) normalSums[index] = Add(normalSums[index], n);
        const float du1 = vb.uv[0] - va.uv[0], dv1 = vb.uv[1] - va.uv[1];
        const float du2 = vc.uv[0] - va.uv[0], dv2 = vc.uv[1] - va.uv[1];
        const float determinant = du1 * dv2 - dv1 * du2;
        if (std::abs(determinant) > 1e-12f)
        {
            const auto t = Mul(Sub(Mul(e1, dv2), Mul(e2, dv1)), 1.0f / determinant);
            const auto bt = Mul(Sub(Mul(e2, du1), Mul(e1, du2)), 1.0f / determinant);
            for (const auto index : { a, b, c })
            { tangentSums[index] = Add(tangentSums[index], t); bitangentSums[index] = Add(bitangentSums[index], bt); }
        }
    }
    for (std::size_t index = 0; index < mesh.vertices.size(); ++index)
    {
        auto& vertex = mesh.vertices[index];
        vertex.normal = Normalized(normals ? normalSums[index] : vertex.normal);
        if (tangents)
        {
            const auto n = vertex.normal;
            const auto fallback = Normalized(Cross(std::abs(n[1]) < 0.9f ? F3{ 0, 1, 0 } : F3{ 1, 0, 0 }, n), { 1, 0, 0 });
            const auto tangent = Normalized(Sub(tangentSums[index], Mul(n, Dot(n, tangentSums[index]))), fallback);
            vertex.tangent = { tangent[0], tangent[1], tangent[2], Dot(Cross(n, tangent), bitangentSums[index]) < 0 ? -1.0f : 1.0f };
        }
    }
}

bool ValidateModel(const StaticModel& model)
{
    if (model.meshes.empty() || model.nodes.empty() || model.nodes.size() > MaxNodes) return false;
    std::vector<unsigned char> marks(model.nodes.size());
    for (std::size_t index = 0; index < model.nodes.size(); ++index)
    {
        std::int32_t cursor = static_cast<std::int32_t>(index);
        while (cursor >= 0 && marks[static_cast<std::size_t>(cursor)] == 0)
        {
            marks[static_cast<std::size_t>(cursor)] = 1;
            const auto next = model.nodes[static_cast<std::size_t>(cursor)].parent;
            if (next < -1 || next >= static_cast<std::int32_t>(model.nodes.size())) return false;
            cursor = next;
        }
        if (cursor >= 0 && marks[static_cast<std::size_t>(cursor)] == 1) return false;
        cursor = static_cast<std::int32_t>(index);
        while (cursor >= 0 && marks[static_cast<std::size_t>(cursor)] == 1)
        { marks[static_cast<std::size_t>(cursor)] = 2; cursor = model.nodes[static_cast<std::size_t>(cursor)].parent; }
        for (const float v : model.nodes[index].localMatrix) if (!std::isfinite(v)) return false;
        for (const auto mesh : model.nodes[index].meshes) if (mesh >= model.meshes.size()) return false;
    }
    for (const auto& mesh : model.meshes)
    {
        if (mesh.vertices.empty() || mesh.vertices.size() > MaxVertices || mesh.indices.empty() || mesh.indices.size() % 3 != 0) return false;
        if (mesh.material < -1 || mesh.material >= static_cast<std::int32_t>(model.materials.size())) return false;
        for (const auto index : mesh.indices) if (index >= mesh.vertices.size()) return false;
        for (const auto& v : mesh.vertices)
        {
            for (const auto f : v.position) if (!std::isfinite(f)) return false;
            for (const auto f : v.normal) if (!std::isfinite(f)) return false;
            for (const auto f : v.uv) if (!std::isfinite(f)) return false;
            for (const auto f : v.tangent) if (!std::isfinite(f)) return false;
        }
    }
    for (const auto& material : model.materials)
    {
        for (const auto value : material.baseColor) if (!std::isfinite(value)) return false;
        if (!std::isfinite(material.metallic) || !std::isfinite(material.roughness) || !std::isfinite(material.normalScale)) return false;
        for (const auto texture : { material.baseColorTexture, material.normalTexture, material.metallicRoughnessTexture })
            if (texture < -1 || texture >= static_cast<std::int32_t>(model.textures.size())) return false;
    }
    return true;
}

bool AddExternalTexture(StaticModel& output, TextureReference& texture, const std::string& uri,
    const std::filesystem::path& source, const ImportOptions& options, std::vector<AssetDiagnostic>& diagnostics, bool encodedUri = true)
{
    std::filesystem::path resolved;
    if (!(encodedUri ? asset_io::ResolveReference(source, options.allowedRoot, uri, resolved, texture.relativePath)
        : asset_io::ResolveRelative(source, options.allowedRoot, uri, resolved, texture.relativePath)))
    { Diagnostic(diagnostics, "Assets.ReferenceOutsideRoot", uri); return false; }
    texture.missing = !std::filesystem::is_regular_file(resolved);
    output.dependencies.push_back(texture.relativePath);
    if (texture.missing) Diagnostic(diagnostics, "Assets.MissingTexture", texture.relativePath, true);
    return true;
}

struct GltfFileContext { std::filesystem::path root; };
cgltf_result ReadGltfFile(const cgltf_memory_options*, const cgltf_file_options* files,
    const char* path, cgltf_size* size, void** data)
{
    try
    {
        const auto native = asset_io::FromUtf8(path);
        const auto& context = *static_cast<GltfFileContext*>(files->user_data);
        std::vector<std::uint8_t> bytes;
        if (!asset_io::Within(context.root, native) || !asset_io::Read(native, bytes)) return cgltf_result_io_error;
        if (*size != 0 && *size > bytes.size()) return cgltf_result_data_too_short;
        const auto length = *size == 0 ? bytes.size() : *size;
        *data = std::malloc((std::max)(length, std::size_t{ 1 }));
        if (!*data) return cgltf_result_out_of_memory;
        std::memcpy(*data, bytes.data(), length);
        *size = length;
        return cgltf_result_success;
    }
    catch (...) { return cgltf_result_io_error; }
}
void ReleaseGltfFile(const cgltf_memory_options*, const cgltf_file_options*, void* data) { std::free(data); }

bool ImportGltf(const std::filesystem::path& path, const std::vector<std::uint8_t>& bytes,
    const ImportOptions& options, StaticModel& output, std::vector<AssetDiagnostic>& diagnostics)
{
    GltfFileContext context{ options.allowedRoot };
    cgltf_options settings{};
    settings.file = { ReadGltfFile, ReleaseGltfFile, &context };
    cgltf_data* raw = nullptr;
    if (cgltf_parse(&settings, bytes.data(), bytes.size(), &raw) != cgltf_result_success)
    { Diagnostic(diagnostics, "Assets.InvalidModel", asset_io::Utf8(path)); return false; }
    std::unique_ptr<cgltf_data, decltype(&cgltf_free)> data(raw, cgltf_free);
    if (data->animations_count || data->skins_count)
    { Diagnostic(diagnostics, "Assets.UnsupportedAnimation"); return false; }
    if (data->nodes_count > MaxNodes || data->extensions_required_count != 0)
    { Diagnostic(diagnostics, "Assets.UnsupportedFeature", "required extension or node limit"); return false; }
    for (cgltf_size index = 0; index < data->buffer_views_count; ++index)
        if (data->buffer_views[index].has_meshopt_compression)
        { Diagnostic(diagnostics, "Assets.UnsupportedFeature", "meshopt compression"); return false; }
    for (cgltf_size index = 0; index < data->textures_count; ++index)
        if (data->textures[index].has_basisu || data->textures[index].has_webp)
        { Diagnostic(diagnostics, "Assets.UnsupportedFeature", "compressed/extended image"); return false; }
    for (cgltf_size index = 0; index < data->buffers_count; ++index)
    {
        const auto& buffer = data->buffers[index];
        if (buffer.size > asset_io::MaxFileBytes) return false;
        if (buffer.uri && std::strncmp(buffer.uri, "data:", 5) != 0)
        {
            std::filesystem::path resolved; std::string relative;
            if (!asset_io::ResolveReference(path, options.allowedRoot, buffer.uri, resolved, relative))
            { Diagnostic(diagnostics, "Assets.ReferenceOutsideRoot", buffer.uri); return false; }
            output.dependencies.push_back(relative);
        }
    }
    const auto filename = asset_io::Utf8(path);
    if (cgltf_load_buffers(&settings, data.get(), filename.c_str()) != cgltf_result_success
        || cgltf_validate(data.get()) != cgltf_result_success)
    { Diagnostic(diagnostics, "Assets.InvalidModel", "buffer or accessor validation"); return false; }

    std::unordered_map<const cgltf_image*, std::int32_t> imageIndices;
    for (cgltf_size index = 0; index < data->images_count; ++index)
    {
        const auto& image = data->images[index];
        TextureReference texture;
        if (image.mime_type) texture.mimeType = image.mime_type;
        if (image.buffer_view)
        {
            const auto* view = image.buffer_view;
            if (view->size > asset_io::MaxFileBytes || !view->buffer || !view->buffer->data) return false;
            const auto* begin = static_cast<const std::uint8_t*>(view->buffer->data) + view->offset;
            texture.embeddedBytes.assign(begin, begin + view->size);
        }
        else if (image.uri)
        {
            if (std::strncmp(image.uri, "data:", 5) == 0)
            { Diagnostic(diagnostics, "Assets.UnsupportedFeature", "image data URI; use bufferView or external image"); return false; }
            if (!AddExternalTexture(output, texture, image.uri, path, options, diagnostics)) return false;
        }
        else { Diagnostic(diagnostics, "Assets.InvalidModel", "image source"); return false; }
        imageIndices[&image] = static_cast<std::int32_t>(output.textures.size());
        output.textures.push_back(std::move(texture));
    }
    const auto textureIndex = [&](const cgltf_texture_view& view) -> std::int32_t {
        if (!view.texture || !view.texture->image) return -1;
        return imageIndices.at(view.texture->image);
    };
    for (cgltf_size index = 0; index < data->materials_count; ++index)
    {
        const auto& source = data->materials[index];
        MaterialData material;
        material.name = source.name ? source.name : "Material";
        if (source.alpha_mode != cgltf_alpha_mode_opaque || source.has_pbr_specular_glossiness
            || source.has_clearcoat || source.has_transmission || source.has_volume || source.has_sheen
            || source.has_iridescence || source.has_anisotropy || source.has_dispersion)
            Diagnostic(diagnostics, "Assets.MaterialApproximation", material.name, true);
        const auto& pbr = source.pbr_metallic_roughness;
        std::copy_n(pbr.base_color_factor, 4, material.baseColor.begin());
        material.metallic = pbr.metallic_factor;
        material.roughness = pbr.roughness_factor;
        for (const auto* view : { &pbr.base_color_texture, &pbr.metallic_roughness_texture, &source.normal_texture })
            if (view->texture && (view->texcoord != 0 || view->has_transform))
            { Diagnostic(diagnostics, "Assets.UnsupportedFeature", "texture coordinate set/transform"); return false; }
        material.baseColorTexture = textureIndex(pbr.base_color_texture);
        material.normalTexture = textureIndex(source.normal_texture);
        material.normalScale = source.normal_texture.scale;
        material.metallicRoughnessTexture = textureIndex(pbr.metallic_roughness_texture);
        material.doubleSided = source.double_sided != 0;
        output.materials.push_back(std::move(material));
    }

    std::vector<std::vector<std::uint32_t>> meshIndices(data->meshes_count);
    for (cgltf_size meshIndex = 0; meshIndex < data->meshes_count; ++meshIndex)
    {
        const auto& sourceMesh = data->meshes[meshIndex];
        for (cgltf_size primitiveIndex = 0; primitiveIndex < sourceMesh.primitives_count; ++primitiveIndex)
        {
            const auto& primitive = sourceMesh.primitives[primitiveIndex];
            if (primitive.type != cgltf_primitive_type_triangles || primitive.targets_count || primitive.has_draco_mesh_compression)
            { Diagnostic(diagnostics, "Assets.UnsupportedFeature", "non-triangle, morph or compressed primitive"); return false; }
            const cgltf_accessor *position = nullptr, *normal = nullptr, *uv = nullptr, *tangent = nullptr;
            for (cgltf_size attribute = 0; attribute < primitive.attributes_count; ++attribute)
            {
                const auto& value = primitive.attributes[attribute];
                if (value.type == cgltf_attribute_type_position) position = value.data;
                if (value.type == cgltf_attribute_type_normal) normal = value.data;
                if (value.type == cgltf_attribute_type_texcoord && value.index == 0) uv = value.data;
                if (value.type == cgltf_attribute_type_tangent) tangent = value.data;
                if (value.data->is_sparse)
                { Diagnostic(diagnostics, "Assets.UnsupportedFeature", "sparse accessor"); return false; }
            }
            if (!position || position->type != cgltf_type_vec3 || position->count == 0 || position->count > MaxVertices)
            { Diagnostic(diagnostics, "Assets.InvalidModel", "positions"); return false; }
            MeshData mesh;
            mesh.name = sourceMesh.name ? sourceMesh.name : "Mesh";
            mesh.material = primitive.material ? static_cast<std::int32_t>(primitive.material - data->materials) : -1;
            mesh.vertices.resize(position->count);
            for (cgltf_size vertex = 0; vertex < position->count; ++vertex)
            {
                auto& target = mesh.vertices[vertex];
                if (!cgltf_accessor_read_float(position, vertex, target.position.data(), 3)) return false;
                if (normal && (normal->count != position->count || !cgltf_accessor_read_float(normal, vertex, target.normal.data(), 3))) return false;
                if (uv && (uv->count != position->count || !cgltf_accessor_read_float(uv, vertex, target.uv.data(), 2))) return false;
                if (tangent && (tangent->count != position->count || !cgltf_accessor_read_float(tangent, vertex, target.tangent.data(), 4))) return false;
                for (auto& component : target.position) component *= options.scale;
            }
            const auto count = primitive.indices ? primitive.indices->count : position->count;
            if (count == 0 || count > MaxVertices * 6 || count % 3 != 0 || (primitive.indices && primitive.indices->is_sparse)) return false;
            mesh.indices.reserve(count);
            for (cgltf_size element = 0; element < count; ++element)
            {
                const auto value = primitive.indices ? cgltf_accessor_read_index(primitive.indices, element) : element;
                if (value >= mesh.vertices.size()) return false;
                mesh.indices.push_back(static_cast<std::uint32_t>(value));
            }
            GenerateBasis(mesh, !normal, options.generateTangents && !tangent);
            meshIndices[meshIndex].push_back(static_cast<std::uint32_t>(output.meshes.size()));
            output.meshes.push_back(std::move(mesh));
        }
    }
    for (cgltf_size index = 0; index < data->nodes_count; ++index)
    {
        const auto& source = data->nodes[index];
        if (source.has_mesh_gpu_instancing)
        { Diagnostic(diagnostics, "Assets.UnsupportedFeature", "mesh GPU instancing"); return false; }
        ModelNode node;
        node.name = source.name ? source.name : "Node";
        node.parent = source.parent ? static_cast<std::int32_t>(source.parent - data->nodes) : -1;
        cgltf_node_transform_local(&source, node.localMatrix.data());
        for (const auto offset : { 12, 13, 14 }) node.localMatrix[offset] *= options.scale;
        if (source.mesh) node.meshes = meshIndices[static_cast<std::size_t>(source.mesh - data->meshes)];
        output.nodes.push_back(std::move(node));
    }
    return true;
}

std::string String(ufbx_string value) { return { value.data, value.length }; }
bool ImportFbx(const std::filesystem::path& path, const std::vector<std::uint8_t>& bytes,
    const ImportOptions& options, StaticModel& output, std::vector<AssetDiagnostic>& diagnostics)
{
    ufbx_load_opts settings{};
    settings.target_axes = ufbx_axes_right_handed_y_up;
    settings.target_unit_meters = 1.0;
    settings.space_conversion = UFBX_SPACE_CONVERSION_MODIFY_GEOMETRY;
    settings.geometry_transform_handling = UFBX_GEOMETRY_TRANSFORM_HANDLING_HELPER_NODES;
    settings.inherit_mode_handling = UFBX_INHERIT_MODE_HANDLING_HELPER_NODES;
    settings.generate_missing_normals = true;
    const auto filename = asset_io::Utf8(path);
    settings.filename = { filename.data(), filename.size() };
    ufbx_error error{};
    std::unique_ptr<ufbx_scene, decltype(&ufbx_free_scene)> scene(
        ufbx_load_memory(bytes.data(), bytes.size(), &settings, &error), ufbx_free_scene);
    if (!scene)
    { Diagnostic(diagnostics, "Assets.InvalidModel", String(error.description)); return false; }
    if (scene->anim_stacks.count || scene->bones.count || scene->skin_deformers.count || scene->blend_deformers.count || scene->cache_deformers.count)
    { Diagnostic(diagnostics, "Assets.UnsupportedAnimation"); return false; }
    if (scene->nodes.count > MaxNodes) return false;

    std::unordered_map<const ufbx_texture*, std::int32_t> textureIndices;
    bool textureOk = true;
    const auto textureIndex = [&](const ufbx_material_map& map) -> std::int32_t {
        const auto* texture = map.texture;
        if (!texture || !map.texture_enabled) return -1;
        if (texture->type != UFBX_TEXTURE_FILE)
        { Diagnostic(diagnostics, "Assets.MaterialApproximation", "layered/procedural texture", true); return -1; }
        if (const auto found = textureIndices.find(texture); found != textureIndices.end()) return found->second;
        TextureReference target;
        if (texture->content.size)
        {
            const auto* begin = static_cast<const std::uint8_t*>(texture->content.data);
            target.embeddedBytes.assign(begin, begin + texture->content.size);
        }
        else
        {
            const auto relative = String(texture->relative_filename);
            textureOk = AddExternalTexture(output, target, relative.empty() ? String(texture->filename) : relative,
                path, options, diagnostics, false) && textureOk;
        }
        const auto result = static_cast<std::int32_t>(output.textures.size());
        output.textures.push_back(std::move(target));
        textureIndices[texture] = result;
        return result;
    };
    for (const auto* source : scene->materials)
    {
        MaterialData material;
        material.name = String(source->name);
        const auto& color = source->pbr.base_color.has_value ? source->pbr.base_color : source->fbx.diffuse_color;
        if (color.has_value)
            material.baseColor = { static_cast<float>(color.value_vec3.x), static_cast<float>(color.value_vec3.y), static_cast<float>(color.value_vec3.z), 1.0f };
        if (source->pbr.metalness.has_value) material.metallic = static_cast<float>(source->pbr.metalness.value_real);
        if (source->pbr.roughness.has_value) material.roughness = static_cast<float>(source->pbr.roughness.value_real);
        material.baseColorTexture = textureIndex(source->pbr.base_color.texture ? source->pbr.base_color : source->fbx.diffuse_color);
        material.normalTexture = textureIndex(source->pbr.normal_map.texture ? source->pbr.normal_map : source->fbx.normal_map);
        output.materials.push_back(std::move(material));
    }
    if (!textureOk) return false;
    for (const auto* sourceNode : scene->nodes)
    {
        ModelNode node;
        node.name = String(sourceNode->name);
        node.parent = sourceNode->parent ? static_cast<std::int32_t>(sourceNode->parent->typed_id) : -1;
        for (std::size_t column = 0; column < 4; ++column)
            for (std::size_t row = 0; row < 3; ++row)
                node.localMatrix[column * 4 + row] = static_cast<float>(sourceNode->node_to_parent.v[column * 3 + row]);
        for (const auto offset : { 12, 13, 14 }) node.localMatrix[offset] *= options.scale;
        if (const auto* source = sourceNode->mesh)
        {
            if (source->num_indices > MaxVertices || source->max_face_triangles > MaxVertices / 3) return false;
            std::vector<std::uint32_t> triangleIndices(source->max_face_triangles * 3);
            for (const auto& part : source->material_parts)
            {
                if (part.num_triangles == 0) continue;
                MeshData mesh;
                mesh.name = String(source->name);
                if (part.index < sourceNode->materials.count)
                    mesh.material = static_cast<std::int32_t>(sourceNode->materials.data[part.index]->typed_id);
                for (const auto faceIndex : part.face_indices)
                {
                    const auto face = source->faces.data[faceIndex];
                    const auto triangleCount = ufbx_triangulate_face(triangleIndices.data(), triangleIndices.size(), source, face);
                    for (std::size_t corner = 0; corner < static_cast<std::size_t>(triangleCount) * 3; ++corner)
                    {
                        const auto index = triangleIndices[corner];
                        MeshVertex vertex;
                        const auto position = ufbx_get_vertex_vec3(&source->vertex_position, index);
                        const auto normal = ufbx_get_vertex_vec3(&source->vertex_normal, index);
                        vertex.position = { static_cast<float>(position.x) * options.scale, static_cast<float>(position.y) * options.scale, static_cast<float>(position.z) * options.scale };
                        vertex.normal = { static_cast<float>(normal.x), static_cast<float>(normal.y), static_cast<float>(normal.z) };
                        if (source->vertex_uv.exists)
                        {
                            const auto uv = ufbx_get_vertex_vec2(&source->vertex_uv, index);
                            vertex.uv = { static_cast<float>(uv.x), 1.0f - static_cast<float>(uv.y) };
                        }
                        mesh.indices.push_back(static_cast<std::uint32_t>(mesh.vertices.size()));
                        mesh.vertices.push_back(vertex);
                    }
                }
                GenerateBasis(mesh, false, options.generateTangents);
                node.meshes.push_back(static_cast<std::uint32_t>(output.meshes.size()));
                output.meshes.push_back(std::move(mesh));
            }
        }
        output.nodes.push_back(std::move(node));
    }
    return true;
}
}

bool ValidateStaticModel(const StaticModel& model) { return ValidateModel(model); }

bool ImportStaticModel(const std::filesystem::path& source, const ImportOptions& requested,
    StaticModel& output, std::vector<AssetDiagnostic>& diagnostics)
{
    try
    {
        if (!std::isfinite(requested.scale) || requested.scale <= 0)
        { Diagnostic(diagnostics, "Assets.InvalidOptions"); return false; }
        const auto path = std::filesystem::weakly_canonical(source);
        ImportOptions options = requested;
        if (options.allowedRoot.empty()) options.allowedRoot = path.parent_path();
        if (!asset_io::Within(options.allowedRoot, path))
        { Diagnostic(diagnostics, "Assets.ReferenceOutsideRoot", asset_io::Utf8(source)); return false; }
        std::vector<std::uint8_t> bytes;
        if (!asset_io::Read(path, bytes))
        { Diagnostic(diagnostics, "Assets.ReadFailed", asset_io::Utf8(path)); return false; }
        std::string extension = path.extension().string();
        std::transform(extension.begin(), extension.end(), extension.begin(), [](unsigned char value) { return static_cast<char>(std::tolower(value)); });
        StaticModel model;
        const auto previousDiagnostics = diagnostics.size();
        bool imported = false;
        if (extension == ".fbx") imported = ImportFbx(path, bytes, options, model, diagnostics);
        else if (extension == ".gltf" || extension == ".glb") imported = ImportGltf(path, bytes, options, model, diagnostics);
        else { Diagnostic(diagnostics, "Assets.UnsupportedFormat", extension); return false; }
        if (!imported || !ValidateModel(model))
        {
            if (diagnostics.size() == previousDiagnostics) Diagnostic(diagnostics, "Assets.InvalidModel", asset_io::Utf8(path));
            return false;
        }
        std::sort(model.dependencies.begin(), model.dependencies.end());
        model.dependencies.erase(std::unique(model.dependencies.begin(), model.dependencies.end()), model.dependencies.end());
        for (auto& material : model.materials)
            for (const auto index : { material.baseColorTexture, material.normalTexture, material.metallicRoughnessTexture })
                if (index >= 0 && model.textures[static_cast<std::size_t>(index)].missing) material.needsFallbackTexture = true;
        output = std::move(model);
        return true;
    }
    catch (const std::exception& error)
    { Diagnostic(diagnostics, "Assets.InvalidModel", error.what()); return false; }
}
}
