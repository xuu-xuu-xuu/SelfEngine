#pragma once
#include "CoreLayer/Math/Math.h"
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <variant>
#include <vector>

namespace se
{
enum class PropertyType { Boolean, Number, Vector3, Quaternion, AssetId, AssetList, Text, Vector4, EntityReference };
enum class PropertyPresentation { Default, Color, Slider };
// 持久引用只保存稳定 ID；运行时句柄始终由所属 World 解析。
struct PropertyEntityReference {std::uint64_t stableId=0;bool operator==(const PropertyEntityReference&)const=default;};
using PropertyValue=std::variant<bool,float,Vec3,Quat,std::string,std::vector<std::string>,Vec4,PropertyEntityReference>;
struct PropertyDescriptor
{
    std::string_view key,labelKey;
    PropertyType type;
    PropertyValue defaultValue;
    std::optional<float> minimum,maximum;
    bool editable=true;
    // 空串使用 key 的末段；材质可使用 textures/baseColor 这样的相对 JSON 路径。
    std::string_view serializationKey;
    PropertyPresentation presentation=PropertyPresentation::Default;
    bool serializedOptional=false;
    bool serialized=true;
};
inline std::string_view SerializedPropertyKey(const PropertyDescriptor& field)
{
    if(!field.serializationKey.empty())return field.serializationKey;
    const auto dot=field.key.find_last_of('.');return dot==std::string_view::npos?field.key:field.key.substr(dot+1);
}
// 通用类型/数值约束；实体、资源 ID 和跨字段约束由拥有者的候选写入继续验证。
inline bool ValidatePropertyValue(const PropertyDescriptor& field,const PropertyValue& value)
{
    if(value.index()!=field.defaultValue.index())return false;
    const auto number=[&](float n){return IsFinite(n)&&(!field.minimum||n>=*field.minimum)&&(!field.maximum||n<=*field.maximum);};
    if(const auto* n=std::get_if<float>(&value))return number(*n);
    if(const auto* v=std::get_if<Vec3>(&value))return number(v->x)&&number(v->y)&&number(v->z);
    if(const auto* v=std::get_if<Vec4>(&value))return number(v->x)&&number(v->y)&&number(v->z)&&number(v->w);
    if(const auto* q=std::get_if<Quat>(&value))return IsFinite(*q)&&IsFinite(LengthSquared(*q))&&LengthSquared(*q)>1e-12f;
    return true;
}
}
