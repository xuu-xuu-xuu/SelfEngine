# 组件基类与生命周期

## ComponentBase

[`se::ComponentBase`](https://github.com/xuu-xuu-xuu/FlareEngine/blob/e7ca801292037b33172d094cc4a434682ac83c9c/Source/FunctionLayer/Runtime/ComponentBase.h#L10) 是原生运行时组件的共同基类。[World](world.md#world) 维护组件的身份和状态，调用方通过只读接口访问它们。复制组件的属性时，副本不会继承原组件的运行身份。[查看源码](https://github.com/xuu-xuu-xuu/FlareEngine/blob/e7ca801292037b33172d094cc4a434682ac83c9c/Source/FunctionLayer/Runtime/ComponentBase.h#L10)

| 想知道什么 | 公开接口 |
| --- | --- |
| 属于哪个实体，怎样标识该组件？ | [Owner()](https://github.com/xuu-xuu-xuu/FlareEngine/blob/e7ca801292037b33172d094cc4a434682ac83c9c/Source/FunctionLayer/Runtime/ComponentBase.h#L20) 返回实体句柄，[Handle()](https://github.com/xuu-xuu-xuu/FlareEngine/blob/e7ca801292037b33172d094cc4a434682ac83c9c/Source/FunctionLayer/Runtime/ComponentBase.h#L21) 返回组件句柄 |
| 属于哪一种组件？ | [Type()](https://github.com/xuu-xuu-xuu/FlareEngine/blob/e7ca801292037b33172d094cc4a434682ac83c9c/Source/FunctionLayer/Runtime/ComponentBase.h#L22) 返回组件类型 ID |
| 是否启用，当前是否活跃？ | [Enabled()](https://github.com/xuu-xuu-xuu/FlareEngine/blob/e7ca801292037b33172d094cc4a434682ac83c9c/Source/FunctionLayer/Runtime/ComponentBase.h#L23)、[Active()](https://github.com/xuu-xuu-xuu/FlareEngine/blob/e7ca801292037b33172d094cc4a434682ac83c9c/Source/FunctionLayer/Runtime/ComponentBase.h#L24) |
| 正处于哪个生命周期阶段？ | [Lifecycle()](https://github.com/xuu-xuu-xuu/FlareEngine/blob/e7ca801292037b33172d094cc4a434682ac83c9c/Source/FunctionLayer/Runtime/ComponentBase.h#L25) 返回挂接、活跃或移除等阶段 |

### 直接派生类

[LightRuntimeComponent](light.md#lightruntimecomponent) 保存灯光参数。其余四种分别是保存变换的 [TransformComponent](https://github.com/xuu-xuu-xuu/FlareEngine/blob/e7ca801292037b33172d094cc4a434682ac83c9c/Source/FunctionLayer/Runtime/RuntimeComponents.h#L9)、相机的 [CameraRuntimeComponent](https://github.com/xuu-xuu-xuu/FlareEngine/blob/e7ca801292037b33172d094cc4a434682ac83c9c/Source/FunctionLayer/Runtime/RuntimeComponents.h#L17)、网格的 [MeshRuntimeComponent](https://github.com/xuu-xuu-xuu/FlareEngine/blob/e7ca801292037b33172d094cc4a434682ac83c9c/Source/FunctionLayer/Runtime/RuntimeComponents.h#L24) 和接收生命周期回调的 [ScriptComponent](https://github.com/xuu-xuu-xuu/FlareEngine/blob/e7ca801292037b33172d094cc4a434682ac83c9c/Source/FunctionLayer/Runtime/RuntimeComponents.h#L38)。这五个类都声明为 `final`。

### 生命周期回调

派生类可以覆写受保护的 [`OnAttach`](https://github.com/xuu-xuu-xuu/FlareEngine/blob/e7ca801292037b33172d094cc4a434682ac83c9c/Source/FunctionLayer/Runtime/ComponentBase.h#L27)、[`OnEnable`](https://github.com/xuu-xuu-xuu/FlareEngine/blob/e7ca801292037b33172d094cc4a434682ac83c9c/Source/FunctionLayer/Runtime/ComponentBase.h#L28)、[`OnDisable`](https://github.com/xuu-xuu-xuu/FlareEngine/blob/e7ca801292037b33172d094cc4a434682ac83c9c/Source/FunctionLayer/Runtime/ComponentBase.h#L29) 和 [`OnDetach`](https://github.com/xuu-xuu-xuu/FlareEngine/blob/e7ca801292037b33172d094cc4a434682ac83c9c/Source/FunctionLayer/Runtime/ComponentBase.h#L30)。例如，脚本组件覆写这四个方法，把相应事件转交给注册的回调。[查看脚本组件实现](https://github.com/xuu-xuu-xuu/FlareEngine/blob/e7ca801292037b33172d094cc4a434682ac83c9c/Source/FunctionLayer/Runtime/RuntimeComponents.h#L46)
