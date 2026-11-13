# 实体、层级与组件存储

## 运行句柄和稳定 ID

[World](world.md#world) 使用槽位保存实体。[EntityHandle](https://github.com/xuu-xuu-xuu/FlareEngine/blob/e7ca801292037b33172d094cc4a434682ac83c9c/Source/FunctionLayer/Runtime/RuntimeHandles.h#L6) 包含索引和代数，复用槽位后旧句柄会失效。场景文件和撤销记录使用稳定 ID，载入后经 [FindByStableId()](https://github.com/xuu-xuu-xuu/FlareEngine/blob/e7ca801292037b33172d094cc4a434682ac83c9c/Source/FunctionLayer/Runtime/World.h#L66) 解析回当前句柄。

## 层级变换

[SetParent()](https://github.com/xuu-xuu-xuu/FlareEngine/blob/e7ca801292037b33172d094cc4a434682ac83c9c/Source/FunctionLayer/Runtime/World.h#L79) 更新父子关系，[GetWorldMatrix()](https://github.com/xuu-xuu-xuu/FlareEngine/blob/e7ca801292037b33172d094cc4a434682ac83c9c/Source/FunctionLayer/Runtime/World.h#L80) 组合层级上的局部变换。编辑器的重挂接操作还可以保留世界变换，它通过 [TryDecomposeTrs()](https://github.com/xuu-xuu-xuu/FlareEngine/blob/e7ca801292037b33172d094cc4a434682ac83c9c/Source/CoreLayer/Math/TransformDecomposition.h#L9) 把新的局部矩阵还原为平移、旋转和缩放。

## 组件存储与安全修改

内置组件保存在按实体索引访问的类型池中。扩展组件通过 [AttachRegisteredComponent()](https://github.com/xuu-xuu-xuu/FlareEngine/blob/e7ca801292037b33172d094cc4a434682ac83c9c/Source/FunctionLayer/Runtime/World.h#L104) 写入派生自 [RegisteredComponentPool](https://github.com/xuu-xuu-xuu/FlareEngine/blob/e7ca801292037b33172d094cc4a434682ac83c9c/Source/FunctionLayer/Runtime/RegisteredComponentPool.h#L10) 的 [TypedRegisteredComponentPool](https://github.com/xuu-xuu-xuu/FlareEngine/blob/e7ca801292037b33172d094cc4a434682ac83c9c/Source/FunctionLayer/Runtime/RegisteredComponentPool.h#L23)。组件身份与属性分开维护，详见 [ComponentBase](component-base.md#componentbase)。帧内或回调中的结构变更可以排队，在安全阶段由 [FlushCommands()](https://github.com/xuu-xuu-xuu/FlareEngine/blob/e7ca801292037b33172d094cc4a434682ac83c9c/Source/FunctionLayer/Runtime/World.h#L144) 提交。

销毁子树时，[World::Destroy()](https://github.com/xuu-xuu-xuu/FlareEngine/blob/e7ca801292037b33172d094cc4a434682ac83c9c/Source/FunctionLayer/Runtime/World.cpp#L229) 先让实体逻辑失效，生命周期回调与存储释放留到安全边界。
