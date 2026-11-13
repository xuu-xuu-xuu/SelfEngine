# World 的灯光接口

## World

[`se::World`](https://github.com/xuu-xuu-xuu/FlareEngine/blob/e7ca801292037b33172d094cc4a434682ac83c9c/Source/FunctionLayer/Runtime/World.h#L46) 管理实体和组件。运行时用 [LightRuntimeComponent](light.md#lightruntimecomponent) 保存组件身份与灯光参数，调用方只需提供实体句柄和 [LightComponent](light.md#lightcomponent)。[查看类定义](https://github.com/xuu-xuu-xuu/FlareEngine/blob/e7ca801292037b33172d094cc4a434682ac83c9c/Source/FunctionLayer/Runtime/World.h#L46)

### GetLight

[`const LightComponent* GetLight(EntityHandle entity) const`](https://github.com/xuu-xuu-xuu/FlareEngine/blob/e7ca801292037b33172d094cc4a434682ac83c9c/Source/FunctionLayer/Runtime/World.h#L92) 返回只读参数。实体已失效或没有灯光组件时返回 `nullptr`。[查看源码](https://github.com/xuu-xuu-xuu/FlareEngine/blob/e7ca801292037b33172d094cc4a434682ac83c9c/Source/FunctionLayer/Runtime/World.cpp#L330)

### SetLight

[`bool SetLight(EntityHandle entity, const LightComponent& component)`](https://github.com/xuu-xuu-xuu/FlareEngine/blob/e7ca801292037b33172d094cc4a434682ac83c9c/Source/FunctionLayer/Runtime/World.h#L93) 给实体添加或更新灯光。未知实体、无效光源类型、非有限值、负的颜色或强度、非正范围都会使调用返回 `false`。结构修改在需要时进入命令队列，返回成功时可能尚未反映在同一帧的读取中。[查看源码](https://github.com/xuu-xuu-xuu/FlareEngine/blob/e7ca801292037b33172d094cc4a434682ac83c9c/Source/FunctionLayer/Runtime/World.cpp#L331)

下面的函数修改一盏已有灯的强度，并把写入结果交给调用方。

```cpp
bool SetIntensity(se::World& world, se::EntityHandle entity, float intensity)
{
    const auto* current = world.GetLight(entity);
    if (!current) return false;

    auto light = *current;
    light.intensity = intensity;
    return world.SetLight(entity, light);
}
```

### RemoveLight

[`bool RemoveLight(EntityHandle entity)`](https://github.com/xuu-xuu-xuu/FlareEngine/blob/e7ca801292037b33172d094cc4a434682ac83c9c/Source/FunctionLayer/Runtime/World.h#L94) 移除指定实体的灯光组件。有效实体原本就没有灯光时，移除操作也可以成功。未知实体返回 `false`。[查看源码](https://github.com/xuu-xuu-xuu/FlareEngine/blob/e7ca801292037b33172d094cc4a434682ac83c9c/Source/FunctionLayer/Runtime/World.cpp#L339)
