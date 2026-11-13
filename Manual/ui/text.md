# 字体图集、文本编辑与 DPI

## 测量与绘制共享坐标

[UiTextService](https://github.com/xuu-xuu-xuu/FlareEngine/blob/e7ca801292037b33172d094cc4a434682ac83c9c/Source/FunctionLayer/UI/Interfaces/UiTextService.h#L57) 定义文字测量、命中和光标矩形接口。Windows 实现 [UiDirectWriteTextService](https://github.com/xuu-xuu-xuu/FlareEngine/blob/e7ca801292037b33172d094cc4a434682ac83c9c/Source/PlatformLayer/Windows/UiDirectWriteTextService.h#L17) 使用 DirectWrite 进行排版并缓存字形，通过 [SetRasterScale()](https://github.com/xuu-xuu-xuu/FlareEngine/blob/e7ca801292037b33172d094cc4a434682ac83c9c/Source/PlatformLayer/Windows/UiDirectWriteTextService.h#L59) 按实际 DPI 栅格化图集。[UiStubTextService](https://github.com/xuu-xuu-xuu/FlareEngine/blob/e7ca801292037b33172d094cc4a434682ac83c9c/Source/FunctionLayer/UI/Interfaces/UiTextService.h#L95) 提供固定宽度实现。

## 编辑、选区与输入法

[UiTextEdit](https://github.com/xuu-xuu-xuu/FlareEngine/blob/e7ca801292037b33172d094cc4a434682ac83c9c/Source/FunctionLayer/UI/Widgets/UiTextEdit.h#L17) 维护文本、光标、选区和编辑状态。Win32 实现 [Win32UiPlatformServices](https://github.com/xuu-xuu-xuu/FlareEngine/blob/e7ca801292037b33172d094cc4a434682ac83c9c/Source/PlatformLayer/Windows/Win32UiPlatformServices.h#L13) 派生自 [UiPlatformServices](https://github.com/xuu-xuu-xuu/FlareEngine/blob/e7ca801292037b33172d094cc4a434682ac83c9c/Source/FunctionLayer/UI/Interfaces/UiPlatformServices.h#L12)，负责剪贴板与输入法候选窗口。编辑器通过 [EditorInputAdapter](https://github.com/xuu-xuu-xuu/FlareEngine/blob/e7ca801292037b33172d094cc4a434682ac83c9c/Source/ToolLayer/Editor/EditorUi/EditorInputAdapter.h#L15) 把窗口消息和物理坐标换算到 UI 逻辑坐标。
