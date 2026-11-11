#pragma once
#include "CoreLayer/PropertyMetadata.h"
#include <nlohmann/json.hpp>
#include <limits>
#include <type_traits>
#include <charconv>

namespace se
{
inline nlohmann::json WritePropertyJson(const PropertyValue& value)
{
    return std::visit([](const auto& v)->nlohmann::json{
        using T=std::decay_t<decltype(v)>;
        if constexpr(std::is_same_v<T,Vec3>)return nlohmann::json::array({v.x,v.y,v.z});
        else if constexpr(std::is_same_v<T,Quat>||std::is_same_v<T,Vec4>)return nlohmann::json::array({v.x,v.y,v.z,v.w});
        else if constexpr(std::is_same_v<T,PropertyEntityReference>)return std::to_string(v.stableId);
        else return v;
    },value);
}
inline bool ReadPropertyJson(const nlohmann::json& json,const PropertyDescriptor& field,PropertyValue& output)
{
    try
    {
        PropertyValue value=field.defaultValue;
        const auto number=[](const nlohmann::json& item,float& result){
            if(!item.is_number())return false;const auto d=item.get<double>();
            if(!std::isfinite(d)||std::abs(d)>(std::numeric_limits<float>::max)())return false;
            result=static_cast<float>(d);return true;
        };
        switch(field.type)
        {
        case PropertyType::Boolean:if(!json.is_boolean())return false;value=json.get<bool>();break;
        case PropertyType::Number:{float n;if(!number(json,n))return false;value=n;break;}
        case PropertyType::Vector3:{Vec3 v;if(!json.is_array()||json.size()!=3||!number(json[0],v.x)||!number(json[1],v.y)||!number(json[2],v.z))return false;value=v;break;}
        case PropertyType::Quaternion:case PropertyType::Vector4:{
            Vec4 v;if(!json.is_array()||json.size()!=4||!number(json[0],v.x)||!number(json[1],v.y)||!number(json[2],v.z)||!number(json[3],v.w))return false;
            if(field.type==PropertyType::Quaternion)value=Quat{v.x,v.y,v.z,v.w};else value=v;break;}
        case PropertyType::Text:case PropertyType::AssetId:if(!json.is_string())return false;value=json.get<std::string>();break;
        case PropertyType::AssetList:if(!json.is_array()||json.size()>1024)return false;
            for(const auto& item:json)if(!item.is_string())return false;value=json.get<std::vector<std::string>>();break;
        case PropertyType::EntityReference:{
            if(json.is_null()){value=PropertyEntityReference{};break;}
            if(!json.is_string())return false;
            const auto& text=json.get_ref<const std::string&>();std::uint64_t id=0;
            const auto parsed=std::from_chars(text.data(),text.data()+text.size(),id);
            if(parsed.ec!=std::errc{}||parsed.ptr!=text.data()+text.size())return false;
            value=PropertyEntityReference{id};break;}
        }
        if(!ValidatePropertyValue(field,value))return false;output=std::move(value);return true;
    }
    catch(const std::exception&){return false;}
}
}
