#include "StaticModelPreparation.h"
#include "AssetIO.h"
#include <algorithm>
#include <limits>

namespace se
{
namespace
{
bool Affine(const Mat4& matrix)
{
    return IsFinite(matrix)&&std::abs(matrix(3,0))<=1e-5f&&std::abs(matrix(3,1))<=1e-5f
        &&std::abs(matrix(3,2))<=1e-5f&&std::abs(matrix(3,3)-1)<=1e-5f&&Inverse(matrix).has_value();
}
bool Invalid(std::vector<AssetDiagnostic>& diagnostics)
{asset_io::Diagnostic(diagnostics,"Rendering.InvalidModel");return false;}
}
bool BuildStaticModelLayout(const StaticModel& model,StaticModelLayout& output,std::vector<AssetDiagnostic>& diagnostics)
{
    if(!ValidateStaticModel(model))return Invalid(diagnostics);
    StaticModelLayout candidate;
    const float infinity=std::numeric_limits<float>::infinity();
    candidate.minimum={infinity,infinity,infinity};candidate.maximum={-infinity,-infinity,-infinity};
    std::vector<Mat4> transforms(model.nodes.size());
    std::vector<bool> done(model.nodes.size());
    std::vector<std::size_t> path;
    for(std::size_t index=0;index<model.nodes.size();++index)
    {
        path.clear();auto cursor=static_cast<std::int32_t>(index);
        while(cursor>=0&&!done[static_cast<std::size_t>(cursor)])
        {path.push_back(static_cast<std::size_t>(cursor));cursor=model.nodes[static_cast<std::size_t>(cursor)].parent;}
        for(auto it=path.rbegin();it!=path.rend();++it)
        {
            const auto& node=model.nodes[*it];const Mat4 local{node.localMatrix};
            if(!Affine(local))return Invalid(diagnostics);
            transforms[*it]=node.parent<0?local:transforms[static_cast<std::size_t>(node.parent)]*local;
            if(!Affine(transforms[*it]))return Invalid(diagnostics);
            done[*it]=true;
        }
        for(const auto meshIndex:model.nodes[index].meshes)
        {
            const auto& mesh=model.meshes[meshIndex];
            StaticModelPart part;part.mesh=meshIndex;part.modelFromMesh=transforms[index];
            if(mesh.material>=0)
            {
                const auto& color=model.materials[static_cast<std::size_t>(mesh.material)].baseColor;
                for(const auto value:color)if(value<0||value>1)return Invalid(diagnostics);
                part.baseColor={color[0],color[1],color[2],color[3]};
            }
            for(const auto& vertex:mesh.vertices)
            {
                const auto position=TransformPoint(part.modelFromMesh,{vertex.position[0],vertex.position[1],vertex.position[2]});
                if(!IsFinite(position))return Invalid(diagnostics);
                candidate.minimum={std::min(candidate.minimum.x,position.x),std::min(candidate.minimum.y,position.y),std::min(candidate.minimum.z,position.z)};
                candidate.maximum={std::max(candidate.maximum.x,position.x),std::max(candidate.maximum.y,position.y),std::max(candidate.maximum.z,position.z)};
            }
            candidate.parts.push_back(part);
        }
    }
    if(candidate.parts.empty())return Invalid(diagnostics);
    output=std::move(candidate);return true;
}

const ImageData& DefaultTextureImage(DefaultTextureKind kind)
{
    static const ImageData white{1,1,{255,255,255,255}};
    static const ImageData normal{1,1,{128,128,255,255}};
    static const ImageData fallback{2,2,{255,0,255,255,32,0,32,255,32,0,32,255,255,0,255,255}};
    return kind==DefaultTextureKind::White?white:kind==DefaultTextureKind::FlatNormal?normal:fallback;
}
std::string ImageContentKey(const ImageData& image)
{
    const auto hash=asset_io::Hash(image.rgba8);
    return hash.empty()?std::string{}:std::to_string(image.width)+"x"+std::to_string(image.height)+":"+hash;
}
const std::string& DefaultTextureKey(DefaultTextureKind kind)
{
    static const auto white=ImageContentKey(DefaultTextureImage(DefaultTextureKind::White));
    static const auto normal=ImageContentKey(DefaultTextureImage(DefaultTextureKind::FlatNormal));
    static const auto fallback=ImageContentKey(DefaultTextureImage(DefaultTextureKind::Fallback));
    return kind==DefaultTextureKind::White?white:kind==DefaultTextureKind::FlatNormal?normal:fallback;
}
bool SelectModelMaterialImages(const StaticModel& model,const PreparedModelImages& prepared,std::size_t index,ModelMaterialImages& output)
{
    if(index>=model.meshes.size()||prepared.images.size()!=model.textures.size()||prepared.unavailable.size()!=model.textures.size()||prepared.keys.size()!=model.textures.size())return false;
    const auto slot=model.meshes[index].material;
    if(slot<-1||(slot>=0&&static_cast<std::size_t>(slot)>=model.materials.size()))return false;
    ModelMaterialImages result;result.material=slot<0?nullptr:&model.materials[static_cast<std::size_t>(slot)];
    const auto* material=result.material;
    const std::array<std::int32_t,3> indices{material?material->baseColorTexture:-1,material?material->normalTexture:-1,material?material->metallicRoughnessTexture:-1};
    for(std::size_t channel=0;channel<3;++channel)
    {
        const auto selected=indices[channel];if(selected<-1||(selected>=0&&static_cast<std::size_t>(selected)>=model.textures.size()))return false;
        const bool bad=selected>=0&&prepared.unavailable[static_cast<std::size_t>(selected)];
        result.fallback[channel]=bad||(channel==0&&material&&material->needsFallbackTexture);
        if(channel==1)result.normalMapping=selected>=0&&!bad;
        if(selected<0||result.fallback[channel])
        {
            const auto kind=channel==1?DefaultTextureKind::FlatNormal:channel==0&&result.fallback[0]?DefaultTextureKind::Fallback:DefaultTextureKind::White;
            result.images[channel]=&DefaultTextureImage(kind);
            result.keys[channel]=DefaultTextureKey(kind);
        }
        else
        {
            result.images[channel]=&prepared.images[static_cast<std::size_t>(selected)];result.keys[channel]=prepared.keys[static_cast<std::size_t>(selected)];
            if(result.keys[channel].empty())return false;
        }
    }
    output=std::move(result);return true;
}
}
