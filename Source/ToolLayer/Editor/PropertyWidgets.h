#pragma once
#include "CoreLayer/PropertyMetadata.h"
#include "CoreLayer/Localization.h"

namespace se
{
struct PropertyWidgetResult { bool supported=false,changed=false,activated=false,deactivated=false; };
// 资产选择和自定义复合控件由调用者处理；这里仅生成通用值控件。
PropertyWidgetResult DrawPropertyWidget(const PropertyDescriptor& field,PropertyValue& value,const Localization& text);
}
