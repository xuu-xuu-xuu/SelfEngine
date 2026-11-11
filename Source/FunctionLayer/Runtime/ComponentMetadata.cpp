#include "FunctionLayer/Runtime/ComponentMetadata.h"
#include <array>
#include <algorithm>
#include <unordered_set>

namespace se
{
namespace
{
const std::array transformProperties{
    PropertyDescriptor{"transform.position","Property.Transform.Position",PropertyType::Vector3,Vec3{},std::nullopt,std::nullopt},
    PropertyDescriptor{"transform.rotation","Property.Transform.Rotation",PropertyType::Quaternion,Quat{},std::nullopt,std::nullopt},
    PropertyDescriptor{"transform.scale","Property.Transform.Scale",PropertyType::Vector3,Vec3{1,1,1},std::nullopt,std::nullopt}};
const std::array cameraProperties{
    PropertyDescriptor{"camera.fov","Property.Camera.Fov",PropertyType::Number,Pi/3,0.001f,Pi-0.001f},
    PropertyDescriptor{"camera.near","Property.Camera.Near",PropertyType::Number,0.1f,0.0001f,std::nullopt},
    PropertyDescriptor{"camera.far","Property.Camera.Far",PropertyType::Number,1000.0f,0.0001f,std::nullopt},
    PropertyDescriptor{"camera.primary","Property.Camera.Primary",PropertyType::Boolean,false,std::nullopt,std::nullopt}};
const std::array meshProperties{
    PropertyDescriptor{"mesh.asset","Property.Mesh.Asset",PropertyType::AssetId,std::string{},std::nullopt,std::nullopt},
    PropertyDescriptor{"mesh.materials","Property.Mesh.Materials",PropertyType::AssetList,std::vector<std::string>{},std::nullopt,std::nullopt},
    PropertyDescriptor{"mesh.castsShadow","Property.Mesh.CastsShadow",PropertyType::Boolean,true,std::nullopt,std::nullopt},
    PropertyDescriptor{"mesh.receivesShadow","Property.Mesh.ReceivesShadow",PropertyType::Boolean,true,std::nullopt,std::nullopt}};
const std::array lightProperties{
    PropertyDescriptor{"light.color","Property.Light.Color",PropertyType::Vector3,Vec3{1,1,1},0.0f,std::nullopt,true,{},PropertyPresentation::Color},
    PropertyDescriptor{"light.intensity","Property.Light.Intensity",PropertyType::Number,1.0f,0.0f,std::nullopt},
    PropertyDescriptor{"light.point","Property.Light.Point",PropertyType::Boolean,false,std::nullopt,std::nullopt,true,"type"},
    PropertyDescriptor{"light.range","Property.Light.Range",PropertyType::Number,10.0f,0.001f,std::nullopt}};
const std::array components{
    ComponentDescriptor{"transform","Component.Transform",transformProperties,false},
    ComponentDescriptor{"camera","Component.Camera",cameraProperties},
    ComponentDescriptor{"mesh","Component.Mesh",meshProperties},
    ComponentDescriptor{"light","Component.Light",lightProperties}};
bool Fail(std::string& error,const char* key){error=key;return false;}
bool Safe(const World& world,EntityHandle entity,std::string& error)
{
    if(!world.IsAlive(entity))return Fail(error,"Metadata.InvalidEntity");
    if(world.IsIterating()||(world.HasPendingCommands()&&!world.IsSceneCommitBoundary()))return Fail(error,"Metadata.PendingCommands");
    return true;
}
}

const PropertyDescriptor* FindBuiltinProperty(std::string_view key)
{for(const auto& component:components)for(const auto& property:component.properties)if(property.key==key)return &property;return nullptr;}
bool HasBuiltinComponent(const World& world,EntityHandle entity,std::string_view component)
{
    if(component=="transform")return world.GetTransform(entity)!=nullptr;
    if(component=="camera")return world.GetCamera(entity)!=nullptr;
    if(component=="mesh")return world.GetMesh(entity)!=nullptr;
    if(component=="light")return world.GetLight(entity)!=nullptr;
    return false;
}
bool ReadBuiltinProperty(const World& world,EntityHandle entity,std::string_view key,PropertyValue& value,std::string& error)
{
    error.clear();if(!world.IsAlive(entity))return Fail(error,"Metadata.InvalidEntity");
    if(!FindBuiltinProperty(key))return Fail(error,"Metadata.UnknownProperty");
    if(!HasBuiltinComponent(world,entity,key.substr(0,key.find('.'))))return Fail(error,"Metadata.MissingComponent");
    if(key=="transform.position")value=world.GetTransform(entity)->position;
    else if(key=="transform.rotation")value=world.GetTransform(entity)->rotation;
    else if(key=="transform.scale")value=world.GetTransform(entity)->scale;
    else if(key=="camera.fov")value=world.GetCamera(entity)->verticalFovRadians;
    else if(key=="camera.near")value=world.GetCamera(entity)->nearPlane;
    else if(key=="camera.far")value=world.GetCamera(entity)->farPlane;
    else if(key=="camera.primary")value=world.GetCamera(entity)->primary;
    else if(key=="mesh.asset")value=world.GetMesh(entity)->meshAsset;
    else if(key=="mesh.materials")value=world.GetMesh(entity)->materials;
    else if(key=="mesh.castsShadow")value=world.GetMesh(entity)->castsShadow;
    else if(key=="mesh.receivesShadow")value=world.GetMesh(entity)->receivesShadow;
    else if(key=="light.color")value=world.GetLight(entity)->color;
    else if(key=="light.intensity")value=world.GetLight(entity)->intensity;
    else if(key=="light.point")value=world.GetLight(entity)->type==LightType::Point;
    else if(key=="light.range")value=world.GetLight(entity)->range;
    return true;
}
bool WriteBuiltinProperty(World& world,EntityHandle entity,std::string_view key,const PropertyValue& value,std::string& error)
{
    error.clear();if(!Safe(world,entity,error))return false;
    const auto* field=FindBuiltinProperty(key);if(!field)return Fail(error,"Metadata.UnknownProperty");
    if(!field->editable||!ValidatePropertyValue(*field,value))return Fail(error,"Metadata.InvalidValue");
    const auto component=key.substr(0,key.find('.'));
    if(!HasBuiltinComponent(world,entity,component))return Fail(error,"Metadata.MissingComponent");
    bool result=false;
    if(component=="transform")
    {
        auto copy=*world.GetTransform(entity);
        if(key=="transform.position")copy.position=std::get<Vec3>(value);
        else if(key=="transform.rotation")copy.rotation=std::get<Quat>(value);
        else copy.scale=std::get<Vec3>(value);
        result=world.SetTransform(entity,copy);
    }
    else if(component=="camera")
    {
        auto copy=*world.GetCamera(entity);
        if(key=="camera.fov")copy.verticalFovRadians=std::get<float>(value);
        else if(key=="camera.near")copy.nearPlane=std::get<float>(value);
        else if(key=="camera.far")copy.farPlane=std::get<float>(value);
        else copy.primary=std::get<bool>(value);
        result=world.SetCamera(entity,copy);
    }
    else if(component=="mesh")
    {
        auto copy=*world.GetMesh(entity);
        if(key=="mesh.asset")copy.meshAsset=std::get<std::string>(value);
        else if(key=="mesh.materials")copy.materials=std::get<std::vector<std::string>>(value);
        else if(key=="mesh.castsShadow")copy.castsShadow=std::get<bool>(value);
        else copy.receivesShadow=std::get<bool>(value);
        result=world.SetMesh(entity,copy);
    }
    else
    {
        auto copy=*world.GetLight(entity);
        if(key=="light.color")copy.color=std::get<Vec3>(value);
        else if(key=="light.point")copy.type=std::get<bool>(value)?LightType::Point:LightType::Directional;
        else if(key=="light.range")copy.range=std::get<float>(value);
        else copy.intensity=std::get<float>(value);
        result=world.SetLight(entity,copy);
    }
    if(!result)error=world.LastError();return result;
}

namespace
{
bool ReadBuiltin(const World& world,ComponentHandle handle,std::size_t index,PropertyValue& value,std::string& error)
{
    const auto* component=world.GetComponent(handle);if(!component)return Fail(error,"World.InvalidComponent");
    const auto* descriptor=FindComponentDescriptor(component->Type());
    return descriptor&&index<descriptor->properties.size()&&ReadBuiltinProperty(world,component->Owner(),descriptor->properties[index].key,value,error);
}
bool WriteBuiltin(World& world,ComponentHandle handle,std::size_t index,const PropertyValue& value,std::string& error)
{
    const auto* component=world.GetComponent(handle);if(!component)return Fail(error,"World.InvalidComponent");
    const auto* descriptor=FindComponentDescriptor(component->Type());
    return descriptor&&index<descriptor->properties.size()&&WriteBuiltinProperty(world,component->Owner(),descriptor->properties[index].key,value,error);
}
template<ComponentTypeId Type> ComponentHandle CreateBuiltin(World& world,EntityHandle owner,std::span<const PropertyValue> v,bool enabled,std::string& error)
{
    bool result=false;
    if constexpr(Type==ComponentTypes::Transform)
        result=world.SetTransform(owner,{std::get<Vec3>(v[0]),std::get<Quat>(v[1]),std::get<Vec3>(v[2])},TransformWriteMode::PreserveUnit);
    if constexpr(Type==ComponentTypes::Camera)
        result=world.SetCamera(owner,{std::get<float>(v[0]),std::get<float>(v[1]),std::get<float>(v[2]),std::get<bool>(v[3])});
    if constexpr(Type==ComponentTypes::Mesh)
        result=world.SetMesh(owner,{std::get<std::string>(v[0]),std::get<std::vector<std::string>>(v[1]),std::get<bool>(v[2]),std::get<bool>(v[3])});
    if constexpr(Type==ComponentTypes::Light)
        result=world.SetLight(owner,{std::get<Vec3>(v[0]),std::get<float>(v[1]),std::get<bool>(v[2])?LightType::Point:LightType::Directional,std::get<float>(v[3])});
    const auto handle=world.FindComponent(owner,Type);
    if(!result||!handle||!world.SetComponentEnabled(handle,enabled)){error=world.LastError();return {};}
    return handle;
}
std::vector<ComponentDescriptor>& Registry()
{
    static std::vector<ComponentDescriptor> values=[] {
        std::vector<ComponentDescriptor> result(components.begin(),components.end());
        const std::array ids{ComponentTypes::Transform,ComponentTypes::Camera,ComponentTypes::Mesh,ComponentTypes::Light};
        const std::array factories{CreateBuiltin<ComponentTypes::Transform>,CreateBuiltin<ComponentTypes::Camera>,CreateBuiltin<ComponentTypes::Mesh>,CreateBuiltin<ComponentTypes::Light>};
        for(std::size_t i=0;i<result.size();++i){result[i].type=ids[i];result[i].read=ReadBuiltin;result[i].write=WriteBuiltin;result[i].create=factories[i];}
        return result;
    }();return values;
}
bool ReferenceValid(const World& world,const PropertyValue& value)
{const auto* reference=std::get_if<PropertyEntityReference>(&value);return !reference||!reference->stableId||world.CanResolveReference(reference->stableId);}
}
std::span<const ComponentDescriptor> ComponentMetadata(){return Registry();}
const ComponentDescriptor* FindComponentDescriptor(ComponentTypeId type)
{for(const auto& descriptor:Registry())if(descriptor.type==type)return &descriptor;return nullptr;}
const ComponentDescriptor* FindComponentDescriptor(std::string_view key)
{for(const auto& descriptor:Registry())if(descriptor.key==key)return &descriptor;return nullptr;}
bool RegisterComponent(const ComponentDescriptor& descriptor,std::string& error)
{
    error.clear();
    if(FindComponentDescriptor(descriptor.type)||FindComponentDescriptor(descriptor.key))return Fail(error,"Metadata.DuplicateType");
    if(descriptor.type<1024||descriptor.key.empty()||descriptor.key.size()>128||descriptor.key.find('.')!=std::string_view::npos||!descriptor.version
        ||!descriptor.read||!descriptor.write||!descriptor.create||descriptor.properties.size()>256||Registry().size()>=1024)
        return Fail(error,"Metadata.InvalidDescriptor");
    std::unordered_set<std::string_view> keys;const auto prefix=std::string(descriptor.key)+'.';
    constexpr std::array<std::size_t,9> indices{0,1,2,3,4,5,4,6,7};
    for(const auto& property:descriptor.properties)
    {
        const auto type=static_cast<std::size_t>(property.type);
        if(!property.key.starts_with(prefix)||property.key.size()==prefix.size()||type>=indices.size()||property.defaultValue.index()!=indices[type]
            ||!keys.insert(SerializedPropertyKey(property)).second||FindProperty(property.key)||!ValidatePropertyValue(property,property.defaultValue))
            return Fail(error,"Metadata.InvalidDescriptor");
    }
    Registry().push_back(descriptor);return true;
}
const PropertyDescriptor* FindProperty(std::string_view key)
{for(const auto& component:Registry())for(const auto& property:component.properties)if(property.key==key)return &property;return nullptr;}
bool HasComponent(const World& world,EntityHandle entity,std::string_view key)
{const auto* descriptor=FindComponentDescriptor(key);return descriptor&&bool(world.FindComponent(entity,descriptor->type));}
bool ReadProperty(const World& world,EntityHandle entity,std::string_view key,PropertyValue& value,std::string& error)
{
    error.clear();if(!world.IsAlive(entity))return Fail(error,"Metadata.InvalidEntity");
    const auto* descriptor=FindComponentDescriptor(key.substr(0,key.find('.')));
    const auto* field=FindProperty(key);if(!descriptor||!field)return Fail(error,"Metadata.UnknownProperty");
    const auto handle=world.FindComponent(entity,descriptor->type);if(!handle)return Fail(error,"Metadata.MissingComponent");
    return descriptor->read(world,handle,static_cast<std::size_t>(field-descriptor->properties.data()),value,error);
}
bool WriteProperty(World& world,EntityHandle entity,std::string_view key,const PropertyValue& value,std::string& error)
{
    error.clear();if(!Safe(world,entity,error))return false;
    const auto* descriptor=FindComponentDescriptor(key.substr(0,key.find('.')));const auto* field=FindProperty(key);
    if(!descriptor||!field)return Fail(error,"Metadata.UnknownProperty");
    if(!field->editable||!ValidatePropertyValue(*field,value))return Fail(error,"Metadata.InvalidValue");
    if(!ReferenceValid(world,value))return Fail(error,"Metadata.MissingReference");
    const auto handle=world.FindComponent(entity,descriptor->type);if(!handle)return Fail(error,"Metadata.MissingComponent");
    return descriptor->write(world,handle,static_cast<std::size_t>(field-descriptor->properties.data()),value,error);
}
bool ReadComponentProperties(const World& world,ComponentHandle handle,std::vector<ComponentProperty>& properties,std::string& error)
{
    error.clear();const auto* component=world.GetComponent(handle);if(!component)return Fail(error,"World.InvalidComponent");
    const auto* descriptor=FindComponentDescriptor(component->Type());if(!descriptor)return Fail(error,"Metadata.UnknownComponent");
    std::vector<ComponentProperty> result;
    for(std::size_t i=0;i<descriptor->properties.size();++i)
    {
        const auto& field=descriptor->properties[i];if(!field.serialized)continue;
        PropertyValue value;if(!descriptor->read(world,handle,i,value,error))return false;
        if(!ValidatePropertyValue(field,value))return Fail(error,"Metadata.InvalidValue");
        if(!ReferenceValid(world,value))return Fail(error,"Metadata.MissingReference");
        result.push_back({std::string(SerializedPropertyKey(field)),std::move(value)});
    }
    properties=std::move(result);return true;
}
ComponentHandle CreateComponentFromProperties(World& world,EntityHandle owner,ComponentTypeId type,std::uint32_t version,
    std::vector<ComponentProperty> properties,bool enabled,std::string& error)
{
    error.clear();if(!Safe(world,owner,error))return {};
    const auto* descriptor=FindComponentDescriptor(type);if(!descriptor){error="Metadata.UnknownComponent";return {};}
    if(version!=descriptor->version&&(!descriptor->migrate||!descriptor->migrate(version,properties,error)))
    {if(error.empty())error="Metadata.UnsupportedVersion";return {};}
    if(descriptor->optional&&!descriptor->multiple&&world.FindComponent(owner,type)){error="Metadata.ComponentExists";return {};}
    std::vector<PropertyValue> values;values.reserve(descriptor->properties.size());
    std::unordered_set<std::string> used;
    for(const auto& input:properties)
    {
        const auto it=std::find_if(descriptor->properties.begin(),descriptor->properties.end(),[&](const auto& field){return field.serialized&&SerializedPropertyKey(field)==input.key;});
        if(it==descriptor->properties.end()||!used.insert(input.key).second){error="Metadata.UnknownProperty";return {};}
    }
    for(const auto& field:descriptor->properties)
    {
        const auto it=std::find_if(properties.begin(),properties.end(),[&](const auto& property){return property.key==SerializedPropertyKey(field);});
        if(field.serialized&&!field.serializedOptional&&it==properties.end()){error="Metadata.MissingProperty";return {};}
        const auto value=it==properties.end()?field.defaultValue:it->value;
        if(!ValidatePropertyValue(field,value)){error="Metadata.InvalidValue";return {};}
        if(!ReferenceValid(world,value)){error="Metadata.MissingReference";return {};}
        values.push_back(value);
    }
    return descriptor->create(world,owner,values,enabled,error);
}
bool AddComponent(World& world,EntityHandle entity,std::string_view key,std::string& error)
{
    error.clear();if(!Safe(world,entity,error))return false;
    const auto* descriptor=FindComponentDescriptor(key);if(!descriptor)return Fail(error,"Metadata.UnknownComponent");
    if(!descriptor->multiple&&world.FindComponent(entity,descriptor->type))return Fail(error,"Metadata.ComponentExists");
    std::vector<ComponentProperty> defaults;
    for(const auto& field:descriptor->properties)if(field.serialized)defaults.push_back({std::string(SerializedPropertyKey(field)),field.defaultValue});
    return bool(CreateComponentFromProperties(world,entity,descriptor->type,descriptor->version,std::move(defaults),true,error));
}
bool RemoveComponent(World& world,EntityHandle entity,std::string_view key,std::string& error)
{
    error.clear();if(!Safe(world,entity,error))return false;
    const auto* descriptor=FindComponentDescriptor(key);if(!descriptor)return Fail(error,"Metadata.UnknownComponent");
    if(!descriptor->optional)return Fail(error,"Metadata.RequiredComponent");
    const auto handle=world.FindComponent(entity,descriptor->type);if(!handle)return Fail(error,"Metadata.MissingComponent");
    if(world.RemoveComponent(handle))return true;error=world.LastError();return false;
}
bool CloneComponentProperties(const World& source,EntityHandle from,World& target,EntityHandle to,std::string& error,
    const std::unordered_map<StableEntityId,StableEntityId>& references)
{
    for(const auto handle:source.Components(from))
    {
        const auto* component=source.GetComponent(handle);if(component->Type()==ComponentTypes::Script)continue;
        std::vector<ComponentProperty> properties;if(!ReadComponentProperties(source,handle,properties,error))return false;
        for(auto& property:properties)if(auto* reference=std::get_if<PropertyEntityReference>(&property.value))
            if(const auto it=references.find(reference->stableId);it!=references.end())reference->stableId=it->second;
        const auto* descriptor=FindComponentDescriptor(component->Type());
        if(!CreateComponentFromProperties(target,to,descriptor->type,descriptor->version,std::move(properties),component->Enabled(),error))return false;
    }
    return true;
}
}
