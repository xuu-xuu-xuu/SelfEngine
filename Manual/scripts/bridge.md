# 托管宿主与原生 ABI

## 从入口到会话

[Bootstrap 入口](https://github.com/xuu-xuu-xuu/FlareEngine/blob/e7ca801292037b33172d094cc4a434682ac83c9c/Source/FunctionLayer/Scripting/Bootstrap/Program.cs) 解析项目与运行参数，创建 [EngineSession](https://github.com/xuu-xuu-xuu/FlareEngine/blob/e7ca801292037b33172d094cc4a434682ac83c9c/Source/FunctionLayer/Scripting/Managed/EngineSession.cs#L25)。[NativeMethods](https://github.com/xuu-xuu-xuu/FlareEngine/blob/e7ca801292037b33172d094cc4a434682ac83c9c/Source/FunctionLayer/Scripting/Managed/NativeMethods.cs#L6) 定义托管导入，与原生 [se_get_abi_info()](https://github.com/xuu-xuu-xuu/FlareEngine/blob/e7ca801292037b33172d094cc4a434682ac83c9c/Source/FunctionLayer/Scripting/NativeBridge/EngineNative.cpp#L459) 核对 ABI，再经 [se_session_create()](https://github.com/xuu-xuu-xuu/FlareEngine/blob/e7ca801292037b33172d094cc4a434682ac83c9c/Source/FunctionLayer/Scripting/NativeBridge/EngineNative.cpp#L469) 创建会话。宿主通过函数指针接收每帧、事件、场景和编辑器阶段回调。

## 托管 API 的转发位置

| 子系统 | 实现如何跨边界 |
| --- | --- |
| 场景与时间 | [SceneService](https://github.com/xuu-xuu-xuu/FlareEngine/blob/e7ca801292037b33172d094cc4a434682ac83c9c/Source/FunctionLayer/Scripting/Managed/Scenes.cs#L42) 管理场景句柄，[TimeOfDayService](https://github.com/xuu-xuu-xuu/FlareEngine/blob/e7ca801292037b33172d094cc4a434682ac83c9c/Source/FunctionLayer/Scripting/Managed/TimeOfDay.cs#L14) 获取环境快照，按原生时钟设置时刻与倍率。 |
| 物理与输入 | [RigidBody](https://github.com/xuu-xuu-xuu/FlareEngine/blob/e7ca801292037b33172d094cc4a434682ac83c9c/Source/FunctionLayer/Scripting/Managed/Physics.cs#L38) 按句柄访问刚体，[ScriptInput](https://github.com/xuu-xuu-xuu/FlareEngine/blob/e7ca801292037b33172d094cc4a434682ac83c9c/Source/FunctionLayer/Scripting/Managed/Behaviour.cs#L75) 把按下、抬起和消耗状态提供给当前脚本。 |
| 游戏界面 | [GameUi](https://github.com/xuu-xuu-xuu/FlareEngine/blob/e7ca801292037b33172d094cc4a434682ac83c9c/Source/FunctionLayer/Scripting/Managed/GameUi.cs#L87) 向原生树提交控件操作，托管元素代理随会话和界面状态失效。 |
| 性能采集 | [SetWindowProfile()](https://github.com/xuu-xuu-xuu/FlareEngine/blob/e7ca801292037b33172d094cc4a434682ac83c9c/Source/FunctionLayer/Scripting/Managed/WindowProfiling.cs#L9) 把采集设置交给原生窗口渲染器。 |

## 运行诊断

[BootstrapDiagnostics](https://github.com/xuu-xuu-xuu/FlareEngine/blob/e7ca801292037b33172d094cc4a434682ac83c9c/Source/FunctionLayer/Scripting/Bootstrap/BootstrapDiagnostics.cs#L4) 记录托管启动阶段。显式预检通过 [Verify()](https://github.com/xuu-xuu-xuu/FlareEngine/blob/e7ca801292037b33172d094cc4a434682ac83c9c/Source/FunctionLayer/Scripting/Bootstrap/RuntimePreflight.cs#L8) 实际读写管道、序列化数据并检查程序集加载位置。预览目录由 [PreviewLease](https://github.com/xuu-xuu-xuu/FlareEngine/blob/e7ca801292037b33172d094cc4a434682ac83c9c/Source/FunctionLayer/Scripting/Bootstrap/PreviewLease.cs#L3) 持有文件锁，限制相互冲突的预览会话。

托管行为还可以用 [TickDependency](https://github.com/xuu-xuu-xuu/FlareEngine/blob/e7ca801292037b33172d094cc4a434682ac83c9c/Source/FunctionLayer/Scripting/Managed/EnvironmentScheduling.cs#L7) 声明阶段前置关系。[调度器](../runtime/scheduling.md#tickscheduler) 在原生世界按帧或模拟时钟执行这些依赖。
