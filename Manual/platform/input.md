# 输入、文字事件与窗口适配

## 持续状态与单帧事件

[InputState](https://github.com/xuu-xuu-xuu/FlareEngine/blob/e7ca801292037b33172d094cc4a434682ac83c9c/Source/PlatformLayer/InputState.h#L8) 同时保存持续按键、焦点和本轮事件。平台收到键盘、文本或指针消息时更新对应记录。所有消费者读取后调用 [ClearEvents()](https://github.com/xuu-xuu-xuu/FlareEngine/blob/e7ca801292037b33172d094cc4a434682ac83c9c/Source/PlatformLayer/InputState.h#L22) 清空事件，按住状态延续到下一帧。[UpdateFrameTimer()](https://github.com/xuu-xuu-xuu/FlareEngine/blob/e7ca801292037b33172d094cc4a434682ac83c9c/Source/PlatformLayer/Windows/FrameTimer.cpp#L21) 记录计时，[SandboxDiagnostics](https://github.com/xuu-xuu-xuu/FlareEngine/blob/e7ca801292037b33172d094cc4a434682ac83c9c/Source/FunctionLayer/Application/SandboxDiagnostics.h#L8) 观察输入和帧耗时。

## 游戏控件与操作系统

[WindowsUiInput](https://github.com/xuu-xuu-xuu/FlareEngine/blob/e7ca801292037b33172d094cc4a434682ac83c9c/Source/PlatformLayer/Windows/WindowsUiInput.h#L23) 把 Win32 输入、捕获和 IME 组合串接到游戏控件树。[MakeUiSliderGeometry()](https://github.com/xuu-xuu-xuu/FlareEngine/blob/e7ca801292037b33172d094cc4a434682ac83c9c/Source/FunctionLayer/GameUI/UiControlGeometry.cpp#L8) 计算滑条的绘制和命中位置，[PrepareUiText()](https://github.com/xuu-xuu-xuu/FlareEngine/blob/e7ca801292037b33172d094cc4a434682ac83c9c/Source/FunctionLayer/GameUI/UiTextPresentation.cpp#L12) 整理可显示的文本与编辑状态。编辑器中的 ImGui 路径经独立的 [WindowsEditorInput](https://github.com/xuu-xuu-xuu/FlareEngine/blob/e7ca801292037b33172d094cc4a434682ac83c9c/Source/ToolLayer/Editor/WindowsEditorInput.h#L10) 准备帧输入。

## 从数学数据到 GPU

[Quat](https://github.com/xuu-xuu-xuu/FlareEngine/blob/e7ca801292037b33172d094cc4a434682ac83c9c/Source/CoreLayer/Math/Math.h#L34) 表示旋转，[Mat4](https://github.com/xuu-xuu-xuu/FlareEngine/blob/e7ca801292037b33172d094cc4a434682ac83c9c/Source/CoreLayer/Math/Math.h#L68) 表示变换，[FloatToHalf()](https://github.com/xuu-xuu-xuu/FlareEngine/blob/e7ca801292037b33172d094cc4a434682ac83c9c/Source/CoreLayer/Math/Half.h#L8) 将浮点数压缩为半精度。[GuiGeometry](https://github.com/xuu-xuu-xuu/FlareEngine/blob/e7ca801292037b33172d094cc4a434682ac83c9c/Source/CoreLayer/GuiGeometry.h#L18) 共用顶点、索引和绘制命令的表示，界面可以先完成布局再交给渲染层。
