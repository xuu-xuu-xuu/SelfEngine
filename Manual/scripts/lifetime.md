# 脚本反射、程序集卸载与 GC

## 字段怎样进入属性面板

[ExposeAttribute](https://github.com/xuu-xuu-xuu/FlareEngine/blob/e7ca801292037b33172d094cc4a434682ac83c9c/Source/FunctionLayer/Scripting/Managed/Behaviour.cs#L9) 标记需要暴露的实例字段。[ScriptFields](https://github.com/xuu-xuu-xuu/FlareEngine/blob/e7ca801292037b33172d094cc4a434682ac83c9c/Source/FunctionLayer/Scripting/Managed/ScriptFields.cs#L10) 用 .NET 反射枚举公开和非公开字段，筛选带标记、可写的实例字段。[Describe()](https://github.com/xuu-xuu-xuu/FlareEngine/blob/e7ca801292037b33172d094cc4a434682ac83c9c/Source/FunctionLayer/Scripting/Managed/ScriptFields.cs#L44) 生成字段描述，[Save()](https://github.com/xuu-xuu-xuu/FlareEngine/blob/e7ca801292037b33172d094cc4a434682ac83c9c/Source/FunctionLayer/Scripting/Managed/ScriptFields.cs#L48) 与 [Load()](https://github.com/xuu-xuu-xuu/FlareEngine/blob/e7ca801292037b33172d094cc4a434682ac83c9c/Source/FunctionLayer/Scripting/Managed/ScriptFields.cs#L69) 在可支持的字段值和 JSON 之间转换。C++ 组件对应[原生元数据](../runtime/metadata.md)。

## ScriptProject

[ScriptProject](https://github.com/xuu-xuu-xuu/FlareEngine/blob/e7ca801292037b33172d094cc4a434682ac83c9c/Source/FunctionLayer/Scripting/Managed/ScriptProject.cs#L7) 为项目建立可卸载的 [ProjectContext](https://github.com/xuu-xuu-xuu/FlareEngine/blob/e7ca801292037b33172d094cc4a434682ac83c9c/Source/FunctionLayer/Scripting/Managed/ScriptProject.cs#L9)。启动时加载候选程序集并解析允许的行为类型，停止时解除脚本引用并请求卸载上下文。托管 GC 在引用释放后回收对象，卸载请求与完成回收之间可以相隔多个 GC 周期。

## 原生对象由谁释放

[EngineSession](https://github.com/xuu-xuu-xuu/FlareEngine/blob/e7ca801292037b33172d094cc4a434682ac83c9c/Source/FunctionLayer/Scripting/Managed/EngineSession.cs#L25) 为回调分配弱 GC 句柄，通过 [NativeSessionHandle](https://github.com/xuu-xuu-xuu/FlareEngine/blob/e7ca801292037b33172d094cc4a434682ac83c9c/Source/FunctionLayer/Scripting/Managed/NativeMethods.cs#L102) 持有原生会话。会话的 [Dispose()](https://github.com/xuu-xuu-xuu/FlareEngine/blob/e7ca801292037b33172d094cc4a434682ac83c9c/Source/FunctionLayer/Scripting/Managed/EngineSession.cs#L161) 走显式关闭路径，句柄的 [ReleaseHandle()](https://github.com/xuu-xuu-xuu/FlareEngine/blob/e7ca801292037b33172d094cc4a434682ac83c9c/Source/FunctionLayer/Scripting/Managed/NativeMethods.cs#L107) 收尾。实体和组件由[原生世界](../runtime/entities.md)管理，GPU 资源另受[提交 Fence](../render/device.md)约束。
