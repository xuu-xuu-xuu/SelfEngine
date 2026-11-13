# 昼夜、天空与场景环境

## 配置与采样

[TODProfile](https://github.com/xuu-xuu-xuu/FlareEngine/blob/e7ca801292037b33172d094cc4a434682ac83c9c/Source/ResourceLayer/Environment/TODProfile.h#L17) 保存一天中的关键帧，[SampleTODProfile()](https://github.com/xuu-xuu-xuu/FlareEngine/blob/e7ca801292037b33172d094cc4a434682ac83c9c/Source/ResourceLayer/Environment/TODProfile.cpp#L68) 按时间插值环境参数。[TODSystem](https://github.com/xuu-xuu-xuu/FlareEngine/blob/e7ca801292037b33172d094cc4a434682ac83c9c/Source/FunctionLayer/Runtime/TODSystem.h#L20) 记录日数、时刻和运行倍率，用 [Advance()](https://github.com/xuu-xuu-xuu/FlareEngine/blob/e7ca801292037b33172d094cc4a434682ac83c9c/Source/FunctionLayer/Runtime/TODSystem.cpp#L75) 推进模拟并产生昼夜边界事件。

## 渲染与编辑器共享的快照

[Snapshot()](https://github.com/xuu-xuu-xuu/FlareEngine/blob/e7ca801292037b33172d094cc4a434682ac83c9c/Source/FunctionLayer/Runtime/TODSystem.cpp#L86) 输出可插值的 [EnvironmentSnapshot](https://github.com/xuu-xuu-xuu/FlareEngine/blob/e7ca801292037b33172d094cc4a434682ac83c9c/Source/FunctionLayer/Runtime/TODSystem.h#L12)。构建帧数据时提取太阳方向、光照和天空参数，编辑器的 [TODPreview](https://github.com/xuu-xuu-xuu/FlareEngine/blob/e7ca801292037b33172d094cc4a434682ac83c9c/Source/ToolLayer/Editor/TODPreview.h#L8) 可以单独设置预览时刻和暂停状态。C# 场景通过 [TimeOfDayService](https://github.com/xuu-xuu-xuu/FlareEngine/blob/e7ca801292037b33172d094cc4a434682ac83c9c/Source/FunctionLayer/Scripting/Managed/TimeOfDay.cs#L14) 修改时间或倍率。
