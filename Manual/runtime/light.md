# 灯光组件

场景实体可以携带方向光或点光。修改一盏灯的强度，从 [World::GetLight()](world.md#getlight) 读取参数，修改副本，再通过 [World::SetLight()](world.md#setlight) 写回。

## LightRuntimeComponent

[`se::LightRuntimeComponent`](https://github.com/xuu-xuu-xuu/FlareEngine/blob/e7ca801292037b33172d094cc4a434682ac83c9c/Source/FunctionLayer/Runtime/RuntimeComponents.h#L31) 直接继承 [ComponentBase](component-base.md#componentbase)，声明为 `final`。基类记录组件属于哪个实体。派生类通过成员 `value` 保存一份 [LightComponent](#lightcomponent)，由 [World](world.md#world) 的灯光组件池管理。[查看源码](https://github.com/xuu-xuu-xuu/FlareEngine/blob/e7ca801292037b33172d094cc4a434682ac83c9c/Source/FunctionLayer/Runtime/RuntimeComponents.h#L31)

## LightComponent

[`se::LightComponent`](https://github.com/xuu-xuu-xuu/FlareEngine/blob/e7ca801292037b33172d094cc4a434682ac83c9c/Source/FunctionLayer/Runtime/ComponentValues.h#L12) 是传给 [World::SetLight()](world.md#setlight) 的参数结构体。新建时默认得到强度为 1 的白色方向光。[查看源码](https://github.com/xuu-xuu-xuu/FlareEngine/blob/e7ca801292037b33172d094cc4a434682ac83c9c/Source/FunctionLayer/Runtime/ComponentValues.h#L12)

| 字段 | 默认值 | 含义 |
| --- | --- | --- |
| `type` | `Directional` | 光源类型，可选方向光或点光（`Point`） |
| `color` | `{1, 1, 1}` | RGB 光色，各分量均为非负有限数 |
| `intensity` | `1` | 光照强度，允许设为零 |
| `range` | `10` | 光源范围，写入时必须为正的有限数 |

从实体上移除灯光用 [World::RemoveLight()](world.md#removelight)。

灯光写入后，下一次提取帧数据时会计算渲染用的光色和方向。继续读[前向管线与帧图](../render/pipeline.md)或[方向光阴影](../render/shadows.md)。
