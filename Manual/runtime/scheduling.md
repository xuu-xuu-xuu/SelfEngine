# 主循环、任务顺序与事件

## 帧与固定步

[Environment](https://github.com/xuu-xuu-xuu/FlareEngine/blob/e7ca801292037b33172d094cc4a434682ac83c9c/Source/FunctionLayer/Runtime/Environment.h#L20) 持有世界、时钟和阶段调度器。[RunFrame()](https://github.com/xuu-xuu-xuu/FlareEngine/blob/e7ca801292037b33172d094cc4a434682ac83c9c/Source/FunctionLayer/Runtime/Environment.h#L56) 输入实际帧间隔，将物理等固定步与按帧更新穿插执行。[FixedStepClock](https://github.com/xuu-xuu-xuu/FlareEngine/blob/e7ca801292037b33172d094cc4a434682ac83c9c/Source/CoreLayer/FixedStepClock.h#L11) 累积余量并限制单帧追赶步数，[StepFrame()](https://github.com/xuu-xuu-xuu/FlareEngine/blob/e7ca801292037b33172d094cc4a434682ac83c9c/Source/FunctionLayer/Runtime/Environment.h#L58) 提供暂停时的单步入口。

## TickScheduler

[TickScheduler](https://github.com/xuu-xuu-xuu/FlareEngine/blob/e7ca801292037b33172d094cc4a434682ac83c9c/Source/FunctionLayer/Runtime/TickScheduler.h#L31) 按阶段、时间域和前置任务安排回调。[SetPrerequisites()](https://github.com/xuu-xuu-xuu/FlareEngine/blob/e7ca801292037b33172d094cc4a434682ac83c9c/Source/FunctionLayer/Runtime/TickScheduler.cpp#L75) 更新依赖，[RunStage()](https://github.com/xuu-xuu-xuu/FlareEngine/blob/e7ca801292037b33172d094cc4a434682ac83c9c/Source/FunctionLayer/Runtime/TickScheduler.cpp#L105) 执行当前阶段。依赖可以只约束顺序，也可以要求前置任务的结果在本轮已更新。

## EventBus

[EventBus](https://github.com/xuu-xuu-xuu/FlareEngine/blob/e7ca801292037b33172d094cc4a434682ac83c9c/Source/FunctionLayer/Runtime/EventBus.h#L37) 用类型描述符登记载荷格式，发布端将消息送入邮箱。[Subscribe()](https://github.com/xuu-xuu-xuu/FlareEngine/blob/e7ca801292037b33172d094cc4a434682ac83c9c/Source/FunctionLayer/Runtime/EventBus.h#L80) 记录订阅，[Drain()](https://github.com/xuu-xuu-xuu/FlareEngine/blob/e7ca801292037b33172d094cc4a434682ac83c9c/Source/FunctionLayer/Runtime/EventBus.h#L87) 在指定阶段取出消息并分发。跨线程投递与主线程回调之间用有容量限制的 [BoundedMailbox](https://github.com/xuu-xuu-xuu/FlareEngine/blob/e7ca801292037b33172d094cc4a434682ac83c9c/Source/CoreLayer/BoundedMailbox.h#L20) 交接。
