#pragma once
#include "MaterialDocument.h"
#include "CoreLayer/PropertyMetadata.h"
#include <span>

namespace se
{
std::span<const PropertyDescriptor> MaterialProperties();
const PropertyDescriptor* FindMaterialProperty(std::string_view key);
bool ReadMaterialProperty(const MaterialDocument& material,std::string_view key,PropertyValue& value,std::string& error);
// 验证完整候选后一次替换，不直接改 AssetStore 或 GPU 资源。
bool WriteMaterialProperty(MaterialDocument& material,std::string_view key,const PropertyValue& value,std::string& error);
}
