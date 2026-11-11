#include "PropertyWidgets.h"
#include <imgui/imgui.h>
#include <algorithm>
#include <array>
#include <limits>

namespace se
{
PropertyWidgetResult DrawPropertyWidget(const PropertyDescriptor& field,PropertyValue& value,const Localization& text)
{
    PropertyWidgetResult result;
    if(value.index()!=field.defaultValue.index()||field.type==PropertyType::AssetId||field.type==PropertyType::AssetList||field.type==PropertyType::EntityReference)return result;
    const auto label=text.GetText(std::string(field.labelKey))+"###"+std::string(field.key);
    result.supported=true;
    const float low=field.minimum.value_or(field.maximum?-(std::numeric_limits<float>::max)():0.0f);
    const float high=field.maximum.value_or(field.minimum?(std::numeric_limits<float>::max)():0.0f);
    ImGui::BeginDisabled(!field.editable);
    switch(field.type)
    {
    case PropertyType::Boolean:result.changed=ImGui::Checkbox(label.c_str(),&std::get<bool>(value));break;
    case PropertyType::Number:
        if(field.presentation==PropertyPresentation::Slider&&field.minimum&&field.maximum)
            result.changed=ImGui::SliderFloat(label.c_str(),&std::get<float>(value),low,high);
        else result.changed=ImGui::DragFloat(label.c_str(),&std::get<float>(value),.01f,low,high);
        break;
    case PropertyType::Vector3:
        if(field.presentation==PropertyPresentation::Color)
            result.changed=ImGui::ColorEdit3(label.c_str(),&std::get<Vec3>(value).x,field.maximum?0:ImGuiColorEditFlags_HDR|ImGuiColorEditFlags_Float);
        else result.changed=ImGui::DragFloat3(label.c_str(),&std::get<Vec3>(value).x,.02f,low,high);
        break;
    case PropertyType::Quaternion:result.changed=ImGui::DragFloat4(label.c_str(),&std::get<Quat>(value).x,.005f,-1,1);break;
    case PropertyType::Vector4:
        if(field.presentation==PropertyPresentation::Color)result.changed=ImGui::ColorEdit4(label.c_str(),&std::get<Vec4>(value).x);
        else result.changed=ImGui::DragFloat4(label.c_str(),&std::get<Vec4>(value).x,.01f,low,high);
        break;
    case PropertyType::Text:{
        std::array<char,4097> buffer{};const auto& current=std::get<std::string>(value);
        std::copy_n(current.data(),std::min(current.size(),buffer.size()-1),buffer.data());
        result.changed=ImGui::InputText(label.c_str(),buffer.data(),buffer.size());if(result.changed)value=std::string(buffer.data());break;}
    case PropertyType::AssetId:case PropertyType::AssetList:case PropertyType::EntityReference:break;
    }
    result.activated=ImGui::IsItemActivated();result.deactivated=ImGui::IsItemDeactivated();
    ImGui::EndDisabled();return result;
}
}
