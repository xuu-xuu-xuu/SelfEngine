# 原生组件反射与属性序列化

## 类型从哪里来

原生反射由显式登记的元数据实现。[ComponentDescriptor](https://github.com/xuu-xuu-xuu/FlareEngine/blob/e7ca801292037b33172d094cc4a434682ac83c9c/Source/FunctionLayer/Runtime/ComponentMetadata.h#L11) 描述组件类型、版本及属性操作，[RegisterComponent()](https://github.com/xuu-xuu-xuu/FlareEngine/blob/e7ca801292037b33172d094cc4a434682ac83c9c/Source/FunctionLayer/Runtime/ComponentMetadata.cpp#L168) 登记描述符。[PropertyDescriptor](https://github.com/xuu-xuu-xuu/FlareEngine/blob/e7ca801292037b33172d094cc4a434682ac83c9c/Source/CoreLayer/PropertyMetadata.h#L17) 记录属性键、值类型和展示方式，供属性面板、存档与克隆共用。

## 怎样读写和复制

[ReadProperty()](https://github.com/xuu-xuu-xuu/FlareEngine/blob/e7ca801292037b33172d094cc4a434682ac83c9c/Source/FunctionLayer/Runtime/ComponentMetadata.cpp#L190) 按属性键读取值。[WriteProperty()](https://github.com/xuu-xuu-xuu/FlareEngine/blob/e7ca801292037b33172d094cc4a434682ac83c9c/Source/FunctionLayer/Runtime/ComponentMetadata.cpp#L198) 通过世界的组件写入接口校验并提交候选值。[CloneComponentProperties()](https://github.com/xuu-xuu-xuu/FlareEngine/blob/e7ca801292037b33172d094cc4a434682ac83c9c/Source/FunctionLayer/Runtime/ComponentMetadata.cpp#L266) 复制组件时按稳定 ID 映射内部实体引用，保留外部引用。材质使用对应的 [WriteMaterialProperty()](https://github.com/xuu-xuu-xuu/FlareEngine/blob/e7ca801292037b33172d094cc4a434682ac83c9c/Source/ResourceLayer/Assets/MaterialMetadata.cpp#L38)。托管脚本的字段发现见[脚本反射与寿命](../scripts/lifetime.md)。

登记时描述符和字符串必须具有进程寿命。再次登记可能改变描述符存储位置，使用方应在操作时重新查询。
