#include "MaterialDocument.h"
#include "MaterialMetadata.h"
#include "ResourceLayer/PropertyJson.h"
#include "AssetIO.h"
#include "CoreLayer/Utf.h"
#include <nlohmann/json.hpp>
#include <algorithm>
#include <cctype>
#include <cmath>

namespace se
{
namespace
{
bool AssetReference(const AssetId& id)
{
    return id.empty()||(id.size()==32&&std::all_of(id.begin(),id.end(),[](char c){return (c>='0'&&c<='9')||(c>='a'&&c<='f');}));
}
}
bool IsMaterialDocumentPath(const std::filesystem::path& path)
{
    auto name=asset_io::Utf8(path.filename());
    std::transform(name.begin(),name.end(),name.begin(),[](unsigned char c){return static_cast<char>(std::tolower(c));});
    return name.size()>std::string_view(".material.json").size()&&name.ends_with(".material.json");
}
bool ValidateMaterialDocument(const MaterialDocument& material)
{
    std::u32string decoded;
    if(material.name.size()>1024||!DecodeUtf8(material.name,decoded))return false;
    for(float value:material.baseColor)if(!std::isfinite(value)||value<0||value>1)return false;
    if(!std::isfinite(material.metallic)||material.metallic<0||material.metallic>1
        ||!std::isfinite(material.roughness)||material.roughness<0||material.roughness>1||!std::isfinite(material.normalScale))return false;
    return AssetReference(material.baseColorTexture)&&AssetReference(material.normalTexture)
        &&AssetReference(material.metallicRoughnessTexture)&&AssetReference(material.shader);
}
std::vector<AssetId> MaterialTextureDependencies(const MaterialDocument& material)
{
    std::vector<AssetId> ids;
    for(const auto& id:{material.baseColorTexture,material.normalTexture,material.metallicRoughnessTexture})if(!id.empty())ids.push_back(id);
    std::sort(ids.begin(),ids.end());ids.erase(std::unique(ids.begin(),ids.end()),ids.end());return ids;
}
std::vector<AssetId> MaterialDependencies(const MaterialDocument& material)
{
    auto ids=MaterialTextureDependencies(material);
    if(!material.shader.empty())ids.push_back(material.shader);
    std::sort(ids.begin(),ids.end());ids.erase(std::unique(ids.begin(),ids.end()),ids.end());return ids;
}
bool SerializeMaterialDocument(const MaterialDocument& material,std::string& output,std::vector<AssetDiagnostic>& diagnostics)
{
    if(!ValidateMaterialDocument(material)){asset_io::Diagnostic(diagnostics,"Assets.InvalidMaterial");return false;}
    try
    {
        nlohmann::json json={{"type","SelfEngine.Material"},{"version",1}};
        for(const auto& field:MaterialProperties())
        {
            PropertyValue value;std::string error;if(!ReadMaterialProperty(material,field.key,value,error))return false;
            json[nlohmann::json::json_pointer("/"+std::string(SerializedPropertyKey(field)))]=WritePropertyJson(value);
        }
        output=json.dump(2);return true;
    }
    catch(const std::exception& error){asset_io::Diagnostic(diagnostics,"Assets.InvalidMaterial",error.what());return false;}
}
bool DeserializeMaterialDocument(std::string_view text,MaterialDocument& output,std::vector<AssetDiagnostic>& diagnostics)
{
    try
    {
        if(text.size()>65536){asset_io::Diagnostic(diagnostics,"Assets.InvalidMaterial","document size");return false;}
        const auto json=nlohmann::json::parse(text);
        if(json.at("type")!="SelfEngine.Material"||json.at("version")!=1)
        {asset_io::Diagnostic(diagnostics,"Assets.MaterialVersionUnsupported");return false;}
        MaterialDocument loaded;
        for(const auto& field:MaterialProperties())
        {
            const nlohmann::json::json_pointer path("/"+std::string(SerializedPropertyKey(field)));
            if(field.serializedOptional&&!json.contains(path))continue;
            PropertyValue value;std::string error;
            if(!json.contains(path)||!ReadPropertyJson(json.at(path),field,value)||!WriteMaterialProperty(loaded,field.key,value,error))
            {asset_io::Diagnostic(diagnostics,"Assets.InvalidMaterial",std::string(field.key));return false;}
        }
        if(!ValidateMaterialDocument(loaded)){asset_io::Diagnostic(diagnostics,"Assets.InvalidMaterial");return false;}
        output=std::move(loaded);return true;
    }
    catch(const std::exception& error){asset_io::Diagnostic(diagnostics,"Assets.InvalidMaterial",error.what());return false;}
}
bool SaveMaterialDocument(const std::filesystem::path& path,const MaterialDocument& material,std::vector<AssetDiagnostic>& diagnostics)
{
    if(!IsMaterialDocumentPath(path)){asset_io::Diagnostic(diagnostics,"Assets.MaterialPathInvalid",asset_io::Utf8(path));return false;}
    std::string json;if(!SerializeMaterialDocument(material,json,diagnostics))return false;
    if(!asset_io::WriteAtomic(path,{reinterpret_cast<const std::uint8_t*>(json.data()),json.size()}))
    {asset_io::Diagnostic(diagnostics,"Assets.WriteFailed",asset_io::Utf8(path));return false;}
    return true;
}
bool LoadMaterialDocument(const std::filesystem::path& path,MaterialDocument& output,std::vector<AssetDiagnostic>& diagnostics)
{
    if(!IsMaterialDocumentPath(path)){asset_io::Diagnostic(diagnostics,"Assets.MaterialPathInvalid",asset_io::Utf8(path));return false;}
    std::vector<std::uint8_t> bytes;
    if(!asset_io::Read(path,bytes)){asset_io::Diagnostic(diagnostics,"Assets.ReadFailed",asset_io::Utf8(path));return false;}
    return DeserializeMaterialDocument(std::string_view(reinterpret_cast<const char*>(bytes.data()),bytes.size()),output,diagnostics);
}
}
