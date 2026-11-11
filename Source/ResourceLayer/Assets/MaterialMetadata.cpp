#include "MaterialMetadata.h"
#include <array>

namespace se
{
namespace
{
const std::array fields{
    PropertyDescriptor{"material.name","EditorUi.Name",PropertyType::Text,std::string{},std::nullopt,std::nullopt},
    PropertyDescriptor{"material.baseColor","EditorUi.BaseColor",PropertyType::Vector4,Vec4{1,1,1,1},0.0f,1.0f,true,{},PropertyPresentation::Color},
    PropertyDescriptor{"material.metallic","EditorUi.Metallic",PropertyType::Number,0.0f,0.0f,1.0f,true,{},PropertyPresentation::Slider},
    PropertyDescriptor{"material.roughness","EditorUi.Roughness",PropertyType::Number,1.0f,0.0f,1.0f,true,{},PropertyPresentation::Slider},
    PropertyDescriptor{"material.doubleSided","EditorUi.DoubleSided",PropertyType::Boolean,false,std::nullopt,std::nullopt},
    PropertyDescriptor{"material.normalScale","Property.Material.NormalScale",PropertyType::Number,1.0f,std::nullopt,std::nullopt,true,{},PropertyPresentation::Default,true},
    PropertyDescriptor{"material.baseColorTexture","EditorUi.BaseColorTexture",PropertyType::AssetId,std::string{},std::nullopt,std::nullopt,true,"textures/baseColor"},
    PropertyDescriptor{"material.normalTexture","EditorUi.NormalTexture",PropertyType::AssetId,std::string{},std::nullopt,std::nullopt,true,"textures/normal"},
    PropertyDescriptor{"material.metallicRoughnessTexture","EditorUi.MetallicRoughnessTexture",PropertyType::AssetId,std::string{},std::nullopt,std::nullopt,true,"textures/metallicRoughness"},
    PropertyDescriptor{"material.shader","EditorUi.Shader",PropertyType::AssetId,std::string{},std::nullopt,std::nullopt,true,"shader",PropertyPresentation::Default,true}};
}
std::span<const PropertyDescriptor> MaterialProperties(){return fields;}
const PropertyDescriptor* FindMaterialProperty(std::string_view key)
{for(const auto& field:fields)if(field.key==key)return &field;return nullptr;}
bool ReadMaterialProperty(const MaterialDocument& m,std::string_view key,PropertyValue& value,std::string& error)
{
    error.clear();if(!FindMaterialProperty(key)){error="Metadata.UnknownProperty";return false;}
    if(key=="material.name")value=m.name;
    else if(key=="material.baseColor")value=Vec4{m.baseColor[0],m.baseColor[1],m.baseColor[2],m.baseColor[3]};
    else if(key=="material.metallic")value=m.metallic;
    else if(key=="material.roughness")value=m.roughness;
    else if(key=="material.doubleSided")value=m.doubleSided;
    else if(key=="material.normalScale")value=m.normalScale;
    else if(key=="material.baseColorTexture")value=m.baseColorTexture;
    else if(key=="material.normalTexture")value=m.normalTexture;
    else if(key=="material.metallicRoughnessTexture")value=m.metallicRoughnessTexture;
    else value=m.shader;
    return true;
}
bool WriteMaterialProperty(MaterialDocument& material,std::string_view key,const PropertyValue& value,std::string& error)
{
    error.clear();const auto* field=FindMaterialProperty(key);
    if(!field){error="Metadata.UnknownProperty";return false;}
    if(!field->editable||!ValidatePropertyValue(*field,value)){error="Metadata.InvalidValue";return false;}
    auto copy=material;
    if(key=="material.name")copy.name=std::get<std::string>(value);
    else if(key=="material.baseColor"){const auto v=std::get<Vec4>(value);copy.baseColor={v.x,v.y,v.z,v.w};}
    else if(key=="material.metallic")copy.metallic=std::get<float>(value);
    else if(key=="material.roughness")copy.roughness=std::get<float>(value);
    else if(key=="material.doubleSided")copy.doubleSided=std::get<bool>(value);
    else if(key=="material.normalScale")copy.normalScale=std::get<float>(value);
    else if(key=="material.baseColorTexture")copy.baseColorTexture=std::get<std::string>(value);
    else if(key=="material.normalTexture")copy.normalTexture=std::get<std::string>(value);
    else if(key=="material.metallicRoughnessTexture")copy.metallicRoughnessTexture=std::get<std::string>(value);
    else copy.shader=std::get<std::string>(value);
    if(!ValidateMaterialDocument(copy)){error="Assets.InvalidMaterial";return false;}
    material=std::move(copy);return true;
}
}
